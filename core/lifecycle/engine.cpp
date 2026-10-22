#include "berry/engine.h"
#include "berry/event.h"
#include "berry/services.h"
#include "berry/session_recorder.h"
#include "berry/thread.h"

#include <cstring>
#include <mutex>

namespace {

// A struct equal to defaults but with size + abi_version stamped, so the
// no-arg paths can call the same validated C ABI entry point.
BerryConfigC DefaultConfigC() {
  BerryConfigC cfg{};
  cfg.size = sizeof(cfg);
  cfg.abi_version = BERRY_ABI_VERSION;
  cfg.auto_attach_modules = 1;
  cfg.enable_render = 1;
  return cfg;
}

}  // namespace

namespace Berry {

Engine& Engine::Instance() {
  static Engine eng;
  return eng;
}

Engine::Engine() : recorder_(std::make_unique<SessionRecorder>()) {}

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

namespace {
const char* LogLevelNameC(int level) {
  switch (level) {
    case BerryLogTraceC: return "trace";
    case BerryLogDebugC: return "debug";
    case BerryLogInfoC:  return "info";
    case BerryLogWarnC:  return "warn";
    case BerryLogErrorC: return "error";
    default:             return nullptr;
  }
}
}  // namespace

bool Engine::Bootstrap() {
  return Bootstrap(DefaultConfigC());
}

bool Engine::Bootstrap(const BerryConfigC& cfg) {
  if (cfg.abi_version != BERRY_ABI_VERSION) {
    Log::Error("BerryAPI config ABI mismatch: library=%u caller=%u",
               (unsigned)BERRY_ABI_VERSION, (unsigned)cfg.abi_version);
    return false;
  }
  if (cfg.size == 0 || cfg.size < sizeof(cfg)) {
    Log::Error("BerryAPI config too small or empty: got %u bytes, need %zu",
               (unsigned)cfg.size, sizeof(cfg));
    return false;
  }

  EngineConfig internal;
  internal.logLevel = LogLevelNameC(cfg.log_level);
  internal.captureLog = cfg.capture_log != 0;
  internal.autoAttachModules = cfg.auto_attach_modules != 0;
  internal.enableRender = cfg.enable_render != 0;
  ApplyConfig(internal);
  Log::Info("BerryAPI v%s bootstrapped (abi %u)", BERRY_VERSION_STRING,
            (unsigned)BERRY_ABI_VERSION);
  // Register core signatures used by module seams. The combat/render/world
  // modules register their own per-version tables; engine here just resolves
  // the local runtime symbol the modules need.
  ResolveCoreSignatures();
  return true;
}

void Engine::ResolveCoreSignatures() {
  // Fail-closed default: until a module registers its per-version signature
  // table AND pins the expected build fingerprint, no signature rule may
  // resolve. An empty expected build permanently closes the table.
  SigCache::Instance().SetExpectedBuild({});
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
  ThreadModel::Instance().Enter(ThreadRole::GameTick);
  ThreadModel::Instance().AssertTick();
  currentTick_.store(tick);
  sched_.SetCurrentTick(tick);
  sched_.Tick(tick);
  Modules().TickAll(tick);
  ThreadModel::Instance().Leave();
}

void Engine::OnFrame() {
  if (!attached_.load()) return;
  ThreadModel::Instance().Enter(ThreadRole::Render);
  ThreadModel::Instance().AssertRender();
  // Render module's frame hook gets installed by Berry::Render on Attach.
  // Scripts never run on this thread; only render work + overlay.
  EventBus::Instance().Emit("berry:frame", nullptr, nullptr);
  ThreadModel::Instance().Leave();
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
  if (!e.Bootstrap()) return false;
  return e.Attach();
}

BERRY_API bool BERRY_InjectWithConfig(const BerryConfigC* cfg) {
  if (!cfg) return BERRY_Inject();   // null config fails safe to defaults
  std::lock_guard<std::mutex> lk(g_injectMutex);
  auto& e = Berry::Engine::Instance();
  if (!e.Bootstrap(*cfg)) return false;
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
    Berry::Engine::Instance().Bootstrap();
    Berry::Engine::Instance().Attach();
  }
}
#endif