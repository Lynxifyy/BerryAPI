#pragma once
// BerryAPI hot reload with state handoff (Phase 3).
//
// Reloading is ATOMIC: the new source is compiled and loaded into a fresh VM
// first, and only if that succeeds is the live script swapped. A bad edit
// therefore never takes down a working script — the previous revision keeps
// running and the failure is surfaced through the error overlay.
//
// State handoff: before the swap the outgoing script's state is serialized
// (see IScriptVm::SerializeState) and restored into the incoming VM, so a
// script that keeps counters/accumulators across a reload does not reset.
// The handoff is best-effort and reported: a new revision may legitimately
// change its state shape, in which case the script starts from defaults.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/script.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Berry {

class Renderer;   // fwd: overlay drawing is optional (see DrawErrorOverlay)

//----------------------------------------------------------------------------
// Reload result.
//----------------------------------------------------------------------------

struct BERRY_API ReloadResult {
  bool attempted = false;      // a reload actually ran (source changed)
  bool reloaded = false;       // the new revision is now live
  bool compileFailed = false;  // the new revision failed to load
  bool stateRestored = false;  // state handoff succeeded
  bool unchanged = false;      // revision matched; nothing to do
  u64 elapsedMs = 0;           // wall-clock cost of the swap
  ScriptError error;           // populated when compileFailed
};

//----------------------------------------------------------------------------
// Error overlay — data the HUD renders in-game.
//----------------------------------------------------------------------------

/// A single overlay line: text plus whether it is the error line (so the HUD
/// can highlight it). Rendering is deliberately separate so this stays
/// headless-testable.
struct BERRY_API OverlayLine {
  std::string text;
  bool isError = false;
};

class BERRY_API ScriptErrorOverlay {
 public:
  /// Records a failure for display. Replaces any previous error.
  void Set(const std::string& script, const ScriptError& error);
  /// Clears the overlay (a successful reload or an explicit dismiss).
  void Clear();

  bool visible() const;
  std::string script() const;
  ScriptError error() const;
  /// Formatted lines for the HUD, newest error only.
  std::vector<OverlayLine> Lines() const;

 private:
  mutable std::mutex mutex_;
  bool visible_ = false;
  std::string script_;
  ScriptError error_;
};

/// Draws the overlay in the top-left corner via the Renderer. Optional: the
/// HUD module can render Lines() itself instead.
BERRY_API void DrawErrorOverlay(Renderer& renderer, const ScriptErrorOverlay& overlay,
                                const Vec2& screenSize);

//----------------------------------------------------------------------------
// Source provider seam.
//----------------------------------------------------------------------------

/// Supplies script source + a revision marker. The revision is what the
/// reloader compares to decide whether anything changed, so a provider that
/// cannot cheaply hash can return an mtime or a monotonic counter.
class BERRY_API IScriptSourceProvider {
 public:
  virtual ~IScriptSourceProvider() = default;
  /// Reads the current source. On success fills `source` and `revision`.
  /// Returns false when the source is unavailable (missing file, IO error).
  virtual bool Read(std::string* source, u64* revision) = 0;
};

/// Reads from the filesystem; the revision is a content hash, so touching a
/// file without changing it does not trigger a reload.
class BERRY_API FileScriptSource : public IScriptSourceProvider {
 public:
  explicit FileScriptSource(std::string path) : path_(std::move(path)) {}
  bool Read(std::string* source, u64* revision) override;
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

/// In-memory provider for tests and for script text delivered over the RPC
/// channel. Thread-safe.
class BERRY_API MemoryScriptSource : public IScriptSourceProvider {
 public:
  void Set(std::string source);
  bool Read(std::string* source, u64* revision) override;
  u64 revision() const;

 private:
  mutable std::mutex mutex_;
  std::string source_;
  u64 revision_ = 0;
};

//----------------------------------------------------------------------------
// Reloader.
//----------------------------------------------------------------------------

/// Owns one script across revisions. Must be driven from the script worker
/// (reload runs script code, so it never belongs on the render thread).
class BERRY_API ScriptReloader {
 public:
  /// `vmFactory` produces a fresh VM per revision so a failed load can be
  /// discarded without disturbing the live VM.
  using VmFactory = std::function<std::unique_ptr<IScriptVm>()>;

  ScriptReloader(std::string name, const CapabilitySet& granted,
                 IScriptSourceProvider* source, VmFactory vmFactory,
                 std::unique_ptr<ScriptErrorOverlay> overlay = nullptr);
  ~ScriptReloader();

  ScriptReloader(const ScriptReloader&) = delete;
  ScriptReloader& operator=(const ScriptReloader&) = delete;

  const std::string& name() const { return name_; }

  /// Loads the first revision. Fails when the source is unreadable or invalid.
  bool InitialLoad(ScriptError* err);

  /// Re-reads the source and reloads when the revision changed. Atomic: on a
  /// compile failure the live script is left untouched and the overlay shows
  /// the error.
  ReloadResult Poll();

  /// Forces a reload even when the revision is unchanged (explicit user
  /// action, e.g. a "reload script" keybind).
  ReloadResult ForceReload();

  /// The live script. Never null after a successful InitialLoad.
  ScriptHost* host() { return host_.get(); }
  const ScriptHost* host() const { return host_.get(); }

  ScriptErrorOverlay& overlay() { return *overlay_; }
  const ScriptErrorOverlay& overlay() const { return *overlay_; }

  void SetLimits(const ScriptLimits& limits);
  void SetBindings(const ScriptBindings* bindings);

  u64 reloads() const { return reloads_; }
  u64 failedReloads() const { return failedReloads_; }
  u64 lastRevision() const { return lastRevision_; }

 private:
  ReloadResult Reload(bool force);

  std::string name_;
  CapabilitySet granted_;
  IScriptSourceProvider* source_ = nullptr;
  VmFactory vmFactory_;
  std::unique_ptr<ScriptErrorOverlay> ownedOverlay_;
  ScriptErrorOverlay* overlay_ = nullptr;
  std::unique_ptr<ScriptHost> host_;
  ScriptLimits limits_;
  const ScriptBindings* bindings_ = nullptr;
  u64 lastRevision_ = 0;
  bool haveRevision_ = false;
  u64 reloads_ = 0;
  u64 failedReloads_ = 0;
  mutable std::mutex mutex_;
};

}  // namespace Berry
