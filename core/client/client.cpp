// BerryClient — the production composition root for the client surface.
//
// Before this existed, every client-side service (the UI layer, the HUD, the
// PvP overlay, the radial menu, the animator, the crystal advisor, the voice
// session, the combat-state tracker, the registries, the file store, the audio
// cues, the packet log, the WebSocket pool, the replay camera, the intent
// validator) was declared and implemented, but nothing owned an instance: a
// `ScriptApi::Services` was only ever assembled by a test, so no production
// caller ever reached any of it. This is that owner.
//
// It builds the script host through the one verified path (`ScriptLoader`) and
// drives it on the thread the threading model assigns: the tick and the async
// pump run on the script worker, the frame work runs on the render thread. The
// services reach the world through the same `WorldHandle` seam every other
// consumer uses, so nothing here reads game memory.
#include "berry/client.h"

#include "berry/engine.h"
#include "berry/logger.h"
#include "berry/services.h"
#include "berry/thread.h"

#include <utility>

namespace Berry {

BerryClient::BerryClient() = default;

BerryClient::~BerryClient() { Detach(); }

bool BerryClient::Attach(Engine& engine, const ClientConfig& cfg) {
  if (attached_) return true;
  engine_ = &engine;
  localPlayer_ = cfg.localPlayer;
  networkPolicy_ = cfg.networkPolicy;
  screenSource_ = cfg.screenSource;

  // The world handle comes from the World module, which the engine registered
  // and attached. Without it every world/player read fails closed, which is the
  // correct reading of "the seam is not bound yet".
  world_ = WorldFrom(engine.Modules());
  if (!world_) {
    Log::Warn("BerryClient: no World module — world/player reads fail closed");
  }

  // Own the services. Construction order follows their dependencies: the
  // registries and stores first, then the modules that read them, then the
  // overlay services.
  governor_ = std::make_unique<PerformanceGovernor>();
  hud_ = std::make_unique<Hud>();
  input_ = std::make_unique<InputObserver>();
  ui_ = std::make_unique<UiLayer>();
  pvp_ = std::make_unique<PvpOverlay>();
  radial_ = std::make_unique<RadialMenu>();
  animator_ = std::make_unique<Animator>();
  combatState_ = std::make_unique<CombatStateTracker>();
  cosmetics_ = std::make_unique<CosmeticService>();
  // The animator drives the rendered rig through this seam: the cosmetic
  // renderer resolves bones through it, so a clip a script plays moves the
  // local rig instead of sitting in a table. Its base is null; a device adapter
  // installs a real skeleton seam behind it.
  animatorSeam_ = std::make_unique<AnimatedSkeletonSource>(nullptr, animator_.get());
  cosmetics_->SetSkeletonSource(animatorSeam_.get());
  modules_ = std::make_unique<ScriptModuleRegistry>();
  shaders_ = std::make_unique<ShaderRegistry>();
  packages_ = std::make_unique<PackageRegistry>();
  files_ = std::make_unique<ScriptFileStore>();
  audio_ = std::make_unique<AudioService>();
  packets_ = std::make_unique<PacketLog>();
  intents_ = std::make_unique<IntentValidator>();
  wsPool_ = std::make_unique<WsClientPool>();
  replayCamera_ = std::make_unique<ReplayCamera>();
  crystal_ = std::make_unique<CrystalOptimizer>();
  voice_ = std::make_unique<VoiceService>();
  // Model packs load through the loader's filesystem seam; the client owns a
  // real-filesystem instance so `cosmetic.reload_packs` has a root to read.
  fs_ = std::make_unique<RealScriptFileSystem>();
  packs_ = std::make_unique<CosmeticPackLoader>(fs_.get());

  // Own the on-device overlay backend and install it as the Renderer's seam.
  // This is the production owner of the render path: the Renderer otherwise
  // only ever had a test-installed seam, so nothing presented the batch on
  // device. On the host the backend is the null stub; `Start()` then reports
  // false and no seam is installed (the batch is discarded), which is the
  // correct headless behaviour. A backend that cannot start must not leave a
  // half-bound seam behind, so the seam is installed only after Start().
  renderBackend_.reset(CreateRenderBackend());
  if (renderBackend_ && renderBackend_->Start()) {
    renderSeam_.reset(CreateBackendSeam(renderBackend_.get()));
    Log::Info("BerryClient: render backend '%s' started", renderBackend_->name());
  } else {
    Log::Info("BerryClient: no render backend (headless); overlays draw to a "
              "discarded batch");
    renderBackend_.reset();
  }
  // Install the seam on the Renderer (the Render module's instance) so the
  // frame's batch reaches the backend. Only when a backend started; otherwise
  // the Renderer keeps its null seam and discards the batch.
  Renderer* renderer = RendererFrom(engine.Modules());
  if (renderer && renderSeam_) renderer->BindSeam(renderSeam_.get());

  // Build the services view the ScriptApi binds. The replay service lives on
  // the engine (the World module feeds its capture and the engine drives its
  // review); the script-facing pointer is bound to that same instance.
  BuildServices();
  api_->InstallBindings(&bindings_);

  // The async runtime shares the binding table: awaits and the scheduler are
  // reachable from a script under the same names the IDL documents.
  async_ = std::make_unique<AsyncRuntime>(
      [&engine]() { return engine.Tick(); },
      []() { return NowMs(); });
  async_->InstallBindings(&bindings_, cfg.networkPolicy);

  // The loader is the only thing allowed to build a host. A configured
  // directory is loaded through it; an empty one leaves the client scriptless
  // (the services still run, so overlays driven by other modules work).
  if (!cfg.scriptDirectory.empty()) {
    ScriptLoaderConfig lcfg;
    lcfg.anchor = cfg.anchor;
    lcfg.requireSignature = cfg.requireSignature;
    lcfg.requireSourceHash = cfg.requireSourceHash;
    // The device host runs real Luau; the reference VM is the host-test backend.
    lcfg.vmFactory = []() -> std::unique_ptr<IScriptVm> {
      return std::unique_ptr<IScriptVm>(CreateLuauVm());
    };
    loader_ = std::make_unique<ScriptLoader>(lcfg, fs_.get());
    if (!LoadScript(cfg)) {
      Log::Warn("BerryClient: script '%s' not loaded: %s",
                cfg.scriptDirectory.c_str(), lastScriptError_.c_str());
      loader_.reset();
    }
  }

  // The worker runs the script. It is started even when no script loaded, so
  // `Tick` can post and drain uniformly.
  worker_ = std::make_unique<ScriptWorker>();
  worker_->Start();

  attached_ = true;
  Log::Info("BerryClient attached (script: %s)", script_ ? "loaded" : "none");
  return true;
}

void BerryClient::BuildServices() {
  ScriptApi::Services s;
  renderer_ = RendererFrom(engine_->Modules());
  s.renderer = renderer_;
  s.hud = hud_.get();
  s.input = input_.get();
  s.governor = governor_.get();
  s.combat = CombatFrom(engine_->Modules());
  s.world = world_;
  s.cosmetics = cosmetics_.get();
  s.packs = packs_.get();
  s.velocity = nullptr;   // the device adapter installs a real motion source
  s.intents = intents_.get();
  s.shaders = shaders_.get();
  s.packages = packages_.get();
  s.modules = modules_.get();
  s.files = files_.get();
  s.audio = audio_.get();
  s.pvp = pvp_.get();
  s.packets = packets_.get();
  s.profiler = &Profiler::Instance();
  s.localPlayer = localPlayer_;
  s.recorder = &engine_->Recorder();
  s.replay = &engine_->Replay();
  s.networkPolicy = networkPolicy_;
  s.wsPool = wsPool_.get();
  s.combatState = combatState_.get();
  s.ui = ui_.get();
  s.animator = animator_.get();
  s.animatorSeam = animatorSeam_.get();
  s.replayCamera = replayCamera_.get();
  s.radial = radial_.get();
  s.crystal = crystal_.get();
  s.voice = voice_.get();
  services_ = s;
  api_ = std::make_unique<ScriptApi>(services_);
}

bool BerryClient::LoadScript(const ClientConfig& cfg) {
  script_ = std::make_unique<LoadedScript>();
  LoadOutcome outcome;
  // No approval store: a first-party script's declared set is its effective
  // set. A user-facing build passes a ManifestStore to intersect approvals.
  if (!loader_->Load(cfg.scriptDirectory, script_.get(), &outcome, nullptr)) {
    lastScriptError_ =
        outcome.message.empty() ? LoadErrorName(outcome.error) : outcome.message;
    script_.reset();
    return false;
  }
  // The host must expose the same bindings the rest of the client uses.
  script_->SetBindings(&bindings_);
  lastScriptError_.clear();
  return true;
}

void BerryClient::Detach() {
  if (!attached_) return;
  attached_ = false;
  // Stop the worker before tearing down the host/bindings it runs against: a
  // task in flight must not touch a destroyed ScriptApi.
  if (worker_) worker_->Stop();
  // Unload the script before the services it binds: the host holds pointers
  // into them through the binding lambdas.
  if (script_ && script_->host()) script_->host()->Unload();
  script_.reset();
  worker_.reset();
  loader_.reset();
  async_.reset();
  if (api_) api_->SetServices(ScriptApi::Services{});
  api_.reset();
  bindings_.Clear();
  services_ = ScriptApi::Services{};
  // Stop the backend after the renderer is no longer presenting, then release
  // the seam before the backend it points at.
  renderSeam_.reset();
  if (renderBackend_) renderBackend_->Stop();
  renderBackend_.reset();
  Log::Info("BerryClient detached");
}

void BerryClient::Tick(GameTick tick) {
  if (!attached_ || !script_ || !script_->host()) return;
  if (!worker_) return;
  // The script's tick work runs on the worker, never the game thread. Capture
  // the tick by value; the worker may run after this returns.
  worker_->Post([this, tick]() { RunTickOnWorker(tick); });
}

void BerryClient::RunTickOnWorker(GameTick tick) {
  // Runs on the script worker. Everything here touches the host (script code)
  // and the async scheduler, both of which belong to this thread.
  ThreadModel::Instance().AssertScripts();
  ScriptHost* host = script_ ? script_->host() : nullptr;
  if (!host || !host->loaded()) return;
  // A suspended host resumes through the async pump; a live one runs on_tick.
  if (host->suspended()) {
    async_->Pump(*host, tick);
  } else {
    host->RunHook(ScriptHooks::OnTick());
  }
}

Vec2 BerryClient::screenSize() const {
  // The embedder's source wins when it reports a real size; otherwise the
  // owned backend's EGL surface size is the truth on device. Both are checked
  // so a zero/unknown answer falls through instead of blanking the overlay.
  if (screenSource_) {
    const Vec2 s = screenSource_();
    if (s.x > 0 && s.y > 0) return s;
  }
  if (renderBackend_) {
    const Vec2 s = renderBackend_->SurfaceSize();
    if (s.x > 0 && s.y > 0) return s;
  }
  if (api_) return api_->screenSize();
  return Vec2(0, 0);
}

void BerryClient::BeginFrame(f32 frameMs, const Vec2& screen) {
  if (!attached_) return;
  // Open the renderer frame first, so every overlay that draws through it (the
  // HUD, the PvP overlay, the UI, the radial menu, the replay review, the
  // script-module overlay) queues into this frame's batch. Then publish the
  // timing: that drives the governor, the pipeline and the HUD clock, and is
  // what the synchronous script reads answer from.
  if (renderer_) renderer_->BeginFrame(screen);
  api_->PublishFrame(frameMs, screen);
}

void BerryClient::EndFrame() {
  if (!attached_) return;
  // Present the batch the frame queued. A no-op when no frame is open.
  if (renderer_) renderer_->EndFrame();
}

bool BerryClient::ReloadScript() {
  if (!script_) return false;
  const ReloadResult r = script_->Reload(false);
  if (r.attempted && !r.reloaded && !r.unchanged) {
    lastScriptError_ = r.error.message;
  }
  return r.reloaded;
}

}  // namespace Berry
