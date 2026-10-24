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

namespace {
// How often the frame poll re-queries mDNS. The cache TTL is 120s, so a query
// every 20s keeps a live peer listed without flooding the group; a peer that
// leaves simply ages out.
constexpr u64 kDiscoveryQueryIntervalMs = 20000;
}  // namespace

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
  // The local velocity source reaches the world through the same seam every
  // other read uses; the cosmetic sway and `input.velocity` both pull from it.
  velocitySource_ = std::make_unique<WorldVelocitySource>(world_, localPlayer_);
  cosmetics_->SetVelocitySource(velocitySource_.get());
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

  // The LAN channels (cosmetic sync + voice) are opt-in: each binds a private
  // multicast socket, so nothing is opened unless the config asks for it.
  BuildLan(cfg);

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
  s.velocity = velocitySource_.get();
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
  s.discovery = discovery_.get();
  s.sync = syncChannel_.get();
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

void BerryClient::BuildLan(const ClientConfig& cfg) {
  // Cosmetic sync: a private multicast transport carries the signed envelope.
  // The transport is opened first; if it cannot bind, the channel is still
  // built (so a script sees a configured-but-unreachable channel) but nothing
  // is sent or admitted until the socket is up.
  if (cfg.enableCosmeticSync) {
    syncTransport_ = std::make_unique<LanCosmeticTransport>();
    LanTransportConfig tcfg;
    if (!cfg.lanIface.empty()) tcfg.iface = cfg.lanIface;
    if (!syncTransport_->Open(tcfg)) {
      Log::Warn("BerryClient: cosmetic-sync transport failed to open "
                "(no multicast?); channel disabled");
      syncTransport_.reset();
    } else {
      syncChannel_ = std::make_unique<CosmeticSyncChannel>();
      syncChannel_->SetVerifyKey(cfg.syncVerifyKey);
      syncChannel_->SetTransport(syncTransport_.get());
      // Remember the publishing side: a tick re-broadcasts the loadout only
      // when it changes, and only when a self id + signing secret are set.
      syncSelfId_ = cfg.syncSelfId;
      syncSignSecret_ = cfg.syncSignSecret;
      syncUseHmac_ = cfg.syncUseHmac;
      Log::Info("BerryClient: cosmetic-sync channel on port %u (%s)",
                (unsigned)syncTransport_->localPort(),
                syncChannel_->configured() ? "verify key set"
                                           : "NO verify key; receives refused");
      // mDNS discovery for the same LAN: publish this client so a peer learns
      // its address and port, and browse for peers. Discovery is best-effort —
      // a socket that cannot bind leaves it null and the sync channel still
      // works over the multicast group.
      discovery_ = std::make_unique<LanDiscovery>();
      LanDiscoveryConfig dcfg;
      if (!cfg.lanIface.empty()) dcfg.iface = cfg.lanIface;
      if (discovery_->Open(dcfg)) {
        const std::string instance =
            cfg.syncSelfId.empty() ? "berry-client" : cfg.syncSelfId;
        discovery_->Publish(instance, syncTransport_->localPort(),
                            "sync=1");
        discovery_->Query();
        Log::Info("BerryClient: LAN discovery on port %u as '%s'",
                  (unsigned)discovery_->localPort(), instance.c_str());
      } else {
        Log::Warn("BerryClient: LAN discovery failed to open; peers not listed");
        discovery_.reset();
      }
    }
  }

  // Voice: bind the LAN transport and hand it to the voice service. The
  // session only starts when an id is configured; without one the transport is
  // bound but silent.
  if (cfg.enableVoice) {
    voiceTransport_ = std::make_unique<LanVoiceTransport>();
    VoiceTransportConfig vcfg;
    if (!cfg.lanIface.empty()) vcfg.iface = cfg.lanIface;
    if (!voiceTransport_->Open(vcfg)) {
      Log::Warn("BerryClient: voice transport failed to open (no multicast?); "
                "voice disabled");
      voiceTransport_.reset();
    } else {
      voice_->SetTransport(voiceTransport_.get());
      if (!cfg.voiceSelfId.empty()) {
        if (!voice_->Start(cfg.voiceSelfId)) {
          Log::Warn("BerryClient: voice session refused id '%s'",
                    cfg.voiceSelfId.c_str());
        }
      }
      Log::Info("BerryClient: voice transport on port %u (self '%s')",
                (unsigned)voiceTransport_->localPort(), cfg.voiceSelfId.c_str());
    }
  }
}

void BerryClient::PollCosmeticSync() {
  if (!syncChannel_ || !syncTransport_) return;
  // Both halves of the channel run here, on the frame thread, so the transport
  // is touched from exactly one thread (its counters and fragment state are not
  // atomic). Publish first — a local change should reach peers on the same
  // frame it happened — then drain and apply.
  PublishLoadout();
  // Keep the mDNS cache fresh: drain a bounded slice, then re-query on a slow
  // interval so peers that joined after attach are found and known peers do not
  // age out of the TTL cache. Without the re-query the peer list would be
  // correct only at attach and then decay to empty.
  if (discovery_) {
    discovery_->Poll(1);
    const u64 now = NowMs();
    if (lastDiscoveryQueryMs_ == 0 ||
        now - lastDiscoveryQueryMs_ >= kDiscoveryQueryIntervalMs) {
      discovery_->Query();
      lastDiscoveryQueryMs_ = now;
    }
  }
  // Drain the transport for a bounded slice so the frame is never held up by a
  // busy subnet, and apply each verified message. Receive runs the post-
  // signature defences (replay/rate/revoke) and the ownership gate, so a
  // forged or replayed loadout changes nothing.
  syncTransport_->Poll(
      [this](const std::string& message, const std::string&, u16) {
        size_t applied = 0;
        const CosmeticSyncResult r =
            syncChannel_->Receive(message, cosmetics_.get(), &applied);
        if (r != CosmeticSyncResult::Ok) {
          Log::Debug("BerryClient: cosmetic sync refused: %s",
                     CosmeticSyncResultName(r));
        }
      },
      2);
}

bool BerryClient::PublishLoadout() {
  if (!syncChannel_ || syncSelfId_.empty() || syncSignSecret_.empty()) {
    return false;
  }
  // Only publish a change: the canonical payload is what peers verify, so an
  // unchanged loadout would only be dropped by their replay window.
  const CosmeticLoadout loadout = cosmetics_->localLoadout();
  const std::string payload = CosmeticSyncPayload(syncSelfId_, loadout);
  if (payload == lastPublishedLoadout_) return false;
  if (!syncChannel_->Publish(syncSelfId_, loadout, syncSignSecret_,
                             syncUseHmac_)) {
    return false;
  }
  lastPublishedLoadout_ = payload;
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
  // Unbind the cosmetic seams before the owned seam objects are destroyed, so
  // the service never holds a pointer to a destroyed seam. (Its destructor does
  // not touch them, but the explicit clear keeps the ownership honest.)
  if (cosmetics_) {
    cosmetics_->SetSkeletonSource(nullptr);
    cosmetics_->SetVelocitySource(nullptr);
  }
  // Stop the backend after the renderer is no longer presenting, then release
  // the seam before the backend it points at.
  renderSeam_.reset();
  if (renderBackend_) renderBackend_->Stop();
  renderBackend_.reset();
  // Tear down the LAN channels after the services that borrow them: the voice
  // session is stopped and its transport unbound before the transport dies,
  // and the sync channel's borrowed transport pointer is cleared before the
  // transport is released.
  if (voice_) {
    voice_->Stop();
    voice_->SetTransport(nullptr);
  }
  voiceTransport_.reset();
  discovery_.reset();
  if (syncChannel_) syncChannel_->SetTransport(nullptr);
  syncChannel_.reset();
  syncTransport_.reset();
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
  // Apply any loadout that arrived since the last frame BEFORE the overlays
  // draw, so a peer's cosmetics render the same frame they were received.
  PollCosmeticSync();
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
