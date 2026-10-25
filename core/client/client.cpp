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
#include "berry/replay_service.h"
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
  // local rig instead of sitting in a table. Its base is the on-device player
  // model source (built below), so an animated bone layers on top of the real
  // rig; until the device half is installed the base is null and the animator
  // animates every entity the base would have resolved (i.e. none).
  playerModelSkeleton_ = std::make_unique<PlayerModelSkeletonSource>();
  animatorSeam_ = std::make_unique<AnimatedSkeletonSource>(
      playerModelSkeleton_.get(), animator_.get());
  cosmetics_->SetSkeletonSource(animatorSeam_.get());
  // The camera source lets a device render seam project real geometry through
  // the frame's own camera; the renderer is bound once the engine is up (below).
  cosmeticCamera_ = std::make_unique<RendererCameraSource>();
  cosmetics_->SetCameraSource(cosmeticCamera_.get());
  // Per-asset custom animations sample from the same clip store a script
  // authors: the adapter is the seam the cosmetic renderer reads a named clip
  // through, so an asset's `animation` moves it on its own bone. Without it
  // every asset is placed rigidly.
  animationSource_ = std::make_unique<AnimatorAnimationSource>(animator_.get());
  cosmetics_->SetAnimationSource(animationSource_.get());
  // The on-device mesh backend and the seam that resolves a draw item's
  // geometry/texture handles against the asset store. On the host the backend
  // is the null stub: the seam then drops every item (fail closed), which is
  // the correct headless behaviour. The asset store itself is process-wide
  // (`CosmeticAssetStore::Instance()`), so the seam borrows it directly.
  cosmeticMeshBackend_.reset(CreateCosmeticMeshBackend());
  if (cosmeticMeshBackend_ && cosmeticMeshBackend_->Init()) {
    cosmeticRenderSeam_ = std::make_unique<DeviceCosmeticRenderSeam>(
        &CosmeticAssetStore::Instance(), cosmeticMeshBackend_.get());
    cosmetics_->SetRenderSeam(cosmeticRenderSeam_.get());
    Log::Info("BerryClient: cosmetic mesh backend '%s' ready",
              cosmeticMeshBackend_->name());
  } else {
    // No mesh backend: leave the service's seam unset rather than installing a
    // seam that can only drop. A script still authors cosmetics; nothing draws
    // on a headless host.
    cosmeticMeshBackend_.reset();
    Log::Info("BerryClient: no cosmetic mesh backend (headless); cosmetics "
              "are authored but not drawn");
  }
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

  // OTA signature packs load before the script: a pack's signature rows and
  // struct layouts must be registered by the time a script asks the signature
  // cache or a layout to resolve. Verification happens inside the loader.
  LoadSigPacks(cfg);

  // Replay guards before anything can start a playback: the capture's
  // fingerprint and the loopback server's protocol/live checks, so a replay
  // from another build or a live session cannot feed the game's parsers.
  InstallReplayGuards(cfg);

  // Cosmetic asset data and model packs load, and the player-model hook is
  // installed, before the script: a script that equips a pack asset this frame
  // needs the asset's geometry handle and the pack's catalog entry to already
  // exist, and the hook must be live before the first rendered frame resolves
  // bones.
  BuildCosmetics(cfg);

  // Register the built-in starter assets so a fresh client has something to
  // show. Idempotent; a second call adds nothing.
  CosmeticRegistry::Instance().RegisterBuiltins();

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

  // Bind the sandboxed fs store to the script's own directory, so its data
  // survives a restart. Runs after the script loads (the directory name comes
  // from the manifest) and before the worker starts, so the first script call
  // already sees its persisted files.
  BindScriptData(cfg);

  // The worker runs the script. It is started even when no script loaded, so
  // `Tick` can post and drain uniformly.
  worker_ = std::make_unique<ScriptWorker>();
  worker_->Start();

  // Run the script's on_load on the worker before any tick, so a subscription
  // or one-time setup it registers exists before the first tick's events. A
  // missing on_load is not an error (most scripts implement only a subset).
  if (script_ && script_->host()) {
    worker_->Post([this]() {
      ThreadModel::Instance().AssertScripts();
      ScriptHost* host = script_ ? script_->host() : nullptr;
      if (!host || !host->loaded()) return;
      host->RunHook(ScriptHooks::OnLoad());
    });
  }

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

void BerryClient::BindScriptData(const ClientConfig& cfg) {
  if (cfg.scriptDataDirectory.empty() || !script_ || !script_->valid()) return;
  // One directory per script, named from a hash of the manifest name so a
  // name with a path separator or a dot cannot climb out of the root, and two
  // scripts never collide on the same sandbox. The name is not the manifest
  // name itself: that is attacker-controlled text, and a directory name must
  // be safe by construction.
  const std::string id = crypto::Sha256Hex(script_->manifest().name);
  const std::string dir = cfg.scriptDataDirectory + "/" + id.substr(0, 32);
  if (!files_->Open(dir)) {
    // A store that cannot persist must not silently become volatile: leave it
    // in-memory (the script gets no persistence) and say so.
    Log::Warn("BerryClient: script data directory '%s' unavailable; "
              "fs.* stays in-memory",
              dir.c_str());
    return;
  }
  Log::Info("BerryClient: script data bound to '%s' (%zu files, %zu bytes)",
            dir.c_str(), files_->FileCount(), files_->TotalBytes());
}

void BerryClient::BuildLan(const ClientConfig& cfg) {
  // Cosmetic sync: a transport carries the signed envelope to peers. Two are
  // possible: the LAN multicast transport (same subnet) and the relay transport
  // (the presence backend, reaching another network). Exactly one is built. The
  // transport is opened first; if it cannot bind, the channel is still built (so
  // a script sees a configured-but-unreachable channel) but nothing is sent or
  // admitted until the socket is up.
  if (cfg.enableCosmeticSync) {
    const bool useRelay = !cfg.syncRelayHost.empty();
    CosmeticSyncTransport* transport = nullptr;
    if (useRelay) {
      syncRelay_ = std::make_unique<CosmeticRelayTransport>();
      RelayCosmeticConfig rcfg;
      rcfg.relayHost = cfg.syncRelayHost;
      rcfg.relayPort = cfg.syncRelayPort;
      rcfg.room = cfg.syncRoom.empty() ? "default" : cfg.syncRoom;
      rcfg.roomSecret = cfg.syncRoomSecret;
      if (!syncRelay_->Open(rcfg)) {
        Log::Warn("BerryClient: cosmetic-sync relay failed to open "
                  "(room secret too short or bad relay?); channel disabled");
        syncRelay_.reset();
      } else {
        transport = syncRelay_.get();
      }
    } else {
      syncTransport_ = std::make_unique<LanCosmeticTransport>();
      LanTransportConfig tcfg;
      if (!cfg.lanIface.empty()) tcfg.iface = cfg.lanIface;
      if (!syncTransport_->Open(tcfg)) {
        Log::Warn("BerryClient: cosmetic-sync transport failed to open "
                  "(no multicast?); channel disabled");
        syncTransport_.reset();
      } else {
        transport = syncTransport_.get();
      }
    }

    if (transport != nullptr) {
      syncChannel_ = std::make_unique<CosmeticSyncChannel>();
      syncChannel_->SetVerifyKey(cfg.syncVerifyKey);
      syncChannel_->SetTransport(transport);
      // Remember the publishing side: a tick re-broadcasts the loadout only
      // when it changes, and only when a self id + signing secret are set.
      syncSelfId_ = cfg.syncSelfId;
      syncSignSecret_ = cfg.syncSignSecret;
      syncUseHmac_ = cfg.syncUseHmac;
      const u16 localPort =
          useRelay ? syncRelay_->localPort() : syncTransport_->localPort();
      Log::Info("BerryClient: cosmetic-sync channel on port %u via %s (%s)",
                (unsigned)localPort, useRelay ? "relay" : "multicast",
                syncChannel_->configured() ? "verify key set"
                                           : "NO verify key; receives refused");
      // mDNS discovery is LAN-only: a relay client is reached by the relay, not
      // by a subnet peer, so discovery is not built for it.
      if (!useRelay) {
        discovery_ = std::make_unique<LanDiscovery>();
        LanDiscoveryConfig dcfg;
        if (!cfg.lanIface.empty()) dcfg.iface = cfg.lanIface;
        if (discovery_->Open(dcfg)) {
          const std::string instance =
              cfg.syncSelfId.empty() ? "berry-client" : cfg.syncSelfId;
          discovery_->Publish(instance, syncTransport_->localPort(), "sync=1");
          discovery_->Query();
          Log::Info("BerryClient: LAN discovery on port %u as '%s'",
                    (unsigned)discovery_->localPort(), instance.c_str());
        } else {
          Log::Warn(
              "BerryClient: LAN discovery failed to open; peers not listed");
          discovery_.reset();
        }
      }
    }
  }

  // Voice: bind the authenticated, encrypted transport and hand it to the voice
  // service. The secure transport refuses to open without a signing identity and
  // a trust anchor, so voice never falls back to plaintext; without them it is
  // simply disabled. The session only starts when an id is configured; without
  // one the transport is bound but silent.
  if (cfg.enableVoice) {
    VoiceIdentity voiceId;
    if (!cfg.voiceIdentitySeed.empty()) {
      std::string seed = cfg.voiceIdentitySeed;
      // Accept a 64-char hex seed as well as raw 32 bytes.
      if (seed.size() == 64 && crypto::IsHex(seed)) {
        std::string raw;
        if (crypto::FromHex(seed, &raw, 32)) seed = raw;
      }
      voiceId = VoiceIdentity::FromSeed(seed);
      if (!voiceId.valid()) {
        Log::Warn("BerryClient: voice identity seed is not 32 bytes; "
                  "voice disabled");
      }
    }
    if (!voiceId.valid() || !cfg.voiceTrust.Valid()) {
      Log::Warn("BerryClient: voice needs a signing identity and a trust "
                "anchor; voice disabled (no plaintext fallback)");
    } else {
      secureVoice_ = std::make_unique<SecureVoiceTransport>();
      SecureVoiceConfig vcfg;
      vcfg.group = cfg.voiceGroup;
      vcfg.port = cfg.voicePort;
      if (!cfg.lanIface.empty()) vcfg.iface = cfg.lanIface;
      if (!secureVoice_->Open(vcfg, voiceId, cfg.voiceTrust)) {
        Log::Warn("BerryClient: secure voice transport failed to open; "
                  "voice disabled");
        secureVoice_.reset();
      } else {
        voice_->SetTransport(secureVoice_.get());
        if (!cfg.voiceSelfId.empty()) {
          if (!voice_->Start(cfg.voiceSelfId)) {
            Log::Warn("BerryClient: voice session refused id '%s'",
                      cfg.voiceSelfId.c_str());
          }
        }
        Log::Info("BerryClient: secure voice on port %u (self '%s')",
                  (unsigned)secureVoice_->localPort(), cfg.voiceSelfId.c_str());
      }
    }
  }
}

u64 BerryClient::sigPackSequenceFloor() const {
  return sigPackLoader_ ? sigPackLoader_->sequenceFloor() : 0;
}

void BerryClient::LoadSigPacks(const ClientConfig& cfg) {
  if (!cfg.enableSigPacks || cfg.sigpackDirectory.empty()) return;
  // The loader verifies every pack against the anchor BEFORE parsing it, so a
  // tampered, unsigned, expired or replayed pack never reaches the entry
  // parser. The floor is seeded from the config so a restart cannot be rolled
  // back onto an older pack.
  sigPackFs_ = std::make_unique<RealScriptFileSystem>();
  sigPackLayouts_ = std::make_unique<LayoutRegistry>();
  sigPackLoader_ =
      std::make_unique<SignaturePackLoader>(sigPackFs_.get(), cfg.sigpackTrust,
                                            cfg.sigpackDirectory);
  sigPackLoader_->SetSequenceFloor(cfg.sigpackSequenceFloor);
  sigPackReport_ = sigPackLoader_->LoadDirectory(
      cfg.sigpackBuild, SigCache::Instance(), sigPackLayouts_.get());
  Log::Info("BerryClient: signature packs: %zu applied, %zu refused "
            "(floor %llu)",
            sigPackReport_.packsApplied, sigPackReport_.packsFailed,
            (unsigned long long)sigPackLoader_->sequenceFloor());
}

void BerryClient::InstallReplayGuards(const ClientConfig& cfg) {
  ReplayService& replay = engine_->Replay();
  // The capture stamps every recording with this build's protocol version, so a
  // replay made against another build is refused on load by the check below.
  ReplayFingerprint fp = replay.capture().fingerprint();
  fp.protocolVersion = cfg.protocolVersion;
  replay.capture().SetFingerprint(fp);
  // The service enforces the version on Load/Attach and answers the live-session
  // probe on every start/stream, so both guards reach the server and the loader
  // through one place rather than each call site.
  replay.SetExpectedProtocolVersion(cfg.protocolVersion);
  replay.SetLiveSessionProbe(cfg.liveSessionActive);
}

void BerryClient::BuildCosmetics(const ClientConfig& cfg) {
  // Cosmetic asset data loads first: a pack names geometry/texture handles, so
  // the handles must exist before the pack's assets are registered (otherwise a
  // pack asset would resolve to a retired handle and draw nothing).
  if (!cfg.cosmeticAssetDirectory.empty()) {
    cosmeticAssets_ = std::make_unique<CosmeticAssetLoader>(
        fs_.get(), cfg.cosmeticAssetDirectory);
    const auto report = cosmeticAssets_->LoadDirectory();
    Log::Info("BerryClient: cosmetic assets: %zu meshes, %zu textures, "
              "%zu refused%s",
              report.meshes, report.textures, report.failed,
              report.truncated ? " (truncated)" : "");
  }

  // Model packs register into the catalog the wardrobe reads. Loading after the
  // assets means a pack's handles resolve; a pack that names a handle no asset
  // supplied still registers (the asset simply draws nothing), which keeps a
  // missing texture from discarding a whole cape.
  if (!cfg.cosmeticPackDirectory.empty()) {
    packs_->SetRoot(cfg.cosmeticPackDirectory);
    const PackLoadReport report = packs_->LoadDirectory();
    Log::Info("BerryClient: cosmetic packs: %zu loaded, %zu refused%s",
              report.packsLoaded, report.packsFailed,
              report.truncated ? " (truncated)" : "");
  }

  // Author the built-in clips into the animator, so the built-in cape flaps and
  // the aura spins out of the box. A pack or a script can override a clip by
  // name; the built-ins are added only when the name is free, so a creator's
  // clip of the same name wins and the built-in never clobbers it.
  RegisterBuiltinClips();

  // The player-model hook is what makes the bones real: it refreshes the
  // per-build reader's cache from the rendered pose, so the cosmetic renderer
  // resolves bones the game actually computed. A missing target or reader
  // leaves the skeleton source fail-closed (nothing resolves), which is the
  // correct default for a build whose layout is unknown.
  if (cfg.playerModelRenderTarget && cfg.playerModelBones.valid()) {
    playerModelSkeleton_->SetReader(cfg.playerModelBones);
    playerModelHook_ = std::make_unique<PlayerModelHook>();
    playerModelHook_->SetReader(cfg.playerModelBones);
    if (playerModelHook_->Install(cfg.playerModelRenderTarget,
                                  HookManager::Instance().Backend())) {
      Log::Info("BerryClient: player-model hook installed");
    } else {
      Log::Warn("BerryClient: player-model hook refused; cosmetics resolve no "
                "bones");
      playerModelHook_.reset();
    }
  } else {
    Log::Info("BerryClient: no player-model target/reader; cosmetics resolve "
              "no bones (fail closed)");
  }
}

void BerryClient::RegisterBuiltinClips() {
  if (!animator_) return;
  // The cape's flap: the back bone lifts and rolls a little over a loop. Keys
  // are authored at the extremes and the midpoint so the lerp reads as a slow
  // flap rather than a triangle wave. Registered only when the name is free, so
  // a creator pack or a script that authors `builtin.cape.flap` wins.
  if (!animator_->hasClip("builtin.cape.flap")) {
    if (animator_->AddClip("builtin.cape.flap", 2.0f, true)) {
      animator_->AddKey("builtin.cape.flap", Bone::Back, 0.0f, Vec3(0, 0, 0),
                        Vec3(0, 0, 0), 1.0f);
      animator_->AddKey("builtin.cape.flap", Bone::Back, 1.0f, Vec3(0, 0, -0.08f),
                        Vec3(-12.0f, 0, 4.0f), 1.0f);
      animator_->AddKey("builtin.cape.flap", Bone::Back, 2.0f, Vec3(0, 0, 0),
                        Vec3(0, 0, 0), 1.0f);
    }
  }
  // The aura's spin: the torso bone rolls a full turn over a loop, so a
  // billboard aura reads as a slow rotation. A one-shot would stop at the end
  // and then hold; a loop keeps it turning.
  if (!animator_->hasClip("builtin.aura.spin")) {
    if (animator_->AddClip("builtin.aura.spin", 3.0f, true)) {
      animator_->AddKey("builtin.aura.spin", Bone::Torso, 0.0f, Vec3(0, 0, 0),
                        Vec3(0, 0, 0), 1.0f);
      animator_->AddKey("builtin.aura.spin", Bone::Torso, 1.5f, Vec3(0, 0.05f, 0),
                        Vec3(0, 0, 180.0f), 1.1f);
      animator_->AddKey("builtin.aura.spin", Bone::Torso, 3.0f, Vec3(0, 0, 0),
                        Vec3(0, 0, 360.0f), 1.0f);
    }
  }
}

void BerryClient::PollCosmeticSync() {
  if (!syncChannel_ || (!syncTransport_ && !syncRelay_)) return;
  PublishLoadout();
  // Keep the mDNS cache fresh: drain a bounded slice, then re-query on a slow
  // interval so peers that joined after attach are found and known peers do not
  // age out of the TTL cache. Without the re-query the peer list would be
  // correct only at attach and then decay to empty. A relay client has no
  // discovery (it is reached by the relay), so this is a no-op for it.
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
  const auto onMessage = [this](const std::string& message,
                                const std::string&, u16) {
    size_t applied = 0;
    const CosmeticSyncResult r =
        syncChannel_->Receive(message, cosmetics_.get(), &applied);
    if (r != CosmeticSyncResult::Ok) {
      Log::Debug("BerryClient: cosmetic sync refused: %s",
                 CosmeticSyncResultName(r));
    }
  };
  if (syncRelay_) {
    syncRelay_->Poll(onMessage, 2);
  } else {
    syncTransport_->Poll(onMessage, 2);
  }
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
    // The animation source is borrowed by the service and owns nothing, but the
    // clear keeps the seam honest before the adapter (and its Animator) die.
    cosmetics_->SetAnimationSource(nullptr);
    cosmetics_->SetRenderSeam(nullptr);
    cosmetics_->SetCameraSource(nullptr);
  }
  // The player-model hook must be uninstalled before its detour target and the
  // skeleton source it feeds are released, or the game would call into freed
  // memory.
  if (playerModelHook_) playerModelHook_->Uninstall();
  playerModelHook_.reset();
  playerModelSkeleton_.reset();
  animationSource_.reset();
  cosmeticRenderSeam_.reset();
  cosmeticMeshBackend_.reset();
  cosmeticAssets_.reset();
  cosmeticCamera_.reset();
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
  secureVoice_.reset();
  voiceTransport_.reset();
  discovery_.reset();
  if (syncChannel_) syncChannel_->SetTransport(nullptr);
  syncChannel_.reset();
  syncTransport_.reset();
  syncRelay_.reset();
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
  // Deliver any persistent-subscription events the engine queued since the last
  // tick. Runs after on_tick so a hook that subscribes this tick is live for
  // the next one, and it is a no-op when nothing subscribed. A host the tick
  // faulted is unloaded, so this returns immediately.
  if (host->loaded() && !host->suspended()) {
    api_->bus().Pump(*host);
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
  if (r.reloaded) {
    // A new revision may have renamed or dropped the functions old
    // subscriptions point at, so the previous revision's subscriptions are
    // dropped and the fresh host re-registers through on_load. The reload
    // swaps the host on the worker's timeline; posting the on_load keeps the
    // re-registration on the worker with every other script call.
    const std::string name = script_->manifest().name;
    worker_->Post([this, name]() {
      ThreadModel::Instance().AssertScripts();
      api_->bus().OffAll(name);
      ScriptHost* host = script_ ? script_->host() : nullptr;
      if (!host || !host->loaded()) return;
      host->RunHook(ScriptHooks::OnLoad());
    });
  }
  return r.reloaded;
}

}  // namespace Berry
