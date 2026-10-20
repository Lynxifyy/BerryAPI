#pragma once
#include "berry/version.h"
#include "berry/export.h"
#include "berry/types.h"
#include "berry/config.h"
#include "berry/logger.h"
#include "berry/hooks.h"
#include "berry/sigscan.h"
#include "berry/event.h"
#include "berry/module.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace Berry {

//----------------------------------------------------------------------------
// Engine — central BerryAPI instance.
//
// Owns the tick/scheduler, hook manager, signature cache, event bus, module
// registry, and the game-code seams that the strict modules attach to.
// On Android, created lazily from the .so constructor; the render/UI loop is
// driven by hooking eglSwapBuffers (the presentation call) once per frame.
//----------------------------------------------------------------------------

// Configuration passed through the C ABI. The C struct BerryConfigC is the
// transport format; this is the internal view the engine holds. Bootstrap
// converts and re-validates so a tampered or mismatched config fails closed.
struct EngineConfig {
  const char* logLevel = nullptr;      // "trace" | "debug" | "info" | "warn" | "error"
  bool captureLog = false;
  bool autoAttachModules = true;
  bool enableRender = true;
};

class BERRY_API Engine {
 public:
  static Engine& Instance();

  // Lifecycle -------------------------------------------------------------
  /// C ABI entry: validates the config struct (size + abi_version) and
  /// bootstraps. Fail-closed on mismatch.
  bool Bootstrap(const BerryConfigC& cfg);
  /// C++ convenience: builds a BerryConfigC from defaults and bootstraps.
  bool Bootstrap();
  bool Attach();               // install hooks, attach modules
  void Detach();               // remove hooks, detach modules
  bool attached() const { return attached_.load(); }

  // Subsystems ------------------------------------------------------------
  HookManager& Hooks() { return hooks_; }
  SigCache& Sig() { return SigCache::Instance(); }
  EventBus& Bus() { return EventBus::Instance(); }
  Scheduler& Sched() { return sched_; }
  ModuleRegistry& Modules() { return modules_; }

  // Tick -------------------------------------------------------------------
  /// Called from the game's RTT simulation tick each frame.
  void OnGameTick(GameTick tick);
  /// Current absolute tick.
  GameTick Tick() const { return currentTick_; }

  // Frame (render) ---------------------------------------------------------
  /// Hooked from eglSwapBuffers in the on-device Render module. When running
  /// on the host harness, called directly by tests / the sample.
  void OnFrame();

  u64 TicksPerSecond() const { return 20; }

  EngineConfig& Config() { return cfg_; }

 private:
  Engine() = default;
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  void ApplyConfig(const EngineConfig& cfg);
  void ResolveCoreSignatures();

  HookManager hooks_;
  Scheduler sched_;
  ModuleRegistry modules_;
  std::atomic<GameTick> currentTick_{0};
  std::atomic<bool> attached_{false};
  EngineConfig cfg_;
  GameTick lastFrameTick_ = 0;
};

}  // namespace Berry

//----------------------------------------------------------------------------
// Injector entry points (exported).
//
// -------- loader tools --------
// On Android these are reached from a JNI / dlopen descriptor, but the
// canonical path is the ELF constructor:
//    __attribute__((constructor))
//    static void BerryInit() { Berry::Engine::Instance().Bootstrap(); }
//
// Berry::Inject() is the explicit, idempotent entry that the constructor
// calls, guarded so multiple dlopen/detach cycles are safe.
//----------------------------------------------------------------------------
extern "C" {
BERRY_API bool BERRY_Inject();
BERRY_API bool BERRY_InjectWithConfig(const BerryConfigC* cfg);
BERRY_API void BERRY_Detach();
BERRY_API bool BERRY_IsAttached();
BERRY_API const char* BERRY_Version();
}