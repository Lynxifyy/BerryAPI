#include "berry/engine.h"
#include "berry/client.h"
#include "berry/event.h"
#include "berry/services.h"
#include "berry/session_recorder.h"
#include "berry/replay_service.h"
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
  cfg.enable_client = 1;
  return cfg;
}

}  // namespace

namespace Berry {

Engine& Engine::Instance() {
  static Engine eng;
  return eng;
}

Engine::Engine()
    : recorder_(std::make_unique<SessionRecorder>()),
      replay_(std::make_unique<ReplayService>()) {}

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
  internal.enableClient = cfg.enable_client != 0;
  internal.devMode = cfg.dev_mode != 0;
  if (cfg.script_directory) internal.scriptDirectory = cfg.script_directory;
  internal.localPlayerId = cfg.local_player_id;
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
  // Build the client composition root: it owns the client services and the
  // script host. It binds the World/Render/Combat handles the modules just
  // exposed, so it must be built after AttachAll. A client that fails to attach
  // is not fatal to the module set, but the failure is reported.
  if (cfg_.enableClient && !client_) {
    ClientConfig ccfg;
    ccfg.scriptDirectory = cfg_.scriptDirectory;
    ccfg.requireSignature = !cfg_.devMode;
    ccfg.requireSourceHash = !cfg_.devMode;
    ccfg.localPlayer.raw = cfg_.localPlayerId;
    client_ = std::make_unique<BerryClient>();
    if (!client_->Attach(*this, ccfg)) {
      Log::Error("BerryClient failed to attach; detaching modules");
      client_.reset();
      Modules().DetachAll(*this);
      return false;
    }
  }
  attached_.store(true);
  Log::Info("BerryAPI attached (%zu modules)", Modules().Count());
  return true;
}

void Engine::Detach() {
  if (!attached_.load()) return;
  // Tear the client down first: it owns the script worker and the services that
  // hold the module handles, so it must stop before the modules do.
  if (client_) {
    client_->Detach();
    client_.reset();
  }
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
  // Published after the modules have run, so a listener sees the tick's world
  // state settled. The combat-state layer uses it to age hit predictions and
  // record hitbox history; it carries the tick number as the payload.
  EventBus::Instance().Emit("berry:tick", &tick, nullptr);
  // The client's script tick runs on its own worker (posted, never inline on
  // this thread), so the game tick never blocks on script code.
  if (client_) client_->Tick(tick);
  ThreadModel::Instance().Leave();
}

void Engine::OnFrame() {
  if (!attached_.load()) return;
  ThreadModel::Instance().Enter(ThreadRole::Render);
  ThreadModel::Instance().AssertRender();
  // The replay mod's frame work: pump the loopback playback server and advance
  // the local review on the frame clock. Both are driven here, so playback and
  // the review work whether or not a script is loaded. The review's own draw
  // goes through the Render module's frame hook (it needs the frame's camera and
  // the renderer), which is the same order every other overlay uses.
  if (replay_) {
    replay_->PumpServer();
    const u64 now = NowMs();
    if (lastReplayFrameMs_ != 0 && now > lastReplayFrameMs_) {
      f32 dt = (f32)(now - lastReplayFrameMs_) / 1000.0f;
      if (dt > 0.1f) dt = 0.1f;   // a stall must not fast-forward a review
      replay_->AdvanceReview(dt);
    }
    lastReplayFrameMs_ = now;
  }
  // Render module's frame hook gets installed by Berry::Render on Attach.
  // Scripts never run on this thread; only render work + overlay.
  // Bracket the client frame around the hook: open the renderer frame and
  // publish the frame timing first (so a synchronous script read and the
  // governor see this frame), then emit `berry:frame` so every overlay draws
  // into that frame, then present. The frame delta is measured here, so the
  // governor's clock is the real presentation cadence.
  f32 frameMs = 0.0f;
  const u64 now = NowMs();
  if (lastFrameMs_ != 0 && now > lastFrameMs_) {
    frameMs = (f32)(now - lastFrameMs_);
    // A long stall (a breakpoint, a paused client) must not be read as a real
    // frame time by the governor; clamp it to a plausible worst case.
    if (frameMs > 1000.0f) frameMs = 1000.0f;
  }
  lastFrameMs_ = now;
  if (client_) client_->BeginFrame(frameMs, client_->screenSize());
  EventBus::Instance().Emit("berry:frame", nullptr, nullptr);
  if (client_) client_->EndFrame();
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