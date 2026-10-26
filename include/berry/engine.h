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
#include "berry/voice_crypto.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace Berry {

class SessionRecorder;
class ReplayService;
class BerryClient;

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
  /// Build the client composition root (owns the client services + the script
  /// host). On by default; a pure telemetry injector turns it off.
  bool enableClient = true;
  /// Load manifests without signature/source-hash verification. Development
  /// only; never set on device.
  bool devMode = false;
  /// The directory the client loads a script from, or empty for none.
  std::string scriptDirectory;
  /// The Ed25519 public key (64 hex chars) a script manifest's signature is
  /// verified against. Empty means "no anchor", and since the loader requires a
  /// signature outside dev mode, an empty anchor then refuses every manifest —
  /// which is the correct fail-closed default. Production sets this to the
  /// publisher's key.
  std::string scriptAnchorKey;
  /// The local player's runtime entity id (0 = unknown).
  u64 localPlayerId = 0;
  /// LAN cosmetic-sync channel (opt-in). Off by default.
  bool enableCosmeticSync = false;
  std::string syncVerifyKey;   // hex Ed25519 public key, or HMAC secret
  std::string syncSelfId;      // empty = verify/apply only, never publish
  std::string syncSignSecret;  // empty = never publish
  bool syncUseHmac = false;
  /// Presence-backend media plane for cosmetic sync. When `syncRelayHost` is
  /// set the sync transport is a UDP relay transport instead of the LAN
  /// multicast transport, so two clients on different networks reach each other.
  std::string syncRelayHost;    // numeric IPv4; empty = LAN multicast
  u16 syncRelayPort = 47826;
  std::string syncRoom;         // relay room name
  std::string syncRoomSecret;   // raw room secret (>= 16 bytes)
  /// LAN proximity voice (opt-in). Off by default.
  bool enableVoice = false;
  std::string voiceSelfId;     // empty = transport bound but session stopped
  /// The Ed25519 seed (raw or hex) that identifies this client on the voice
  /// channel. Empty disables voice: there is no plaintext fallback.
  std::string voiceIdentitySeed;
  /// What a peer's voice announce is verified against. The default accepts
  /// nothing, so voice stays off until an embedder sets it.
  VoiceTrustAnchor voiceTrust;
  /// The voice multicast group and port (defaults are a different group from
  /// the cosmetic channel).
  std::string voiceGroup = "239.66.0.2";
  u16 voicePort = 47824;
  /// Presence-backend media plane for voice. When `voiceRelayHost` is set the
  /// voice transport is a UDP relay transport instead of the LAN multicast
  /// transport, so two clients on different networks hear each other.
  std::string voiceRelayHost;    // numeric IPv4; empty = LAN multicast
  u16 voiceRelayPort = 47825;
  std::string voiceRoom;         // relay room name
  std::string voiceRoomSecret;   // raw room secret (>= 16 bytes)
  /// The interface the LAN channels join and send on ("" = system route).
  std::string lanIface;
  /// Root directory for the sandboxed `fs.*` store. Empty keeps it in-memory.
  std::string scriptDataDirectory;
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

  /// The session recorder: record-and-replay of the world stream. Always
  /// present; recording is off until `SessionRecorder::Start`.
  SessionRecorder& Recorder() { return *recorder_; }

  /// The replay mod: the container capture, the reconstructed replay, the
  /// local review overlay and the loopback playback server, in one owned
  /// object. Always present; capture is off until `ReplayCapture::Start` and
  /// the server is stopped until `ReplayServer::Start`.
  ReplayService& Replay() { return *replay_; }

  /// The client composition root: owns the client services (UI, HUD, overlays,
  /// registries, script host) and drives them on the tick/frame. Created by
  /// `Attach` when the config enables it; null otherwise.
  BerryClient* Client() { return client_.get(); }
  const BerryClient* Client() const { return client_.get(); }

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
  Engine();
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  void ApplyConfig(const EngineConfig& cfg);
  void ResolveCoreSignatures();

  HookManager hooks_;
  Scheduler sched_;
  ModuleRegistry modules_;
  std::unique_ptr<SessionRecorder> recorder_;
  std::unique_ptr<ReplayService> replay_;
  /// The client composition root. Declared after the modules it binds, so it is
  /// destroyed before them (its services hold the world/render handles).
  std::unique_ptr<BerryClient> client_;
  std::atomic<GameTick> currentTick_{0};
  std::atomic<bool> attached_{false};
  EngineConfig cfg_;
  GameTick lastFrameTick_ = 0;
  u64 lastReplayFrameMs_ = 0;
  /// Wall-clock of the last presented frame, for the client frame delta.
  u64 lastFrameMs_ = 0;
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