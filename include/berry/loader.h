#pragma once
// Script loading: the one place a ScriptHost is built for a real script.
//
// Before this existed, a script host could be built from any source string
// with any capability set: nothing tied the running code to the manifest that
// was reviewed, and the manifest's signature covered only the capability list,
// not the code. The loader closes that loop:
//
//   1. Read `manifest.json` from the script directory.
//   2. Verify its signature against the trust anchor (Ed25519 in production).
//   3. Read the entry file and check it hashes to the manifest's
//      `source_sha256`. A signature therefore attests to the exact bytes that
//      will run.
//   4. Intersect the manifest's declared capabilities with the user's
//      approval. A script can never run with more than the user approved, and
//      an unapproved script runs with none.
//   5. Only then construct the host.
//
// Steps 2-3 are repeated on every reload, so a swapped file is caught rather
// than silently loaded over a previously verified revision.

#include "berry/export.h"
#include "berry/manifest.h"
#include "berry/reload.h"
#include "berry/script.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Berry {

//----------------------------------------------------------------------------
// Filesystem seam.
//----------------------------------------------------------------------------

/// Minimal filesystem access. A seam rather than direct calls so the loader is
/// testable without touching disk, and so a device build can route reads
/// through the game's own file layer if it needs to.
class BERRY_API IScriptFileSystem {
 public:
  virtual ~IScriptFileSystem() = default;
  /// Reads a whole file. Returns false when it is missing or unreadable.
  virtual bool ReadFile(const std::string& path, std::string* out) = 0;
  /// Reports a directory listing. The pack loader uses it to discover pack
  /// files under a root without knowing the host's directory API. Default
  /// returns an empty list ("no listing available"), which makes a pack load
  /// find nothing rather than fail: a seam that cannot enumerate is simply a
  /// directory with no packs in it.
  virtual std::vector<std::string> ListDir(const std::string& path) {
    (void)path;
    return {};
  }
};

/// Reads from the host filesystem. Paths are used as given.
class BERRY_API RealScriptFileSystem : public IScriptFileSystem {
 public:
  bool ReadFile(const std::string& path, std::string* out) override;
  /// Lists regular file names (not directories) directly under `path`.
  std::vector<std::string> ListDir(const std::string& path) override;
};

/// In-memory filesystem for tests and for scripts delivered over the RPC
/// channel. Thread-safe.
class BERRY_API MemoryScriptFileSystem : public IScriptFileSystem {
 public:
  void Write(const std::string& path, std::string contents);
  void Erase(const std::string& path);
  bool ReadFile(const std::string& path, std::string* out) override;
  /// Lists stored paths directly under `path` (one level, files only).
  std::vector<std::string> ListDir(const std::string& path) override;

 private:
  mutable std::mutex mutex_;
  std::vector<std::pair<std::string, std::string>> files_;
};

//----------------------------------------------------------------------------
// Verification failures.
//----------------------------------------------------------------------------

enum class LoadError : u32 {
  Ok = 0,
  ManifestUnreadable,
  ManifestInvalid,
  BadSignature,
  SourceUnreadable,
  SourceHashMismatch,
  SourceHashMissing,
  NotApproved,
  NoCapabilities,
  HostLoadFailed,
  AlreadyLoaded,
};

const char* LoadErrorName(LoadError e);

/// A verification outcome: the coarse code plus the manifest error that caused
/// it, so a caller can surface a precise reason in the overlay.
struct BERRY_API LoadOutcome {
  LoadError error = LoadError::Ok;
  ManifestError manifestError = ManifestError::Ok;
  std::string message;

  bool ok() const { return error == LoadError::Ok; }
};

//----------------------------------------------------------------------------
// Configuration.
//----------------------------------------------------------------------------

struct BERRY_API ScriptLoaderConfig {
  /// What manifests are verified against. Ed25519 in production.
  TrustAnchor anchor;
  /// Require a verified signature. Turning this off is a development-only
  /// escape hatch; it also disables the source-hash binding, because an
  /// unsigned manifest's hash proves nothing.
  bool requireSignature = true;
  /// Require the manifest to bind a source hash (independent of
  /// requireSignature: a signed manifest must always bind its code).
  bool requireSourceHash = true;

  /// Produces a fresh VM per revision. Empty means the self-contained reference
  /// VM, which is right for the host tests; the device host sets `CreateLuauVm`
  /// so the same loader path builds the real scripting VM. A reload discards a
  /// failed candidate VM, so this must return a new VM each call.
  std::function<std::unique_ptr<IScriptVm>()> vmFactory;

  /// A development configuration: no signature, no source binding. Never use
  /// on device; scripts load straight from the directory as written.
  static ScriptLoaderConfig DevMode();
};

//----------------------------------------------------------------------------
// A verified, loaded script.
//----------------------------------------------------------------------------

/// Owns everything a running script needs: the verified manifest, the
/// effective capability set, and the reloader that keeps it up to date. The
/// loader is the only thing that constructs one.
class BERRY_API LoadedScript {
 public:
  LoadedScript() = default;
  LoadedScript(LoadedScript&&) = default;
  LoadedScript& operator=(LoadedScript&&) = default;
  LoadedScript(const LoadedScript&) = delete;
  LoadedScript& operator=(const LoadedScript&) = delete;

  const ScriptManifest& manifest() const { return manifest_; }
  const CapabilitySet& granted() const { return granted_; }
  const std::string& directory() const { return directory_; }

  ScriptHost* host() { return reloader_ ? reloader_->host() : nullptr; }
  ScriptErrorOverlay& overlay() { return reloader_->overlay(); }

  /// Re-reads and re-verifies the script. A file that no longer verifies is
  /// refused and the previous revision keeps running.
  ReloadResult Reload(bool force = false);

  /// Sets the native bindings the host exposes. A reload applies them to the
  /// fresh host, so a reloaded revision sees the same surface. The bindings
  /// must outlive the script.
  void SetBindings(const ScriptBindings* bindings);

  /// Reports why the last reload's re-verification failed, if it did. Empty
  /// when the last read verified.
  LoadError lastVerifyError() const;
  std::string lastVerifyMessage() const;

  bool valid() const { return reloader_ != nullptr; }

  /// The provider the reloader reads through. Public only so the loader can
  /// construct it; it is not part of the caller-facing surface.
  class Source;

 private:
  friend class ScriptLoader;

  ScriptManifest manifest_;
  CapabilitySet granted_;
  std::string directory_;
  std::shared_ptr<Source> source_;
  std::unique_ptr<ScriptReloader> reloader_;
};

//----------------------------------------------------------------------------
// The loader.
//----------------------------------------------------------------------------

/// Builds script hosts from verified manifests. Every script in the game goes
/// through here, so there is exactly one code path that decides whether code
/// runs and with what capabilities.
class BERRY_API ScriptLoader {
 public:
  ScriptLoader(ScriptLoaderConfig config, IScriptFileSystem* fs);
  ~ScriptLoader();

  ScriptLoader(const ScriptLoader&) = delete;
  ScriptLoader& operator=(const ScriptLoader&) = delete;

  const ScriptLoaderConfig& config() const { return config_; }

  /// Loads and verifies the script rooted at `directory` (which must contain
  /// `manifest.json` and the declared entry file).
  ///
  /// `grants` supplies the user's approvals; the effective capability set is
  /// the intersection of the manifest's declared set and the approval for the
  /// same name and version. Pass nullptr to require no approval (tests and
  /// first-party scripts), in which case the declared set is used as-is.
  ///
  /// On failure `out` is left untouched and `outcome` describes why.
  bool Load(const std::string& directory, LoadedScript* out,
            LoadOutcome* outcome = nullptr, const ManifestStore* grants = nullptr);

  /// Verifies a manifest + source pair without building a host. Used by the
  /// CLI signer's self-check and by tests.
  bool VerifyOnly(const std::string& manifestJson, const std::string& source,
                  ScriptManifest* outManifest, LoadOutcome* outcome = nullptr,
                  const ManifestStore* grants = nullptr);

  /// The effective capability set for a verified manifest: the intersection of
  /// what it declares and what was approved. Empty when `grants` has no
  /// approval for this name/version. Public so the policy can be asserted
  /// without building a host.
  static CapabilitySet EffectiveCapabilities(const ScriptManifest& manifest,
                                             const ManifestStore* grants);

 private:
  ScriptLoaderConfig config_;
  IScriptFileSystem* fs_ = nullptr;
};

}  // namespace Berry
