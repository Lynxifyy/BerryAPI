// Hot reload with state handoff + error overlay. See include/berry/reload.h.
#include "berry/reload.h"
#include "berry/event.h"    // NowNs
#include "berry/logger.h"
#include "berry/manifest.h" // crypto::Sha256Hex
#include "berry/render.h"
#include "berry/thread.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace Berry {

//----------------------------------------------------------------------------
// ScriptErrorOverlay
//----------------------------------------------------------------------------

void ScriptErrorOverlay::Set(const std::string& script, const ScriptError& error) {
  std::lock_guard<std::mutex> lk(mutex_);
  visible_ = true;
  script_ = script;
  error_ = error;
}

void ScriptErrorOverlay::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  visible_ = false;
  script_.clear();
  error_ = ScriptError::None();
}

bool ScriptErrorOverlay::visible() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return visible_;
}

std::string ScriptErrorOverlay::script() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return script_;
}

ScriptError ScriptErrorOverlay::error() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return error_;
}

std::vector<OverlayLine> ScriptErrorOverlay::Lines() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<OverlayLine> out;
  if (!visible_) return out;

  out.push_back({"[Berry] script error: " + script_, true});
  std::string where = error_.chunk.empty() ? "<script>" : error_.chunk;
  if (error_.line > 0) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), ":%d", (int)error_.line);
    where += buf;
  }
  out.push_back({where + ": " + error_.message, false});
  out.push_back({"(previous revision still running — fix and save)", false});
  return out;
}

void DrawErrorOverlay(Renderer& renderer, const ScriptErrorOverlay& overlay,
                      const Vec2& screenSize) {
  auto lines = overlay.Lines();
  if (lines.empty()) return;
  f32 y = 8;
  for (const auto& line : lines) {
    Rgba color = line.isError ? Rgba(255, 90, 90) : Rgba(255, 230, 160);
    renderer.DrawText(Vec2(8, y), line.text.c_str(), color, 1.0f);
    y += 14;
  }
  (void)screenSize;
}

//----------------------------------------------------------------------------
// FileScriptSource
//----------------------------------------------------------------------------

bool FileScriptSource::Read(std::string* source, u64* revision) {
  std::ifstream in(path_, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  if (in.bad()) return false;
  std::string content = ss.str();
  if (source) *source = content;
  if (revision) {
    // Content hash: touching a file without changing it does not reload.
    const std::string hex = crypto::Sha256Hex(content);
    u64 h = 1469598103934665603ULL;
    for (char c : hex) {
      h ^= (u64)(u8)c;
      h *= 1099511628211ULL;
    }
    *revision = h;
  }
  return true;
}

//----------------------------------------------------------------------------
// MemoryScriptSource
//----------------------------------------------------------------------------

void MemoryScriptSource::Set(std::string source) {
  std::lock_guard<std::mutex> lk(mutex_);
  source_ = std::move(source);
  ++revision_;
}

bool MemoryScriptSource::Read(std::string* source, u64* revision) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (revision_) {
    if (source) *source = source_;
    if (revision) *revision = revision_;
    return true;
  }
  return false;
}

u64 MemoryScriptSource::revision() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return revision_;
}

//----------------------------------------------------------------------------
// ScriptReloader
//----------------------------------------------------------------------------

ScriptReloader::ScriptReloader(std::string name, const CapabilitySet& granted,
                               IScriptSourceProvider* source, VmFactory vmFactory,
                               std::unique_ptr<ScriptErrorOverlay> overlay)
    : name_(std::move(name)),
      granted_(granted),
      source_(source),
      vmFactory_(std::move(vmFactory)) {
  if (overlay) {
    ownedOverlay_ = std::move(overlay);
  } else {
    ownedOverlay_ = std::unique_ptr<ScriptErrorOverlay>(new ScriptErrorOverlay());
  }
  overlay_ = ownedOverlay_.get();
  if (!vmFactory_) {
    vmFactory_ = []() -> std::unique_ptr<IScriptVm> {
      return std::unique_ptr<IScriptVm>(CreateReferenceVm());
    };
  }
}

ScriptReloader::~ScriptReloader() = default;

void ScriptReloader::SetLimits(const ScriptLimits& limits) {
  limits_ = limits;
  if (host_) host_->SetLimits(limits);
}

void ScriptReloader::SetBindings(const ScriptBindings* bindings) {
  bindings_ = bindings;
  if (host_) host_->SetBindings(bindings);
}

bool ScriptReloader::InitialLoad(ScriptError* err) {
  std::string source;
  u64 revision = 0;
  if (!source_ || !source_->Read(&source, &revision)) {
    ScriptError e = ScriptError::Make("source unavailable", name_, 0);
    overlay_->Set(name_, e);
    if (err) *err = e;
    return false;
  }
  auto vm = vmFactory_();
  auto host = std::unique_ptr<ScriptHost>(
      new ScriptHost(name_, granted_, std::move(vm)));
  host->SetLimits(limits_);
  if (bindings_) host->SetBindings(bindings_);
  ScriptError loadErr;
  if (!host->Load(source, name_, &loadErr)) {
    overlay_->Set(name_, loadErr);
    if (err) *err = loadErr;
    return false;
  }
  host_ = std::move(host);
  lastRevision_ = revision;
  haveRevision_ = true;
  overlay_->Clear();
  if (err) *err = ScriptError::None();
  return true;
}

ReloadResult ScriptReloader::Reload(bool force) {
  std::lock_guard<std::mutex> lk(mutex_);
  ReloadResult result;
  const u64 start = NowNs();

  std::string source;
  u64 revision = 0;
  if (!source_ || !source_->Read(&source, &revision)) {
    result.attempted = true;
    result.compileFailed = true;
    result.error = ScriptError::Make("source unavailable", name_, 0);
    overlay_->Set(name_, result.error);
    ++failedReloads_;
    result.elapsedMs = (NowNs() - start) / 1000000ULL;
    return result;
  }

  if (haveRevision_ && revision == lastRevision_ && !force) {
    result.unchanged = true;
    return result;
  }
  result.attempted = true;

  // Build the new revision in a fresh VM. The live host is untouched until the
  // load succeeds, so a broken edit cannot take down a working script.
  auto vm = vmFactory_();
  auto candidate = std::unique_ptr<ScriptHost>(
      new ScriptHost(name_, granted_, std::move(vm)));
  candidate->SetLimits(limits_);
  if (bindings_) candidate->SetBindings(bindings_);

  ScriptError loadErr;
  if (!candidate->Load(source, name_, &loadErr)) {
    result.compileFailed = true;
    result.error = loadErr;
    overlay_->Set(name_, loadErr);
    ++failedReloads_;
    result.elapsedMs = (NowNs() - start) / 1000000ULL;
    Log::Warn("ScriptReloader[%s]: reload failed, keeping previous revision",
              name_.c_str());
    return result;
  }

  // State handoff: carry script-visible state across the swap. Best-effort —
  // a revision may change its state shape, which is reported but not fatal.
  if (host_) {
    std::string state = host_->SerializeState();
    if (!state.empty()) {
      ScriptError restoreErr;
      result.stateRestored = candidate->RestoreState(state, &restoreErr);
      if (!result.stateRestored) {
        Log::Warn("ScriptReloader[%s]: state handoff skipped (%s)",
                  name_.c_str(), restoreErr.message.c_str());
      }
    }
  }

  host_ = std::move(candidate);
  lastRevision_ = revision;
  haveRevision_ = true;
  result.reloaded = true;
  overlay_->Clear();
  ++reloads_;
  result.elapsedMs = (NowNs() - start) / 1000000ULL;
  Log::Info("ScriptReloader[%s]: reloaded in %llu ms (state %s)", name_.c_str(),
            (unsigned long long)result.elapsedMs,
            result.stateRestored ? "handed off" : "reset");
  return result;
}

ReloadResult ScriptReloader::Poll() { return Reload(false); }

ReloadResult ScriptReloader::ForceReload() { return Reload(true); }

}  // namespace Berry
