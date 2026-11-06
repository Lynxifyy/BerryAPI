// Script runtime primitives: values, errors, status names, the
// capability-gated binding registry, and the VM-facing convenience paths.
#include "berry/script.h"
#include "berry/event.h"     // NowNs
#include "berry/generated_bindings.h"  // api::IsBindingDeprecated (IDL source of truth)
#include "berry/logger.h"
#include "berry/thread.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Berry {

namespace {
/// A monotonic microsecond clock for the binding profiler.
u64 NowUs() { return NowNs() / 1000ULL; }
}  // namespace

//----------------------------------------------------------------------------
// ScriptValue
//----------------------------------------------------------------------------

ScriptValue ScriptValue::Bool(bool v) {
  ScriptValue s;
  s.type = Type::Bool;
  s.boolean = v;
  return s;
}

ScriptValue ScriptValue::Num(f64 v) {
  ScriptValue s;
  s.type = Type::Number;
  s.number = v;
  return s;
}

ScriptValue ScriptValue::Str(std::string v) {
  ScriptValue s;
  s.type = Type::String;
  s.string = std::move(v);
  return s;
}

ScriptValue ScriptValue::Tbl(ScriptTable t) {
  ScriptValue s;
  s.type = Type::Table;
  s.table = std::make_shared<ScriptTable>(std::move(t));
  return s;
}

const ScriptValue* ScriptValue::Field(const char* key) const {
  if (type != Type::Table || !table || !key) return nullptr;
  auto it = table->find(key);
  return it == table->end() ? nullptr : &it->second;
}

bool ScriptValue::Truthy() const {
  switch (type) {
    case Type::Nil: return false;
    case Type::Bool: return boolean;
    case Type::Number: return number != 0;
    case Type::String: return !string.empty();
    case Type::Table: return true;   // an empty table is truthy, as in Luau
  }
  return false;
}

f64 ScriptValue::AsNumber() const {
  switch (type) {
    case Type::Number: return number;
    case Type::Bool: return boolean ? 1 : 0;
    case Type::String: {
      // Numeric strings coerce; anything else is 0 (deterministic, no throw).
      char* end = nullptr;
      double v = std::strtod(string.c_str(), &end);
      if (end && end != string.c_str() && *end == '\0') return (f64)v;
      return 0;
    }
    case Type::Nil: return 0;
    case Type::Table: return 0;   // tables have no numeric value
  }
  return 0;
}

const std::string& ScriptValue::AsString() const {
  static const std::string kEmpty;
  return type == Type::String ? string : kEmpty;
}

std::string ScriptValue::ToDisplay() const {
  switch (type) {
    case Type::Nil: return "nil";
    case Type::Bool: return boolean ? "true" : "false";
    case Type::Number: {
      // Integers print without a decimal point; fractions keep precision.
      if (std::isfinite(number) && number == std::floor(number) &&
          std::fabs(number) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", (long long)number);
        return buf;
      }
      char buf[40];
      std::snprintf(buf, sizeof(buf), "%.14g", number);
      return buf;
    }
    case Type::String: return string;
    case Type::Table: {
      if (!table) return "{}";
      // Deterministic key order (std::map) so the form is stable in errors,
      // the REPL and the serialized snapshot.
      std::string out = "{";
      bool first = true;
      for (const auto& kv : *table) {
        if (!first) out += ", ";
        first = false;
        out += kv.first;
        out += " = ";
        out += kv.second.ToDisplay();
      }
      out += "}";
      return out;
    }
  }
  return "";
}

bool ScriptValue::operator==(const ScriptValue& o) const {
  if (type != o.type) {
    // Numbers and bools compare numerically; everything else is unequal.
    if ((type == Type::Number && o.type == Type::Bool) ||
        (type == Type::Bool && o.type == Type::Number)) {
      return AsNumber() == o.AsNumber();
    }
    return false;
  }
  switch (type) {
    case Type::Nil: return true;
    case Type::Bool: return boolean == o.boolean;
    case Type::Number: return number == o.number;
    case Type::String: return string == o.string;
    // Reference equality, like Luau tables: comparing by content could recurse
    // forever on a self-referential table.
    case Type::Table: return table == o.table;
  }
  return false;
}

//----------------------------------------------------------------------------
// ScriptError
//----------------------------------------------------------------------------

ScriptError ScriptError::Make(std::string msg, std::string chunk, i32 line) {
  ScriptError e;
  e.occurred = true;
  e.message = std::move(msg);
  e.chunk = std::move(chunk);
  e.line = line;
  return e;
}

//----------------------------------------------------------------------------
// Status names
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
// FrameQuota (improvement 57)
//----------------------------------------------------------------------------

bool FrameQuota::Record(f32 ms, u64 nowMs) {
  samples_[next_] = ms;
  next_ = (next_ + 1) % kWindow;
  if (count_ < kWindow) ++count_;
  if (ms > (f32)budgetMs_) ++over_;
  // A soft warning only when the ROLLING MEAN is over, so a single spike does
  // not warn; and at most once a second.
  if (budgetMs_ == 0 || MeanMs() <= (f32)budgetMs_) return false;
  if (nowMs - lastWarnMs_ >= 1000) {
    lastWarnMs_ = nowMs;
    Log::Warn("script frame budget: mean %.2fms over %llums (script work); "
              "%llu frames over", MeanMs(), (unsigned long long)budgetMs_,
              (unsigned long long)over_);
    return true;
  }
  return false;
}

f32 FrameQuota::MeanMs() const {
  if (count_ == 0) return 0.0f;
  f32 sum = 0.0f;
  for (size_t i = 0; i < count_; ++i) sum += samples_[i];
  return sum / (f32)count_;
}

void FrameQuota::Reset() {
  count_ = 0;
  next_ = 0;
  over_ = 0;
  lastWarnMs_ = 0;
}

const char* ScriptStatusName(ScriptStatus s) {
  switch (s) {
    case ScriptStatus::Ok: return "ok";
    case ScriptStatus::Running: return "running";
    case ScriptStatus::Yielded: return "yielded";
    case ScriptStatus::NotLoaded: return "not-loaded";
    case ScriptStatus::NoFunction: return "no-function";
    case ScriptStatus::Error: return "error";
    case ScriptStatus::BudgetExceeded: return "budget-exceeded";
    case ScriptStatus::MemoryExceeded: return "memory-exceeded";
    case ScriptStatus::Timeout: return "timeout";
    case ScriptStatus::Interrupted: return "interrupted";
    case ScriptStatus::CapabilityDenied: return "capability-denied";
    case ScriptStatus::Unloaded: return "unloaded";
  }
  return "unknown";
}

const char* ScriptHostStateName(ScriptHostState s) {
  switch (s) {
    case ScriptHostState::Idle: return "idle";
    case ScriptHostState::Loaded: return "loaded";
    case ScriptHostState::Faulted: return "faulted";
    case ScriptHostState::Unloaded: return "unloaded";
  }
  return "unknown";
}

//----------------------------------------------------------------------------
// ScriptBindings
//----------------------------------------------------------------------------

void ScriptBindings::Register(const std::string& name, const Capability& required,
                              ScriptNativeFn fn) {
  for (auto& e : entries_) {
    if (e.first == name) {
      e.second.required = required;
      e.second.fn = std::move(fn);
      return;
    }
  }
  entries_.push_back({name, Entry{required, std::move(fn)}});
}

bool ScriptBindings::Has(const std::string& name) const {
  for (const auto& e : entries_) {
    if (e.first == name) return true;
  }
  return false;
}

Capability ScriptBindings::Required(const std::string& name) const {
  for (const auto& e : entries_) {
    if (e.first == name) return e.second.required;
  }
  return Capability{};   // Unknown
}

ScriptValue ScriptBindings::Invoke(ScriptContext& ctx, const std::string& name,
                                   const std::vector<ScriptValue>& args, bool* ok,
                                   bool* denied) const {
  if (ok) *ok = false;
  if (denied) *denied = false;
  for (const auto& e : entries_) {
    if (e.first != name) continue;
    // Capability gate. A binding is reachable only when it EXPLICITLY declares
    // that it needs no capability, or when the script holds the capability it
    // asks for. An unset requirement (Unknown) is denied: treating "unset" as
    // "no capability required" is precisely the inversion that let a binding
    // be left silently unrestricted. NoCap must be named on purpose.
    if (e.second.required.kind != CapKind::NoCap &&
        !ctx.CanUse(e.second.required)) {
      if (denied) *denied = true;
      return ScriptValue::Nil();
    }
    // Deprecation notice, once per script per binding. The gate is the one
    // path every native call takes, so the warning cannot be missed by a
    // caller that reaches a binding through a different route, and it is
    // emitted only for a call that got past the capability gate.
    if (api::IsBindingDeprecated(name) && ctx.NoteDeprecatedUse(name)) {
      const api::BindingSpec* spec = api::FindBindingSpec(name);
      Log::Warn("script '%s' called deprecated binding '%s': %s",
                ctx.scriptName().c_str(), name.c_str(),
                spec && spec->deprecated ? spec->deprecated : "(no advice)");
    }
    if (!e.second.fn) {
      if (ok) *ok = true;
      return ScriptValue::Nil();
    }
    if (ok) *ok = true;
    // Per-binding profiling (improvement 58): every native call goes through
    // here, so timing it here gives a call/microsecond tally per binding with
    // no second instrumentation point.
    const u64 t0 = NowUs();
    ScriptValue r = e.second.fn(ctx, args);
    BindingProfiler::Instance().Record(name, NowUs() - t0);
    return r;
  }
  return ScriptValue::Nil();
}

BindingProfiler& BindingProfiler::Instance() {
  static BindingProfiler* inst = new BindingProfiler();
  return *inst;
}

void BindingProfiler::Record(const std::string& name, u64 micros) {
  if (!enabled_.load()) return;
  std::lock_guard<std::mutex> lk(mutex_);
  BindingStat& s = stats_[name];
  s.name = name;
  ++s.calls;
  s.totalUs += micros;
  if (micros > s.maxUs) s.maxUs = micros;
}

std::vector<BindingProfiler::BindingStat> BindingProfiler::Snapshot() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<BindingStat> out;
  out.reserve(stats_.size());
  for (const auto& kv : stats_) out.push_back(kv.second);
  std::sort(out.begin(), out.end(),
            [](const BindingStat& a, const BindingStat& b) {
              return a.totalUs > b.totalUs;   // expensive first
            });
  return out;
}

void BindingProfiler::Reset() {
  std::lock_guard<std::mutex> lk(mutex_);
  stats_.clear();
}

std::string BindingProfiler::ExportChromeTrace() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::string out = "{\"traceEvents\":[";
  double ts = 0.0;
  bool first = true;
  // Deterministic order by name.
  std::vector<std::pair<std::string, BindingStat>> v(stats_.begin(), stats_.end());
  std::sort(v.begin(), v.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& kv : v) {
    if (!first) out += ",";
    first = false;
    out += "{\"name\":\"" + kv.first + "\",\"ph\":\"X\",\"pid\":1,\"tid\":1,";
    out += "\"ts\":" + std::to_string(ts) + ",\"dur\":";
    out += std::to_string(kv.second.totalUs / 1000.0) + "}";
    ts += kv.second.totalUs / 1000.0;
  }
  out += "],\"displayTimeUnit\":\"ms\"}";
  return out;
}

size_t ScriptBindings::Count() const { return entries_.size(); }

std::vector<std::string> ScriptBindings::Names() const {
  std::vector<std::string> names;
  names.reserve(entries_.size());
  for (const auto& e : entries_) names.push_back(e.first);
  return names;
}

void ScriptBindings::Clear() { entries_.clear(); }

//----------------------------------------------------------------------------
// ScriptContext
//----------------------------------------------------------------------------

void ScriptContext::SetScratch(const std::string& key, ScriptValue v) {
  for (auto& kv : scratch_) {
    if (kv.first == key) {
      kv.second = std::move(v);
      return;
    }
  }
  scratch_.emplace_back(key, std::move(v));
}

bool ScriptContext::GetScratch(const std::string& key, ScriptValue* out) const {
  for (const auto& kv : scratch_) {
    if (kv.first == key) {
      if (out) *out = kv.second;
      return true;
    }
  }
  return false;
}

bool ScriptContext::NoteDeprecatedUse(const std::string& binding) {
  for (const auto& seen : deprecatedUses_) {
    if (seen == binding) return false;
  }
  deprecatedUses_.push_back(binding);
  return true;
}

//----------------------------------------------------------------------------
// IScriptVm convenience
//----------------------------------------------------------------------------

ScriptStatus IScriptVm::Call(const std::string& name,
                             const std::vector<ScriptValue>& args,
                             std::vector<ScriptValue>* ret, ScriptError* err) {
  ResetInstructionCount();
  ScriptStatus st = Start(name, args, err);
  if (st != ScriptStatus::Running && st != ScriptStatus::Ok) return st;
  // Drive to completion, respecting the VM's own budget as a safety ceiling so
  // a caller cannot spin forever on a runaway script.
  const u64 kStepBudget = 4096;
  u64 guard = 0;
  const u64 maxSteps = (Limits().instructionBudget / kStepBudget) + 16;
  while (inCall()) {
    st = Step(kStepBudget, err);
    if (st != ScriptStatus::Running) {
      if (ret) *ret = Result();
      return st;
    }
    if (++guard > maxSteps) {
      Abort();
      if (err) {
        *err = ScriptError::Make("instruction budget exceeded", "", 0);
      }
      return ScriptStatus::BudgetExceeded;
    }
  }
  if (ret) *ret = Result();
  return ScriptStatus::Ok;
}

//----------------------------------------------------------------------------
// ScopedScriptTimeslice
//----------------------------------------------------------------------------

ScopedScriptTimeslice::ScopedScriptTimeslice(IScriptVm* vm, u64 maxMs)
    : vm_(vm) {
  // Scripts run on the dedicated worker, never the render thread.
  ThreadModel::Instance().AssertScripts();
  startNs_ = NowNs();
  deadlineNs_ = startNs_ + maxMs * 1000000ULL;
}

ScopedScriptTimeslice::~ScopedScriptTimeslice() {
  // Leaving the scope must not leave a stale interrupt armed on the VM.
  if (vm_) vm_->ClearInterrupt();
}

bool ScopedScriptTimeslice::expired() const {
  if (NowNs() < deadlineNs_) return false;
  // Past the deadline: arm the VM interrupt so the next instruction boundary
  // stops the script instead of trusting it to yield voluntarily.
  if (vm_) vm_->RequestInterrupt();
  return true;
}

u64 ScopedScriptTimeslice::elapsedMs() const {
  return (NowNs() - startNs_) / 1000000ULL;
}

}  // namespace Berry
