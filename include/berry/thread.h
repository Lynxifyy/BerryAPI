#pragma once
// BerryAPI threading model (Phase 0.5).
//
// Which thread is allowed to touch what:
//
//   ThreadRole::GameTick — the game's simulation tick (20/s default in
//       Bedrock). Runs the Engine::OnGameTick chain, Scheduler, module ticks.
//   ThreadRole::Render — the render thread, hooking eglSwapBuffers /
//       vkQueuePresentKHR. Runs Engine::OnFrame and the overlay draw. This is
//       the most timing-critical thread.
//   ThreadRole::Scripts — the script runtime worker(s) (Phase 1). NEVER run
//       scripts on the render thread.
//   ThreadRole::Detach — explicit detach / loader-tool calls.
//   ThreadRole::Unknown — no role asserted yet.
//
// ThreadModel tracks the current thread's asserted role. Assert* helpers
// log loudly and stop the offending work inside #ifdef BERRY_THREAD_ASSERT
// (default in debug/test builds) rather than corrupting a frame.
//
// Scripts: use ScopedScriptTimeslice on the script worker. If a caller ever
// tries to run scripts from the render thread, AssertNotRender() fires.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/logger.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace Berry {

enum class ThreadRole : u8 {
  Unknown = 0,
  GameTick,
  Render,
  Scripts,
  Detach
};

const char* ThreadRoleName(ThreadRole r);

class BERRY_API ThreadModel {
 public:
  static ThreadModel& Instance();

  /// Binds the calling thread to `role` for its lifetime on this thread.
  void Enter(ThreadRole role);
  /// Unbinds: restores Unknown.
  void Leave();

  ThreadRole CurrentRole() const;

  /// True when the current thread carries the given role.
  bool Is(ThreadRole role) const;

  // Assertions — every game-facing feature must call these. In builds without
  // BERRY_THREAD_ASSERT they degrade to a logged warning.
  void AssertTick() const;          // must be on GameTick
  void AssertRender() const;        // must be on Render
  void AssertScripts() const;       // must be on a script worker
  void AssertNotRender() const;     // never on Render (scripts, blocking IO)

 private:
  ThreadModel() = default;
  struct PerThread {
    ThreadRole role = ThreadRole::Unknown;
  };
  static ThreadRole GetRole();
  static void SetRole(ThreadRole role);

  // thread_local state lives in the .cpp; the mutex guards the registry for
  // diagnostic listing.
  mutable std::mutex mutex_;
};

}  // namespace Berry