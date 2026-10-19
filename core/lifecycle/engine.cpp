#include "berry/engine.h"
#include "berry/event.h"
#include "berry/services.h"

#include <cstring>
#include <mutex>

namespace Berry {

Engine& Engine::Instance() {
  static Engine eng;
  return eng;
}

Engine::~Engine() {
  Detach();
}

void Engine::ApplyConfig(const EngineConfig& cfg) {
  cfg_ = cfg;
  if (cfg.logLevel) {
    if (strcmp(cfg.logLevel, "trace") == 0) Log::SetLevel(LogLevel::Trace);
    else if (strcmp(cfg.logLevel, "debug") == 0) Log::SetLevel(LogLevel::Debug);
    else if (strcmp(cfg.logLevel, "warn") == 0) Log::SetLevel(LogLevel::Warn);
    else if (strcmp(cfg.logLevel, "error") == 0) Log::SetLevel(LogLevel::Error);
    else Log::SetLevel(LogLevel::Info);
  }
  Log::Capture(cfg.captureLog);
}

bool Engine::Bootstrap(const EngineConfig& cfg) {
  ApplyConfig(cfg);
  Log::Info("BerryAPI v%s bootstrapped", BERRY_VERSION_STRING);
  // Register core signatures used by module seams. The combat/render/world
  // modules register their own per-version tables; engine here just resolves
  // the local runtime symbol the modules need.
  ResolveCoreSignatures();
  return true;
}

void Engine::ResolveCoreSignatures() {
  // This is where on-device code would Register() the "damageCall",
  // "entityPosition", "levelTick" etc signatures from a version table.
  // The table lives in modules/ so it can be updated independently of the
  // core. When running against the shim, resolutions happen inside the
  // modules themselves because the shim exports concrete C functions.
}

bool Engine::Attach() {
  if (attached_.load()) return true;
  Log::Info("BerryAPI attaching…");
  auto backend = Hooks().Backend();
  if (!backend->CanHook()) {
    Log::Error("No usable hook backend (host shim or Dobby). Abort attach.");
    return false;
  }
  // Register the strict modules if nothing has registered them yet.
  if (Modules().Count() == 0 && cfg_.autoAttachModules) {
    for (auto& m : CreateDefaultModules()) {
      if (!Modules().Register(m)) Log::Warn("Failed to register %s", m->Name());
    }
  }
  if (cfg_.autoAttachModules) {
    Modules().AttachAll(*this);
  }
  attached_.store(true);
  Log::Info("BerryAPI attached (%zu modules)", Modules().Count());
  return true;
}

void Engine::Detach() {
  if (!attached_.load()) return;
  Modules().DetachAll(*this);
  attached_.store(false);
  Log::Info("BerryAPI detached");
}

void Engine::OnGameTick(GameTick tick) {
  currentTick_.store(tick);
  sched_.SetCurrentTick(tick);
  sched_.Tick(tick);
  Modules().TickAll(tick);
}

void Engine::OnFrame() {
  if (!attached_.load()) return;
  // Render module's frame hook gets installed by Berry::Render on Attach.
  EventBus::Instance().Emit("berry:frame", nullptr, nullptr);
}

}  // namespace Berry

//----------------------------------------------------------------------------
// Export entry points
//----------------------------------------------------------------------------

extern "C" {

static std::mutex g_injectMutex;

BERRY_API bool BERRY_Inject() {
  std::lock_guard<std::mutex> lk(g_injectMutex);
  auto& e = Berry::Engine::Instance();
  if (!e.Bootstrap({})) return false;
  return e.Attach();
}

BERRY_API bool BERRY_InjectWithConfig(const Berry::EngineConfig& cfg) {
  std::lock_guard<std::mutex> lk(g_injectMutex);
  auto& e = Berry::Engine::Instance();
  if (!e.Bootstrap(cfg)) return false;
  return e.Attach();
}

BERRY_API void BERRY_Detach() {
  std::lock_guard<std::mutex> lk(g_injectMutex);
  Berry::Engine::Instance().Detach();
}

BERRY_API bool BERRY_IsAttached() {
  return Berry::Engine::Instance().attached();
}

BERRY_API const char* BERRY_Version() {
  return BERRY_VERSION_STRING;
}

}  // extern "C"

// Optional ELF constructor — auto-attach when this TU is built into the
// actual shared library (libBerryAPI.so) that gets dlopen'd. Disabled for
// host test binaries where main() drives the lifecycle explicitly.
#if defined(BERRY_BUILD_SHARED) && (defined(__clang__) || defined(__GNUC__))
__attribute__((constructor))
static void BerryLibraryInit() {
  if (!Berry::Engine::Instance().attached()) {
    Berry::Engine::Instance().Bootstrap({});
    Berry::Engine::Instance().Attach();
  }
}
#endif