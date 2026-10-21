// Script loading. See include/berry/loader.h for why this is a single choke
// point rather than a helper.
#include "berry/loader.h"
#include "berry/logger.h"

#include <algorithm>
#include <cstdio>
#include <memory>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <dirent.h>
  #include <sys/stat.h>
#endif

namespace Berry {

namespace {

std::string JoinPath(const std::string& dir, const std::string& leaf) {
  if (dir.empty()) return leaf;
  if (dir.back() == '/') return dir + leaf;
  return dir + "/" + leaf;
}

/// The canonical manifest file name inside a script directory.
constexpr const char* kManifestFile = "manifest.json";

}  // namespace

const char* LoadErrorName(LoadError e) {
  switch (e) {
    case LoadError::Ok: return "ok";
    case LoadError::ManifestUnreadable: return "manifest-unreadable";
    case LoadError::ManifestInvalid: return "manifest-invalid";
    case LoadError::BadSignature: return "bad-signature";
    case LoadError::SourceUnreadable: return "source-unreadable";
    case LoadError::SourceHashMismatch: return "source-hash-mismatch";
    case LoadError::SourceHashMissing: return "source-hash-missing";
    case LoadError::NotApproved: return "not-approved";
    case LoadError::NoCapabilities: return "no-capabilities";
    case LoadError::HostLoadFailed: return "host-load-failed";
    case LoadError::AlreadyLoaded: return "already-loaded";
  }
  return "?";
}

ScriptLoaderConfig ScriptLoaderConfig::DevMode() {
  ScriptLoaderConfig c;
  c.anchor = TrustAnchor::HmacSecret("berry-dev");
  c.requireSignature = false;
  c.requireSourceHash = false;
  return c;
}

//----------------------------------------------------------------------------
// RealScriptFileSystem
//----------------------------------------------------------------------------

bool RealScriptFileSystem::ReadFile(const std::string& path, std::string* out) {
  if (!out) return false;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::string data;
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
    data.append(buf, n);
  }
  const bool bad = std::ferror(f) != 0;
  std::fclose(f);
  if (bad) return false;
  *out = std::move(data);
  return true;
}

std::vector<std::string> RealScriptFileSystem::ListDir(const std::string& path) {
  std::vector<std::string> names;
#if defined(_WIN32)
  const std::string pattern = path.empty() ? std::string("*") : path + "\\*";
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return names;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    names.emplace_back(fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR* d = opendir(path.empty() ? "." : path.c_str());
  if (!d) return names;
  while (struct dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    // Only regular files: a subdirectory (or a device) is not a pack file.
    const std::string full =
        path.empty() ? name : (path + "/" + name);
    struct stat st;
    if (::stat(full.c_str(), &st) != 0) continue;
    if (!S_ISREG(st.st_mode)) continue;
    names.push_back(name);
  }
  closedir(d);
#endif
  std::sort(names.begin(), names.end());
  return names;
}

//----------------------------------------------------------------------------
// MemoryScriptFileSystem
//----------------------------------------------------------------------------

void MemoryScriptFileSystem::Write(const std::string& path,
                                   std::string contents) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto& entry : files_) {
    if (entry.first == path) {
      entry.second = std::move(contents);
      return;
    }
  }
  files_.emplace_back(path, std::move(contents));
}

void MemoryScriptFileSystem::Erase(const std::string& path) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto it = files_.begin(); it != files_.end(); ++it) {
    if (it->first == path) {
      files_.erase(it);
      return;
    }
  }
}

bool MemoryScriptFileSystem::ReadFile(const std::string& path,
                                      std::string* out) {
  if (!out) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& entry : files_) {
    if (entry.first == path) {
      *out = entry.second;
      return true;
    }
  }
  return false;
}

std::vector<std::string> MemoryScriptFileSystem::ListDir(
    const std::string& path) {
  std::vector<std::string> names;
  const std::string prefix = path.empty() ? std::string() : path + "/";
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& entry : files_) {
    if (entry.first.size() <= prefix.size()) continue;
    if (entry.first.compare(0, prefix.size(), prefix) != 0) continue;
    const std::string rest = entry.first.substr(prefix.size());
    // One level only: a nested path is a directory, not a file here.
    if (rest.find('/') != std::string::npos) continue;
    names.push_back(rest);
  }
  std::sort(names.begin(), names.end());
  return names;
}

//----------------------------------------------------------------------------
// Verification
//----------------------------------------------------------------------------

namespace {

/// Verifies a manifest JSON plus its entry source. Fills `outcome` with the
/// reason on failure. Shared by Load() and by the reload source provider, so a
/// reload is checked by exactly the rules of the first load.
bool VerifyManifestAndSource(const ScriptLoaderConfig& config,
                             const std::string& manifestJson,
                             const std::string& source,
                             ScriptManifest* outManifest, LoadOutcome* outcome) {
  auto fail = [outcome](LoadError code, ManifestError merr,
                        const std::string& msg) {
    if (outcome) {
      outcome->error = code;
      outcome->manifestError = merr;
      outcome->message = msg;
    }
    return false;
  };

  ScriptManifest manifest;
  ManifestError merr = ManifestError::Ok;
  std::string msg;
  if (!ScriptManifest::Parse(manifestJson, &manifest, &merr, &msg)) {
    return fail(LoadError::ManifestInvalid, merr, msg);
  }

  // The source binding is what makes a signature cover the code and not just
  // the capability list, so an absent binding is reported as its own failure
  // rather than folded into a generic signature rejection.
  if (config.requireSourceHash && manifest.sourceSha256.empty()) {
    return fail(LoadError::SourceHashMissing, ManifestError::MissingSourceHash,
                "manifest does not bind a source_sha256");
  }

  if (config.requireSignature) {
    if (!config.anchor.Valid()) {
      return fail(LoadError::BadSignature, ManifestError::BadSignature,
                  "no usable trust anchor configured");
    }
    if (!config.anchor.Verify(manifest, &merr, config.requireSourceHash)) {
      return fail(LoadError::BadSignature, merr,
                  std::string("signature rejected: ") + ManifestErrorName(merr));
    }
  }

  if (config.requireSourceHash &&
      !crypto::ConstantTimeEquals(ScriptManifest::SourceHash(source),
                                  manifest.sourceSha256)) {
    return fail(LoadError::SourceHashMismatch,
                ManifestError::SourceHashMismatch,
                "entry source does not match the signed source_sha256");
  }

  if (outManifest) *outManifest = manifest;
  if (outcome) {
    outcome->error = LoadError::Ok;
    outcome->manifestError = ManifestError::Ok;
    outcome->message.clear();
  }
  return true;
}

}  // namespace

//----------------------------------------------------------------------------
// LoadedScript::Source
//----------------------------------------------------------------------------

/// Re-reads and re-verifies the script directory. A file that no longer
/// verifies reports "unavailable", so the reloader keeps the previous revision
/// running instead of swapping in unverified code.
class LoadedScript::Source final : public IScriptSourceProvider {
 public:
  Source(std::string directory, ScriptLoaderConfig config, IScriptFileSystem* fs)
      : directory_(std::move(directory)),
        config_(std::move(config)),
        fs_(fs) {}

  bool Read(std::string* source, u64* revision) override {
    std::string manifestJson;
    if (!fs_ || !fs_->ReadFile(JoinPath(directory_, kManifestFile),
                               &manifestJson)) {
      return Unavailable(LoadError::ManifestUnreadable,
                         "cannot read manifest.json");
    }
    // Parse once to resolve `entry` before reading the source.
    ScriptManifest manifest;
    if (!ScriptManifest::Parse(manifestJson, &manifest, nullptr, nullptr)) {
      return Unavailable(LoadError::ManifestInvalid, "manifest invalid");
    }
    std::string src;
    if (!fs_->ReadFile(JoinPath(directory_, manifest.entry), &src)) {
      return Unavailable(LoadError::SourceUnreadable, "cannot read entry file");
    }
    LoadOutcome outcome;
    if (!VerifyManifestAndSource(config_, manifestJson, src, nullptr, &outcome)) {
      return Unavailable(outcome.error, outcome.message);
    }
    *source = std::move(src);
    // The revision covers both files, so an edit to either triggers a reload.
    *revision = RevisionOf(manifestJson, *source);
    lastError_ = LoadError::Ok;
    lastMessage_.clear();
    return true;
  }

  LoadError lastError() const { return lastError_; }
  const std::string& lastMessage() const { return lastMessage_; }

 private:
  bool Unavailable(LoadError code, std::string msg) {
    lastError_ = code;
    lastMessage_ = std::move(msg);
    return false;
  }

  static u64 RevisionOf(const std::string& manifestJson,
                        const std::string& source) {
    const std::string h = crypto::Sha256Hex(manifestJson + "\n" + source);
    u64 v = 0;
    for (size_t i = 0; i < h.size() && i < 16; ++i) {
      const char c = h[i];
      const u64 nib = (c >= 'a') ? (u64)(c - 'a' + 10) : (u64)(c - '0');
      v = (v << 4) | nib;
    }
    return v ? v : 1;
  }

  std::string directory_;
  ScriptLoaderConfig config_;
  IScriptFileSystem* fs_ = nullptr;
  LoadError lastError_ = LoadError::Ok;
  std::string lastMessage_;
};

//----------------------------------------------------------------------------
// LoadedScript
//----------------------------------------------------------------------------

ReloadResult LoadedScript::Reload(bool force) {
  if (!reloader_) {
    ReloadResult r;
    r.attempted = true;
    r.compileFailed = true;
    r.error = ScriptError::Make("script not loaded", directory_, 0);
    return r;
  }
  return force ? reloader_->ForceReload() : reloader_->Poll();
}

LoadError LoadedScript::lastVerifyError() const {
  return source_ ? source_->lastError() : LoadError::Ok;
}

std::string LoadedScript::lastVerifyMessage() const {
  return source_ ? source_->lastMessage() : std::string();
}

//----------------------------------------------------------------------------
// ScriptLoader
//----------------------------------------------------------------------------

ScriptLoader::ScriptLoader(ScriptLoaderConfig config, IScriptFileSystem* fs)
    : config_(std::move(config)), fs_(fs) {}

ScriptLoader::~ScriptLoader() = default;

CapabilitySet ScriptLoader::EffectiveCapabilities(const ScriptManifest& manifest,
                                                 const ManifestStore* grants) {
  // No store means no approval step: the declared set is the effective set.
  // Callers that need the approval gate pass a store.
  if (!grants) return manifest.capabilities;
  const CapabilityGrant* grant = grants->Grant(manifest.name);
  if (!grant) return CapabilitySet();
  // A grant from a different version never applies: new capabilities cannot
  // ride an approval that was made for older code.
  if (grant->version != manifest.version) return CapabilitySet();
  CapabilitySet effective;
  for (const Capability& want : manifest.capabilities.All()) {
    for (const Capability& have : grant->granted.All()) {
      if (have.Covers(want)) {
        effective.Add(want);
        break;
      }
    }
  }
  return effective;
}

bool ScriptLoader::VerifyOnly(const std::string& manifestJson,
                              const std::string& source,
                              ScriptManifest* outManifest,
                              LoadOutcome* outcome,
                              const ManifestStore* grants) {
  if (!VerifyManifestAndSource(config_, manifestJson, source, outManifest,
                               outcome)) {
    return false;
  }
  if (grants && grants->Count() > 0 && outManifest) {
    auto fail = [outcome](LoadError code, const std::string& msg) {
      if (outcome) {
        outcome->error = code;
        outcome->manifestError = ManifestError::Ok;
        outcome->message = msg;
      }
      return false;
    };
    // An approved-but-version-bumped script has no grant, which is the
    // intended outcome; report it as not approved so the caller can prompt.
    if (!grants->IsApproved(outManifest->name)) {
      return fail(LoadError::NotApproved, "script is not approved by the user");
    }
    if (EffectiveCapabilities(*outManifest, grants).empty() &&
        !outManifest->capabilities.empty()) {
      return fail(LoadError::NoCapabilities,
                  "approved grant covers none of the declared capabilities");
    }
  }
  return true;
}

bool ScriptLoader::Load(const std::string& directory, LoadedScript* out,
                        LoadOutcome* outcome, const ManifestStore* grants) {
  if (!out) return false;
  auto fail = [outcome](LoadError code, ManifestError merr,
                        const std::string& msg) {
    if (outcome) {
      outcome->error = code;
      outcome->manifestError = merr;
      outcome->message = msg;
    }
    return false;
  };
  if (!fs_) {
    return fail(LoadError::ManifestUnreadable, ManifestError::Ok,
                "no filesystem configured");
  }

  std::string manifestJson;
  if (!fs_->ReadFile(JoinPath(directory, kManifestFile), &manifestJson)) {
    return fail(LoadError::ManifestUnreadable, ManifestError::Ok,
                "cannot read manifest.json in " + directory);
  }

  ScriptManifest manifest;
  ManifestError merr = ManifestError::Ok;
  if (!ScriptManifest::Parse(manifestJson, &manifest, &merr, nullptr)) {
    return fail(LoadError::ManifestInvalid, merr, "manifest invalid");
  }

  std::string source;
  const std::string entryPath = JoinPath(directory, manifest.entry);
  if (!fs_->ReadFile(entryPath, &source)) {
    return fail(LoadError::SourceUnreadable, ManifestError::Ok,
                "cannot read entry file " + entryPath);
  }

  if (!VerifyManifestAndSource(config_, manifestJson, source, &manifest,
                               outcome)) {
    return false;
  }

  CapabilitySet effective = EffectiveCapabilities(manifest, grants);
  if (grants && grants->Count() > 0) {
    if (!grants->IsApproved(manifest.name)) {
      return fail(LoadError::NotApproved, ManifestError::Ok,
                  "script is not approved by the user");
    }
    if (effective.empty() && !manifest.capabilities.empty()) {
      return fail(LoadError::NoCapabilities, ManifestError::Ok,
                  "approved grant covers none of the declared capabilities");
    }
  }

  // The reloader re-reads and re-verifies on every poll, so a swapped file is
  // refused rather than trusted. It builds the host, so the host is still
  // created through exactly one path.
  auto provider = std::make_shared<LoadedScript::Source>(directory, config_, fs_);
  auto reloader = std::unique_ptr<ScriptReloader>(new ScriptReloader(
      manifest.name, effective, provider.get(), nullptr));
  ScriptError firstErr;
  if (!reloader->InitialLoad(&firstErr)) {
    return fail(LoadError::HostLoadFailed, ManifestError::Ok, firstErr.message);
  }

  out->manifest_ = manifest;
  out->granted_ = effective;
  out->directory_ = directory;
  out->source_ = std::move(provider);
  out->reloader_ = std::move(reloader);
  if (outcome) {
    outcome->error = LoadError::Ok;
    outcome->manifestError = ManifestError::Ok;
    outcome->message.clear();
  }
  return true;
}

}  // namespace Berry
