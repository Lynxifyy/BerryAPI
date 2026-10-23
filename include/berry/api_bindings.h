#pragma once
// Bindings that connect the declared IDL surface to the C++ services.
//
// `api/berry.idl` declares the functions a script may call; the generated table
// in `generated_bindings.h` says what each one needs (capability, arity). This
// is the layer that makes them real:
//
//   * Every declared binding is registered here, so a name that appears in the
//     docs, the .d.ts and the .luau is callable. Nothing else registers them.
//   * Capability and arity come from the generated spec, not from hand-written
//     code, so the gate and the documentation cannot disagree. A wrong argument
//     count fails closed before the body runs.
//
// Threading is the other half. Scripts run on the script worker; the Renderer
// and the HUD widget tree belong to the render thread. A script therefore never
// touches them directly:
//
//   script thread            render thread
//   -------------            -------------
//   render.draw_text  ---->  [ queue ]  ---->  Renderer::DrawText
//   hud.create_label  ---->  [ queue ]  ---->  Hud::Create
//
// Draw commands are posted to a bounded queue and drained by
// `DrainOnRenderThread()` (called from `Engine::OnFrame`). Reads a script needs
// synchronously — screen size, frame time — are answered from a snapshot the
// render thread publishes each frame, so no lock is held across the boundary.
//
// This is a client-side service, deliberately not a new strict Berry:: module:
// it draws through Berry::Render and holds no engine pointers of its own.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/audio.h"
#include "berry/combat.h"
#include "berry/combat_state.h"
#include "berry/animator.h"
#include "berry/replay_camera.h"
#include "berry/radial.h"
#include "berry/cosmetic.h"
#include "berry/event.h"
#include "berry/fs.h"
#include "berry/hud.h"
#include "berry/input.h"
#include "berry/intent.h"
#include "berry/perf.h"
#include "berry/packet.h"
#include "berry/pvp.h"
#include "berry/pack.h"
#include "berry/package.h"
#include "berry/render.h"
#include "berry/script.h"
#include "berry/scriptmod.h"
#include "berry/session_recorder.h"
#include "berry/shader.h"
#include "berry/ui.h"
#include "berry/services.h"
#include "berry/url.h"
#include "berry/ws_client.h"

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {

//----------------------------------------------------------------------------
// Render-thread command queue.
//
// Bounded and fail-closed: a script that floods the queue has its commands
// dropped (and counted) rather than growing the queue without limit or blocking
// the script worker on the render thread.
//----------------------------------------------------------------------------

class BERRY_API RenderCommandQueue {
 public:
  using Command = std::function<void()>;

  /// Default cap on queued commands. Comfortably more than a well-behaved
  /// script emits in a frame, small enough that a runaway one cannot exhaust
  /// memory.
  static constexpr size_t kDefaultCapacity = 4096;

  explicit RenderCommandQueue(size_t capacity = kDefaultCapacity);

  /// Posts a command. Returns false (and increments dropped()) when the queue
  /// is full — never blocks and never throws.
  bool Post(Command cmd);

  /// Runs every queued command in FIFO order on the calling thread, which must
  /// be the render thread. Returns how many ran.
  size_t Drain();

  size_t Pending() const;
  u64 dropped() const;
  u64 drained() const;
  void Clear();
  size_t capacity() const;

 private:
  mutable std::mutex mutex_;
  std::deque<Command> queue_;
  size_t capacity_;
  u64 dropped_ = 0;
  u64 drained_ = 0;
};

//----------------------------------------------------------------------------
// ScriptApi — the bridge.
//----------------------------------------------------------------------------

class BERRY_API ScriptApi {
 public:
  /// The services a script may reach. Pointers are borrowed; they must outlive
  /// the ScriptApi. Any that is null simply makes its bindings fail closed
  /// (they report "service unavailable") instead of crashing.
  struct Services {
    Renderer* renderer = nullptr;
    Hud* hud = nullptr;
    InputObserver* input = nullptr;
    PerformanceGovernor* governor = nullptr;
    Combat* combat = nullptr;
    WorldHandle* world = nullptr;
    /// Local cosmetic loadouts and wardrobe. Null makes the cosmetic.* bindings
    /// fail closed ("cosmetics unavailable").
    CosmeticService* cosmetics = nullptr;
    /// Local model-pack loader. Null makes `cosmetic.reload_packs` fail closed
    /// (returns 0 and raises "pack loader unavailable").
    CosmeticPackLoader* packs = nullptr;
    /// Local player velocity (motionX/Y/Z). Null makes `input.velocity` fail
    /// closed (returns zero, i.e. "stationary").
    LocalVelocitySource* velocity = nullptr;
    /// The intent validator for the current session. Null makes the `intent.*`
    /// bindings fail closed (every intent reports "not allowed").
    IntentValidator* intents = nullptr;
    /// Script shader registry and post-process chain. Null makes the `shader.*`
    /// bindings fail closed (registration reports false).
    ShaderRegistry* shaders = nullptr;
    /// Script package registry and resolver. Null makes the `package.*`
    /// bindings fail closed (registration reports false).
    PackageRegistry* packages = nullptr;
    /// Script module + settings registry. Null makes the `mod.*` bindings fail
    /// closed (creation returns 0 and the rest report false).
    ScriptModuleRegistry* modules = nullptr;
    /// Sandboxed per-script file store. Null makes the `fs.*` bindings fail
    /// closed (writes report false, reads return empty).
    ScriptFileStore* files = nullptr;
    /// Client-side audio cues. Null makes the `audio.*` bindings fail closed
    /// (a play reports false).
    AudioService* audio = nullptr;
    /// The PvP overlay (CPS, keystrokes, combo, armor, effects, ping/FPS,
    /// target, session stats). Null makes the `pvp.*` bindings fail closed and
    /// the frame hook skip the overlay.
    PvpOverlay* pvp = nullptr;
    /// The read-only packet log. Null makes the `packets.*` bindings fail
    /// closed (empty tables, zero counts).
    PacketLog* packets = nullptr;
    /// The process profiler. Null makes the `perf.flame`/`perf.spans`/
    /// `perf.flame_reset` bindings fail closed ("profiler unavailable").
    Profiler* profiler = nullptr;
    /// The local player's entity id, used as the origin for `world.query`.
    /// Default 0 means "unknown", which makes the query fail closed.
    EntityId localPlayer;
    /// The session recorder (record & replay of the world stream). Null makes
    /// the `record.*` bindings fail closed (start/save report failure).
    SessionRecorder* recorder = nullptr;
    /// Egress policy for `network.get`. The harness relaxes it for loopback.
    UrlPolicy networkPolicy;
    /// Script-facing WebSocket pool. Null makes the `ws.*` bindings fail closed
    /// ("websocket pool unavailable").
    WsClientPool* wsPool = nullptr;
    /// Local combat-state telemetry (reach, swing timing, hitbox history,
    /// projectile prediction, hit accuracy, click budget). Null makes the
    /// `combat_state.*` bindings fail closed.
    CombatStateTracker* combatState = nullptr;
    /// Immediate-mode UI layer. Null makes the `ui.*` bindings fail closed
    /// (inert results, no window opens).
    UiLayer* ui = nullptr;
    /// Client-side keyframed animator. Null makes the `animator.*` bindings
    /// fail closed (no clip is added, playback reports stopped).
    Animator* animator = nullptr;
    /// The skeleton seam that applies the animator's pose to the rendered rig.
    /// Null makes `animator.target`/`set_target` fail closed.
    AnimatedSkeletonSource* animatorSeam = nullptr;
    /// Client-side replay camera: a keyframed timeline over the recorded tape.
    /// When its timeline is active (and a renderer is bound) the frame hook
    /// adopts its pose as the frame's camera, so a replay renders from the
    /// scripted viewpoint instead of the player's. Null makes the `replay.*`
    /// bindings fail closed.
    ReplayCamera* replayCamera = nullptr;
    /// Client-side radial (pie) menu. Null makes the `radial.*` bindings fail
    /// closed and the frame hook skip the menu.
    RadialMenu* radial = nullptr;
  };

  ScriptApi();
  explicit ScriptApi(const Services& services);
  ~ScriptApi();

  ScriptApi(const ScriptApi&) = delete;
  ScriptApi& operator=(const ScriptApi&) = delete;

  /// Rebinds the services. Call before InstallBindings, which captures the
  /// service pointers; changing them afterwards is not supported.
  void SetServices(const Services& services);
  const Services& services() const { return services_; }

  /// Registers every binding in the generated IDL table into `out`. Returns how
  /// many were registered. Idempotent per name (re-registering replaces).
  ///
  /// The registered handlers capture `this`, so `*out` must not outlive the
  /// ScriptApi (nor be invoked after it is destroyed).
  size_t InstallBindings(ScriptBindings* out);

  /// Runs every command a script posted to the render queue. Call on the render
  /// thread. Also called each frame from the `berry:frame` event.
  size_t DrainOnRenderThread();

  /// Publishes a frame's timing and screen size for the synchronous reads
  /// (`render.screen_size`, `perf.frame_time_ms`). Call on the render thread.
  void PublishFrame(f32 frameMs, const Vec2& screen);

  /// The last published screen size (0,0 until a frame has run).
  Vec2 screenSize() const;
  /// The last published frame duration in ms.
  f32 frameTimeMs() const;
  /// The client-side zoom factor a script last requested (1 = no zoom).
  f32 zoomFactor() const { return zoom_.load(); }

  RenderCommandQueue& queue() { return queue_; }
  const RenderCommandQueue& queue() const { return queue_; }

  //--- introspection (tests) ------------------------------------------------
  /// True when the binding `name` is installed.
  bool installed(const std::string& name) const;
  size_t installedCount() const;

 private:
  /// Registers one binding, wrapping `body` with the generated spec's
  /// capability and an arity check. The name must exist in the IDL table.
  bool Install(const std::string& name,
               std::function<ScriptValue(ScriptContext&,
                                         const std::vector<ScriptValue>&)> body);

  /// Runs a render-thread command, tracking whether it applied. Used by the
  /// tests to assert a queued command actually reached the service.
  void PostRender(RenderCommandQueue::Command cmd);

  /// Subscribes to `berry:frame` so the queue drains on the render thread, and
  /// to `berry:combat` to keep the read-only combat telemetry current.
  void InitHooks();
  void InitCombatCache();
  void InitCombatStateHook();
  void InitFrameHook();
  void RemoveHooks();

  u64 AllocWidgetHandle();
  void SetWidgetId(u64 handle, WidgetId id);
  bool ResolveWidgetId(u64 handle, WidgetId* out);

  Services services_;
  RenderCommandQueue queue_;
  std::vector<std::string> installed_;
  ScriptBindings* bindingsSink_ = nullptr;
  mutable std::mutex mutex_;

  // Client-side state a script writes and the client reads back.
  std::atomic<f32> zoom_{1.0f};

  // Published by the render thread, read by the script thread.
  std::atomic<f32> frameMs_{0.0f};
  std::atomic<f32> screenW_{0.0f};
  std::atomic<f32> screenH_{0.0f};

  // Combat telemetry kept from the `berry:combat` event, so `combat.*` reads
  // are cheap and independent of the service's own locking.
  struct ObservedHit {
    u64 tick = 0;
    f32 damage = 0;
  };
  static constexpr size_t kMaxObserved = 256;
  std::atomic<f32> lastHitDamage_{0.0f};
  mutable std::mutex combatMutex_;
  std::deque<ObservedHit> observed_;

  // Virtual widget handles: a script gets a handle immediately; the real
  // WidgetId is filled in when the create command runs on the render thread.
  u64 nextWidgetHandle_ = 1;
  std::unordered_map<u64, WidgetId> widgetIds_;

  EventBus::ListenerId frameListener_ = 0;
  EventBus::ListenerId combatListener_ = 0;
  EventBus::ListenerId tickListener_ = 0;
  EventBus::ListenerId combatStateListener_ = 0;
  /// The wall-clock ms of the last presented frame, for the PvP overlay's
  /// animation delta. Render thread only.
  u64 lastFrameMs_ = 0;
  /// Frame clock for the animator, kept separate from lastFrameMs_ so a frame
  /// with no PvP overlay still advances a playing clip at the real delta.
  u64 lastAnimatorMs_ = 0;
  /// Frame clock for replay-camera playback, kept separate for the same reason.
  u64 lastReplayMs_ = 0;
  /// Frame clock for the radial menu's open/close animation, kept separate for
  /// the same reason.
  u64 lastRadialMs_ = 0;

  // Immediate-mode UI frame state. A script's `ui.begin_frame` opens the frame
  // and `ui.end_frame` publishes it, so the embedder never calls
  // BeginFrame/EndFrame itself. `uiFrameOpen_` also detects a hook that faulted
  // between the two, so the next `begin_frame` aborts the stale frame instead of
  // presenting it. Script-worker-thread only.
  bool uiFrameOpen_ = false;
};

}  // namespace Berry
