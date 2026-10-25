#pragma once
// Client-side touch, gesture and on-screen button API.
//
// This is a *client feature*, not a new strict Berry:: module (the strict set
// stays Combat/Render/World/Network). It is local-only by construction: every
// value it produces comes from the player's own finger on the player's own
// screen, so nothing here reveals anything the client was not already shown.
//
// The engine delivers raw multi-touch (down/move/up per finger). This layer
// turns that stream into the gestures a script actually wants to act on — tap,
// double-tap, long-press, drag, swipe, pinch, rotate — and owns the on-screen
// buttons a mobile mod needs (a labelled rectangle that reports press/release
// and can request a policy-gated intent).
//
// Threading follows the other client services: the game's input hook feeds
// `OnPointer*` on the render thread, the frame hook drives `Update`/`Draw` on
// the same thread, and the script worker only authors configuration and reads
// state. One mutex guards everything, so a script that adds a button mid-frame
// cannot race the hit-test.
//
// Recognition is deterministic and clock-driven: every decision is a pure
// function of the pointer history and the timestamps the caller supplies, so
// the whole model is testable without a device. A gesture threshold is
// validated on the way in and a bad value is refused, not clamped, so a script
// cannot leave the recogniser in a state the geometry cannot express.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/draw.h"
#include "berry/intent.h"

#include <array>
#include <mutex>
#include <string>
#include <vector>

namespace Berry {

class Renderer;

/// The gestures this layer recognises.
enum class GestureKind : u8 {
  None = 0,
  Tap,         // a quick press and release with little movement
  DoubleTap,   // a second tap within the double-tap window
  LongPress,   // a press held past the long-press threshold without moving
  Drag,        // a press moving past the tap movement tolerance
  Swipe,       // a fast release that travelled past the swipe distance
  Pinch,       // two fingers changing their separation
  Rotate,      // two fingers changing their angle
};

/// The direction of a swipe.
enum class SwipeDir : u8 { None = 0, Up, Down, Left, Right };

/// A gesture enable mask, so a script can turn off the ones it does not want
/// (and stop paying for them). Every gesture is on by default.
enum class GestureMask : u32 {
  None = 0,
  Tap = 1u << 0,
  DoubleTap = 1u << 1,
  LongPress = 1u << 2,
  Drag = 1u << 3,
  Swipe = 1u << 4,
  Pinch = 1u << 5,
  Rotate = 1u << 6,
  All = 0x7Fu,
};

const char* GestureKindName(GestureKind k);
GestureKind GestureKindFromName(const std::string& name);
const char* SwipeDirName(SwipeDir d);
/// The mask bit for a gesture kind (None for an unknown kind).
u32 GestureBit(GestureKind k);

/// One active (or just-released) finger. The id is the engine's finger id, so
/// a caller can follow one finger across a move.
struct BERRY_API TouchPointer {
  u32 id = 0;
  Vec2 pos;
  Vec2 start;      // where the finger went down
  bool down = false;
  u64 downMs = 0;
};

/// A recognised gesture, as reported to a script. `magnitude` is the travel
/// distance for Tap/Drag/Swipe (pixels), the scale factor for Pinch (1.0 =
/// unchanged) and the angle delta in degrees for Rotate.
struct BERRY_API TouchGesture {
  GestureKind kind = GestureKind::None;
  SwipeDir dir = SwipeDir::None;
  Vec2 at;             // where it happened (the release point for an up-edge)
  Vec2 delta;          // movement since the gesture began
  f32 magnitude = 0.0f;
  u32 pointers = 0;    // how many fingers were involved
  u64 atMs = 0;
};

/// The recogniser's thresholds. Every field is validated by the setter that
/// owns it.
struct BERRY_API TouchConfig {
  f32 tapMaxMove = 14.0f;       // movement that still counts as a tap
  f32 tapMaxMs = 260.0f;        // duration that still counts as a tap
  f32 longPressMs = 480.0f;     // hold time before a long press fires
  f32 doubleTapMaxMs = 300.0f;  // window for the second tap
  f32 swipeMinDist = 48.0f;     // travel that makes a release a swipe
  f32 swipeMaxMs = 520.0f;      // a swipe must be quicker than this
  f32 pinchMinScale = 0.02f;    // separation change that fires a pinch
};

/// One on-screen button: a labelled rectangle that reports press/release and
/// can request a policy-gated intent.
struct BERRY_API TouchButton {
  std::string label;
  Vec2 min, max;
  bool enabled = true;
  /// An optional intent name ("zoom", "keybind", …). When set, a press is
  /// validated against the bound IntentValidator and the button reports
  /// whether the intent is allowed here; a refused intent still reports the
  /// press, with `allowed` false, so a script can grey it out.
  std::string intent;
  //--- runtime state (owned by the frame thread) ---
  bool held = false;
  bool toggled = false;
  bool allowed = true;
  u64 presses = 0;
  u64 releases = 0;
};

/// A read-only snapshot for a script or a test.
struct BERRY_API TouchState {
  size_t pointers = 0;
  TouchGesture gesture;
  size_t buttons = 0;
};

/// Owns the pointer history, the recogniser and the on-screen buttons.
class BERRY_API TouchService {
 public:
  /// Bounds: a script cannot grow the button list or a label without limit, and
  /// the pointer table is a fixed small size, so a runaway feed cannot grow
  /// memory.
  static constexpr size_t kMaxPointers = 10;
  static constexpr size_t kMaxButtons = 24;
  static constexpr size_t kMaxLabelLen = 24;
  static constexpr size_t kMaxIntentLen = 32;
  /// The largest a button may be, so a typo cannot cover the screen.
  static constexpr f32 kMaxButtonSpan = 4096.0f;

  TouchService() = default;

  //--- feed (render thread, from the game's input hook) --------------------

  /// A finger went down at `pos`. Returns false when the finger table is full.
  bool OnPointerDown(u32 id, const Vec2& pos, u64 nowMs);
  /// A finger moved. Ignored when `id` is not down.
  void OnPointerMove(u32 id, const Vec2& pos, u64 nowMs);
  /// A finger lifted. Resolves a tap/double-tap/swipe and emits it.
  void OnPointerUp(u32 id, const Vec2& pos, u64 nowMs);
  /// Drops every pointer (a focus loss / screen change).
  void ClearPointers();

  //--- per-frame (render thread) -------------------------------------------

  /// Advances the recogniser (long press, drag, pinch, rotate) and hit-tests
  /// the buttons against the primary finger. `intents` is optional: when set,
  /// a button's intent is validated so its state reports `allowed`. Must be
  /// called before `Draw`.
  void Update(u64 nowMs, const IntentValidator* intents);

  /// Draws the on-screen buttons through `r`. A button that is disabled draws
  /// muted; a held or toggled one draws highlighted.
  void Draw(Renderer& r) const;

  //--- gestures (script thread) --------------------------------------------

  /// Enables or disables one gesture. False for an unknown gesture kind.
  bool SetGestureEnabled(GestureKind kind, bool enabled);
  bool gestureEnabled(GestureKind kind) const;
  /// The enabled-gesture mask.
  u32 gestureMask() const;

  TouchConfig config() const;
  /// Every setter refuses a non-finite or out-of-range value (returning false
  /// and leaving the config unchanged) rather than clamping silently.
  bool SetTapMaxMove(f32 px);
  bool SetTapMaxMs(f32 ms);
  bool SetLongPressMs(f32 ms);
  bool SetDoubleTapMaxMs(f32 ms);
  bool SetSwipeMinDist(f32 px);
  bool SetSwipeMaxMs(f32 ms);
  bool SetPinchMinScale(f32 scale);

  /// The last recognised gesture, whatever its kind (the `gesture()` binding).
  TouchGesture lastGesture() const;

  //--- on-screen buttons (script thread) -----------------------------------

  /// True when `label` is a usable button label (non-empty, bounded,
  /// printable).
  static bool ValidLabel(const std::string& label);
  /// True when `intent` is empty (no intent) or a bounded identifier.
  static bool ValidIntentName(const std::string& intent);

  /// Appends a button. False when the label is invalid, the rectangle is not
  /// finite or not positive-area or oversized, or the button cap is reached.
  bool AddButton(const std::string& label, const Vec2& min, const Vec2& max);
  /// Removes the button at `index`. False when out of range.
  bool RemoveButton(size_t index);
  /// Removes every button.
  void ClearButtons();

  size_t buttonCount() const;
  /// The label at `index`, or "" when out of range.
  std::string buttonLabel(size_t index) const;
  bool buttonEnabled(size_t index) const;
  /// Enables or disables the button at `index`. False when out of range.
  bool SetButtonEnabled(size_t index, bool enabled);
  /// Sets the intent the button requests on press. False when out of range or
  /// the name is not a valid identifier.
  bool SetButtonIntent(size_t index, const std::string& intent);
  std::string buttonIntent(size_t index) const;
  /// A snapshot of one button ({label, enabled, held, toggled, allowed,
  /// presses, releases}), or an empty table when out of range.
  TouchButton buttonState(size_t index) const;

  /// The pointers currently down, oldest first (a copy).
  std::vector<TouchPointer> pointers() const;

  TouchState state() const;

  /// The direction a swipe of `delta` travels: the dominant axis, or None when
  /// `delta` is shorter than `minDist`. Pure, so the geometry is testable
  /// without a feed.
  static SwipeDir SwipeDirectionOf(const Vec2& delta, f32 minDist);

  /// The channel a gesture kind publishes on ("" for None). Exposed so the
  /// IDL table and the emitter cannot disagree.
  static const char* GestureChannel(GestureKind k);

 private:
  struct Slot {
    TouchPointer p;
    bool used = false;
  };

  /// Caller holds mutex_. The slot index for `id`, or -1.
  i32 SlotFor(u32 id) const;
  /// Caller holds mutex_. The first down slot, or -1 (the primary finger).
  i32 PrimarySlot() const;
  /// Caller holds mutex_. Publishes `gesture` on its channel and records it.
  void PublishLocked(const TouchGesture& g);

  mutable std::mutex mutex_;
  /// A fixed pointer table: the recogniser is bounded by construction, so a
  /// runaway feed cannot grow memory.
  std::array<Slot, kMaxPointers> slots_{};
  u32 mask_ = (u32)GestureMask::All;
  TouchConfig cfg_;
  TouchGesture last_;
  // Long-press fired for the current primary press, so it fires once.
  bool longFired_ = false;
  // The current primary press began on a button, so it is the button's press
  // and must not also drive a gesture (tap/long press/drag/swipe).
  bool primaryOnButton_ = false;
  // Double-tap bookkeeping: the last tap's time and position.
  u64 lastTapMs_ = 0;
  bool haveLastTap_ = false;
  Vec2 lastTapPos_;
  // Two-finger pinch/rotate baseline, captured when the second finger lands.
  bool havePinchBase_ = false;
  f32 pinchBaseDist_ = 0.0f;
  f32 pinchBaseAngle_ = 0.0f;
  // The last emitted pinch scale / rotation angle, so a stationary pair does
  // not re-fire every frame.
  f32 lastPinchScale_ = 1.0f;
  f32 lastRotateDeg_ = 0.0f;
  std::vector<TouchButton> buttons_;
};

}  // namespace Berry
