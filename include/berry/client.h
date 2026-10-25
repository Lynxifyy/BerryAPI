#pragma once
// BerryClient — the composition root that makes the client surface real.
//
// Every client-side service (the immediate-mode UI, the HUD, the PvP overlay,
// the radial menu, the animator, the crystal advisor, the voice session, the
// combat-state tracker, the shader/package/module registries, the sandboxed
// file store, the audio cues, the packet log, the WebSocket pool, the replay
// camera, the intent validator and the replay service) is declared and
// implemented, but nothing in the engine owned them or built the script host
// that reaches them: a `ScriptApi::Services` was only ever assembled by a test.
// This is the production owner.
//
// It owns, in dependency order:
//
//   1. the services themselves (one instance each, for the client lifetime);
//   2. the `ScriptApi` that binds them to the IDL surface;
//   3. the `ScriptBindings` the VM consults;
//   4. the async runtime (awaits + the scheduler), installed on the same
//      bindings;
//   5. the `ScriptLoader` that is the only thing allowed to build a host, and
//      the loaded script it produces;
//   6. the `ScriptWorker` the script runs on.
//
// Threading follows docs/THREADING.md. `Tick()` and `Frame()` are called from
// the engine's game-tick and render threads respectively; the script itself
// runs only on the worker. Nothing here reads game memory: the services reach
// the world through the same `WorldHandle` seam every other consumer uses.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/api_bindings.h"
#include "berry/async.h"
#include "berry/animator.h"
#include "berry/audio.h"
#include "berry/combat_state.h"
#include "berry/cosmetic.h"
#include "berry/cosmetic_assets.h"
#include "berry/cosmetic_device.h"
#include "berry/cosmetic_sync.h"
#include "berry/crystal.h"
#include "berry/event.h"
#include "berry/fs.h"
#include "berry/hud.h"
#include "berry/input.h"
#include "berry/intent.h"
#include "berry/lan_discovery.h"
#include "berry/lan_transport.h"
#include "berry/loader.h"
#include "berry/pack.h"
#include "berry/package.h"
#include "berry/packet.h"
#include "berry/perf.h"
#include "berry/pvp.h"
#include "berry/radial.h"
#include "berry/render.h"
#include "berry/render_backend.h"
#include "berry/replay_camera.h"
#include "berry/script.h"
#include "berry/scriptmod.h"
#include "berry/services.h"
#include "berry/shader.h"
#include "berry/sigpack.h"
#include "berry/ui.h"
#include "berry/url.h"
#include "berry/voice.h"
#include "berry/voice_crypto.h"
#include "berry/voice_secure_transport.h"
#include "berry/voice_transport.h"
#include "berry/ws_client.h"

#include <functional>
#include <memory>
#include <string>

namespace Berry {

class Engine;

//----------------------------------------------------------------------------
// Configuration.
//----------------------------------------------------------------------------

struct BERRY_API ClientConfig {
  /// Where scripts are loaded from. Empty disables script loading entirely
  /// (the services still run; only the script host is absent).
  std::string scriptDirectory;
  /// The manifest trust anchor. The default (an empty anchor) verifies nothing,
  /// so a signed manifest is refused; production sets an Ed25519 key.
  TrustAnchor anchor;
  /// Require a verified signature + a bound source hash (production). Dev mode
  /// turns both off for local iteration.
  bool requireSignature = true;
  bool requireSourceHash = true;
  /// The local player's entity id, used as the origin for every player/world
  /// read and the voice proximity check. 0 means "unknown", which makes those
  /// reads fail closed.
  EntityId localPlayer;
  /// Egress policy for the network/async bindings. The default is the safe
  /// policy (https only, no private ranges).
  UrlPolicy networkPolicy;
  /// The surface size in physical pixels, read once per frame. The device
  /// embedder supplies the real surface size (from the EGL surface); when it is
  /// empty the client uses the renderer's last-known size, and a zero size
  /// makes the overlays skip the frame rather than draw at a bogus scale.
  std::function<Vec2()> screenSource;
  /// Enable the LAN cosmetic-sync channel: a private multicast transport that
  /// carries the local loadout and applies verified remote ones. Off by
  /// default (a client renders its own cosmetics on its own screen); when on,
  /// `syncVerifyKey` must be set or the channel refuses every inbound message.
  bool enableCosmeticSync = false;
  /// The interface the LAN channels join and send on. "0.0.0.0" (the default)
  /// uses the system multicast route; a host test sets "127.0.0.1" to stay on
  /// loopback.
  std::string lanIface = "0.0.0.0";
  /// The key used to verify inbound cosmetic loadouts. Without it the channel
  /// is not configured and Receive fails closed.
  CosmeticSyncKey syncVerifyKey;
  /// This client's own id on the sync channel (the name peers see). Empty
  /// disables publishing (the client still verifies and applies inbound ones).
  std::string syncSelfId;
  /// The secret used to sign the local loadout before publishing. A 64-byte
  /// Ed25519 secret as hex, or an HMAC secret with `syncUseHmac`. Empty
  /// disables publishing.
  std::string syncSignSecret;
  bool syncUseHmac = false;
  /// Enable LAN proximity voice. Off by default (microphone capture is
  /// privacy-sensitive); when on, the client binds an *authenticated and
  /// encrypted* voice transport (`SecureVoiceTransport`) to the voice service
  /// and starts a session only if `voiceSelfId` is set. The transport requires a
  /// signing identity and a trust anchor (`voiceIdentitySeed` + `voiceTrust`);
  /// without them it refuses to open, so voice never falls back to plaintext.
  bool enableVoice = false;
  /// This client's voice session id. Empty leaves the session stopped (the
  /// transport is bound but nothing is sent or admitted).
  std::string voiceSelfId;
  /// The 32-byte Ed25519 seed (raw, or 64-char hex) that identifies this client
  /// on the voice channel. It authenticates the handshake; it never encrypts
  /// audio. Empty disables the secure transport.
  std::string voiceIdentitySeed;
  /// What a peer's voice announce is verified against (Ed25519 peer keys, or a
  /// backend-issued room key). The default accepts nothing, so voice stays off
  /// until it is set.
  VoiceTrustAnchor voiceTrust;
  /// The voice multicast group and port. A different group from the cosmetic
  /// channel so the two never mix.
  std::string voiceGroup = "239.66.0.2";
  u16 voicePort = 47824;
  /// Enable OTA signature-pack loading from `sigpackDirectory`. Packs are
  /// verified against `sigpackTrust` (Ed25519) BEFORE they are parsed, so an
  /// unsigned, tampered, expired or replayed pack is refused. Off by default;
  /// when on, `sigpackTrust` must carry a key or every pack is refused.
  bool enableSigPacks = false;
  /// The directory `*.sigpack.json` files are read from. Empty disables the
  /// load even when `enableSigPacks` is set.
  std::string sigpackDirectory;
  /// The Ed25519 anchor packs are verified against, plus the anti-rollback
  /// floor and the wall clock used for the signed expiry check.
  SigPackTrust sigpackTrust;
  /// The active build fingerprint packs must name to apply. Empty refuses
  /// every pack (fail closed), matching the signature-table gate.
  std::string sigpackBuild;
  /// The highest pack sequence applied by a previous run, so a restart cannot
  /// be rolled back onto an older pack. Persisted by the embedder.
  u64 sigpackSequenceFloor = 0;
  /// Root directory for the sandboxed `fs.*` store. When set, the client binds
  /// the store to a per-script subdirectory (named from the loaded manifest) so
  /// data survives a restart; when empty the store stays in-memory and a script
  /// gets no persistence. The store refuses symlinks on every path component and
  /// writes through a temp file plus rename, so a persistence attempt cannot
  /// escape the directory or observe a partial write.
  std::string scriptDataDirectory;
  /// Root directory for cosmetic model packs (`*.pack.json`). When set, the
  /// client loads every pack at attach and on `cosmetic.reload_packs`, so a
  /// creator's assets appear without a new build. Empty disables pack loading
  /// (the built-ins still register).
  std::string cosmeticPackDirectory;
  /// Root directory for cosmetic asset data (`*.mesh.json` / `*.tex.json`).
  /// When set, the client loads every mesh and texture at attach, so a pack's
  /// `geometry`/`texture` handles resolve to real data. Empty leaves the asset
  /// store empty (a pack asset then draws nothing).
  std::string cosmeticAssetDirectory;
  /// The game's player-render function, resolved by the embedder (a signature
  /// scan against the target build). When non-null, the client installs the
  /// player-model hook and fills the per-build bone reader, so cosmetics attach
  /// to the rendered rig. Null (the default) leaves the skeleton seam without a
  /// reader, which resolves nothing — the correct fail-closed default for a
  /// build whose layout is unknown.
  void* playerModelRenderTarget = nullptr;
  /// The per-build bone reader the hook refreshes. The embedder fills it from
  /// the resolved player-model layout. An empty reader keeps the cosmetic
  /// skeleton seam fail-closed even when `playerModelRenderTarget` is set.
  PlayerModelBoneReader playerModelBones;
};

//----------------------------------------------------------------------------
// WorldVelocitySource — the production LocalVelocitySource.
//----------------------------------------------------------------------------

/// Reports the local player's velocity from the world seam (the
/// `EntitySnapshot.velocity` every other consumer reads). This is the
/// production implementation of `LocalVelocitySource`: the cosmetic sway and
/// the `input.velocity` binding both pull from it, and it reaches the world the
/// same way everything else does, never game memory. Returns false when the
/// world or the local entity is unknown, so consumers treat the player as
/// stationary rather than using a stale vector.
class BERRY_API WorldVelocitySource final : public LocalVelocitySource {
 public:
  WorldVelocitySource() = default;
  WorldVelocitySource(WorldHandle* world, EntityId local)
      : world_(world), local_(local) {}

  void SetWorld(WorldHandle* world) { world_ = world; }
  void SetLocal(EntityId local) { local_ = local; }

  bool LocalVelocity(Vec3* out) const override {
    if (!world_) return false;
    const EntitySnapshot s = world_->GetEntity(local_);
    if (!s.alive) return false;
    if (out) *out = s.velocity;
    return true;
  }

 private:
  WorldHandle* world_ = nullptr;
  EntityId local_;
};

//----------------------------------------------------------------------------
// RendererCameraSource — the production CosmeticCameraSource.
//----------------------------------------------------------------------------

/// Reports the frame camera from the Renderer (the same camera the overlay
/// projects with), so a device cosmetic render seam draws real geometry through
/// the frame's own projection. Returns false when no renderer is bound or its
/// camera is invalid, so a consumer treats the frame as unprojectable rather
/// than using a stale one.
class BERRY_API RendererCameraSource final : public CosmeticCameraSource {
 public:
  RendererCameraSource() = default;
  explicit RendererCameraSource(Renderer* r) : renderer_(r) {}

  void SetRenderer(Renderer* r) { renderer_ = r; }

  bool Camera(CameraState* out) const override {
    if (!renderer_) return false;
    const CameraState cam = renderer_->camera();
    if (!cam.valid) return false;
    if (out) *out = cam;
    return true;
  }

 private:
  Renderer* renderer_ = nullptr;
};

//----------------------------------------------------------------------------
// BerryClient.
//----------------------------------------------------------------------------

class BERRY_API BerryClient {
 public:
  BerryClient();
  ~BerryClient();

  BerryClient(const BerryClient&) = delete;
  BerryClient& operator=(const BerryClient&) = delete;

  /// Binds the world handle the services read through (from the World module)
  /// and, when a script directory is configured, builds the host.
  ///
  /// Returns false only when a configured script directory fails to produce a
  /// host; the services are still fully usable (a script is optional). Call
  /// after `Engine::Attach` so the world module is attached and its service is
  /// reachable.
  bool Attach(Engine& engine, const ClientConfig& cfg);
  void Detach();

  bool attached() const { return attached_; }

  //--- per-frame drivers ---------------------------------------------------
  //
  // The engine calls these from the two threads the threading model defines:
  // `Tick` on the game-tick thread (once per simulation tick), `Frame` on the
  // render thread (once per presented frame).

  /// Game-tick driver: posts the script's tick work (on_tick + the async pump)
  /// to the worker. Safe to call when no script is loaded (a no-op).
  void Tick(GameTick tick);

  /// Opens the render frame: brackets `Renderer::BeginFrame` on the Render
  /// module's renderer (the production caller that `Renderer` otherwise lacked)
  /// and publishes the frame timing the synchronous script reads answer from
  /// (which also drives the governor, the pipeline and the HUD clock). Call
  /// before emitting `berry:frame`.
  void BeginFrame(f32 frameMs, const Vec2& screen);

  /// Closes the render frame: `Renderer::EndFrame`, presenting the batch the
  /// overlays queued. Call after `berry:frame` has been emitted.
  void EndFrame();

  /// The frame's surface size, resolved from the configured source or the
  /// renderer's last-known size (0,0 when neither is available).
  Vec2 screenSize() const;

  /// The renderer owned by the Render module, or null before Attach. The frame
  /// bracket draws through it.
  Renderer* renderer() { return renderer_; }

  //--- services (borrowed accessors) ---------------------------------------

  ScriptApi& api() { return *api_; }
  const ScriptApi& api() const { return *api_; }
  ScriptBindings& bindings() { return bindings_; }
  UiLayer& ui() { return *ui_; }
  Hud& hud() { return *hud_; }
  PvpOverlay& pvp() { return *pvp_; }
  RadialMenu& radial() { return *radial_; }
  Animator& animator() { return *animator_; }
  CombatStateTracker& combatState() { return *combatState_; }
  CosmeticService& cosmetics() { return *cosmetics_; }
  PerformanceGovernor& governor() { return *governor_; }
  InputObserver& input() { return *input_; }
  WsClientPool& wsPool() { return *wsPool_; }
  ReplayCamera& replayCamera() { return *replayCamera_; }
  CrystalOptimizer& crystal() { return *crystal_; }
  VoiceService& voice() { return *voice_; }
  ScriptModuleRegistry& modules() { return *modules_; }
  ShaderRegistry& shaders() { return *shaders_; }
  PackageRegistry& packages() { return *packages_; }
  ScriptFileStore& files() { return *files_; }
  AudioService& audio() { return *audio_; }
  PacketLog& packets() { return *packets_; }
  IntentValidator& intents() { return *intents_; }
  CosmeticPackLoader& packs() { return *packs_; }
  /// The cosmetic asset loader (mesh/texture store), or null when no asset
  /// directory is configured. For diagnostics and tests.
  CosmeticAssetLoader* cosmeticAssets() { return cosmeticAssets_.get(); }
  /// The cosmetic asset store: the process-wide mesh/texture catalog the render
  /// seam resolves a draw item's handles against.
  static CosmeticAssetStore& assets() { return CosmeticAssetStore::Instance(); }
  AsyncRuntime& async() { return *async_; }

  /// The LAN cosmetic-sync channel, or null when sync is disabled. Owns the
  /// multicast transport when enabled.
  CosmeticSyncChannel* cosmeticSync() { return syncChannel_.get(); }
  /// The mDNS discovery the sync channel uses, or null when sync is disabled or
  /// discovery could not bind. Reads only: discovery names peers, and a loadout
  /// from one is still verified by the sync channel before it is applied.
  LanDiscovery* discovery() { return discovery_.get(); }
  /// The LAN voice transport, or null when voice is disabled. In production this
  /// is the authenticated, encrypted `SecureVoiceTransport`.
  VoiceTransport* voiceTransport() { return voiceTransport_.get(); }
  /// The secure voice transport specifically, or null when it is not the active
  /// transport. For diagnostics (peer count) and tests.
  SecureVoiceTransport* secureVoiceTransport() { return secureVoice_.get(); }

  /// The signature-pack loader, or null when packs are disabled. Owns the
  /// filesystem seam and the anti-rollback sequence floor.
  SignaturePackLoader* sigPacks() { return sigPackLoader_.get(); }
  /// The layout registry packs register into, or null when packs are disabled.
  LayoutRegistry* sigPackLayouts() { return sigPackLayouts_.get(); }
  /// The aggregate of the last pack load (all zero when packs are disabled).
  const SigPackLoadReport& sigPackReport() const { return sigPackReport_; }
  /// The highest pack sequence applied so far, so an embedder can persist it and
  /// refuse a rollback across a restart. 0 when packs are disabled.
  u64 sigPackSequenceFloor() const;

  /// Broadcasts the local cosmetic loadout on the sync channel. False when
  /// sync is disabled, no signing secret/self id is configured, or the send
  /// fails. Called by the frame poll, so a loadout change reaches peers without
  /// a script having to ask.
  bool PublishLoadout();

  /// The loaded script, or null when none is configured / the load failed.
  LoadedScript* script() { return script_.get(); }

  /// Runs one pending reload check and returns whether the script changed. The
  /// loader re-verifies, so an edited file that no longer matches its signed
  /// hash is refused and the previous revision stays live.
  bool ReloadScript();

  /// Reports why the last script load/reload failed, if it did. Empty on
  /// success or when no script is configured.
  const std::string& lastScriptError() const { return lastScriptError_; }

 private:
  /// Wires the world handle and builds the ScriptApi::Services from the owned
  /// services.
  void BuildServices();

  /// Builds the host from `cfg.scriptDirectory` through the loader.
  bool LoadScript(const ClientConfig& cfg);

  /// Builds the opt-in LAN channels: the cosmetic-sync channel (with its
  /// multicast transport) and the LAN voice transport. A no-op for a config
  /// that enables neither.
  void BuildLan(const ClientConfig& cfg);

  /// Builds the signature-pack loader and applies every verified pack. A no-op
  /// when packs are disabled. Runs before the script loads, so a pack's
  /// signatures/layouts are in the registries by the time a script asks.
  void LoadSigPacks(const ClientConfig& cfg);

  /// Binds the sandboxed `fs.*` store to the script's own directory under
  /// `cfg.scriptDataDirectory`, so data survives a restart. A no-op (the store
  /// stays in-memory) when no directory is configured or no script loaded. The
  /// per-script directory name is derived from the loaded manifest, so two
  /// scripts never share a sandbox.
  void BindScriptData(const ClientConfig& cfg);

  /// Loads cosmetic model packs from `cfg.cosmeticPackDirectory` and cosmetic
  /// asset data from `cfg.cosmeticAssetDirectory`, and installs the player-model
  /// hook (with `cfg.playerModelBones`) on `cfg.playerModelRenderTarget`. A no-op
  /// for each half the config leaves empty; each fails closed on its own.
  void BuildCosmetics(const ClientConfig& cfg);

  /// Authors the built-in animation clips into the animator, so a built-in cape
  /// flaps and a built-in aura spins without a script or a pack. A clip is added
  /// only when its name is free, so a creator's clip of the same name wins.
  void RegisterBuiltinClips();

  /// Drains the sync transport and applies every verified loadout to the
  /// cosmetic service. Runs on the frame thread, before the overlays draw, so
  /// a loadout received this frame renders this frame.
  void PollCosmeticSync();

  /// Drives on_tick + the async pump on the worker. Posted by `Tick`.
  void RunTickOnWorker(GameTick tick);

  //--- owned services (declaration order is destruction-reverse order) ------
  std::unique_ptr<PerformanceGovernor> governor_;
  std::unique_ptr<Hud> hud_;
  std::unique_ptr<InputObserver> input_;
  std::unique_ptr<UiLayer> ui_;
  std::unique_ptr<PvpOverlay> pvp_;
  std::unique_ptr<RadialMenu> radial_;
  std::unique_ptr<Animator> animator_;
  std::unique_ptr<CombatStateTracker> combatState_;
  std::unique_ptr<CosmeticService> cosmetics_;
  std::unique_ptr<AnimatedSkeletonSource> animatorSeam_;
  std::unique_ptr<WorldVelocitySource> velocitySource_;
  std::unique_ptr<RealScriptFileSystem> fs_;
  std::unique_ptr<CosmeticPackLoader> packs_;
  std::unique_ptr<ScriptModuleRegistry> modules_;
  std::unique_ptr<ShaderRegistry> shaders_;
  std::unique_ptr<PackageRegistry> packages_;
  std::unique_ptr<ScriptFileStore> files_;
  std::unique_ptr<AudioService> audio_;
  std::unique_ptr<PacketLog> packets_;
  std::unique_ptr<IntentValidator> intents_;
  std::unique_ptr<WsClientPool> wsPool_;
  std::unique_ptr<ReplayCamera> replayCamera_;
  std::unique_ptr<CrystalOptimizer> crystal_;
  std::unique_ptr<VoiceService> voice_;
  std::unique_ptr<ScriptApi> api_;
  // The LAN cosmetic-sync channel and its multicast transport. Built only when
  // the config enables sync; the channel's Receive is what turns a verified
  // message into a rendered remote loadout. The transport is destroyed after
  // the channel, so the channel's borrowed transport pointer is never dangling.
  std::unique_ptr<LanCosmeticTransport> syncTransport_;
  std::unique_ptr<CosmeticSyncChannel> syncChannel_;
  // mDNS discovery for the sync channel: publishes this client so peers can find
  // it and browses for theirs. Built only when sync is enabled; polled on the
  // frame thread alongside the transport, and re-queried on a slow interval so
  // the peer list stays live.
  std::unique_ptr<LanDiscovery> discovery_;
  // The wall clock at the last discovery re-query (0 = none yet, so the first
  // frame queries). Keeps the re-query off the hot path.
  u64 lastDiscoveryQueryMs_ = 0;
  // Publishing side of the sync channel: the self id, the signing secret and
  // whether it is HMAC (dev) or Ed25519 (production). Empty secret or id means
  // the client only verifies/applies, never publishes.
  std::string syncSelfId_;
  std::string syncSignSecret_;
  bool syncUseHmac_ = false;
  // The local loadout as last published, so a tick that did not change it does
  // not re-broadcast (the channel's peers would only drop a duplicate anyway).
  std::string lastPublishedLoadout_;
  // The LAN voice transport. Built only when the config enables voice; the
  // service borrows it, so it is torn down after the service's transport is
  // cleared. In production this is the authenticated, encrypted transport; the
  // plaintext `LanVoiceTransport` remains available for a dev/diagnostic build.
  std::unique_ptr<SecureVoiceTransport> secureVoice_;
  std::unique_ptr<LanVoiceTransport> voiceTransport_;
  // The OTA signature-pack loader, its filesystem seam and the layout registry
  // it registers into. Built only when packs are enabled.
  std::unique_ptr<RealScriptFileSystem> sigPackFs_;
  std::unique_ptr<SignaturePackLoader> sigPackLoader_;
  std::unique_ptr<LayoutRegistry> sigPackLayouts_;
  SigPackLoadReport sigPackReport_;
  // The on-device overlay backend and the seam that funnels the Renderer's
  // frame batch into it. Owned here so the render path has a production owner
  // (the Renderer only ever had a test-installed seam). `null` backend on the
  // host: the seam is then not installed and the batch is discarded, which is
  // the correct headless behaviour.
  std::unique_ptr<IRenderBackend> renderBackend_;
  std::unique_ptr<RenderSeam> renderSeam_;

  // The on-device cosmetic render path: the mesh backend (GLES on device, null
  // headless), the render seam that binds resolved items to it, the player-model
  // hook that refreshes bones, the skeleton source that answers them, and the
  // asset loader/store for real geometry. Built at attach; each fails closed
  // (draws nothing / resolves nothing) when its platform half is unavailable.
  std::unique_ptr<ICosmeticMeshBackend> cosmeticMeshBackend_;
  std::unique_ptr<DeviceCosmeticRenderSeam> cosmeticRenderSeam_;
  std::unique_ptr<PlayerModelHook> playerModelHook_;
  std::unique_ptr<PlayerModelSkeletonSource> playerModelSkeleton_;
  std::unique_ptr<CosmeticAssetLoader> cosmeticAssets_;
  std::unique_ptr<RendererCameraSource> cosmeticCamera_;
  // The per-asset custom-animation seam: adapts the Animator's named clips so a
  // cosmetic's own `animation` moves it. Borrowed by the cosmetic service, so it
  // is torn down after the service's animation source is cleared.
  std::unique_ptr<AnimatorAnimationSource> animationSource_;

  // The VM-facing binding table: the IDL bindings plus the async bindings.
  ScriptBindings bindings_;
  ScriptApi::Services services_;
  std::unique_ptr<AsyncRuntime> async_;
  std::unique_ptr<ScriptLoader> loader_;
  std::unique_ptr<LoadedScript> script_;
  std::unique_ptr<ScriptWorker> worker_;

  // Borrowed; the World module owns the handle and outlives this.
  WorldHandle* world_ = nullptr;
  Engine* engine_ = nullptr;
  Renderer* renderer_ = nullptr;
  EntityId localPlayer_;
  UrlPolicy networkPolicy_;
  std::function<Vec2()> screenSource_;
  bool attached_ = false;
  std::string lastScriptError_;
};

}  // namespace Berry
