#pragma once
// BerryAPI client input observation.
//
// Read-only view of the local player's input: which keys are held and how fast
// the mouse buttons are being clicked. This is exactly what a normal client can
// already observe, so it is safe to expose; it is the data source for the
// keystroke and CPS HUD widgets.
//
// Input is delivered as discrete press/release events (the engine's own input
// hook calls those), so the observer is deterministic and testable without a
// device: feed events, read state.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"

#include <array>
#include <deque>
#include <string>

namespace Berry {

enum class MouseButton : u8 { Left = 0, Right = 1, Middle = 2, Count = 3 };

/// Snapshot of input state for one frame.
struct BERRY_API InputState {
  bool forward = false;    // W
  bool back = false;       // S
  bool left = false;       // A
  bool right = false;      // D
  bool jump = false;       // Space
  bool sneak = false;      // Shift
  bool sprint = false;     // Ctrl / double-tap W
  bool attack = false;     // left mouse held
  bool use = false;        // right mouse held
  std::array<bool, 3> mouse{};
  /// Pointer position in physical pixels, as last delivered by the input hook.
  Vec2 pointer;
  /// Wheel delta for the current frame, in notches (positive = scroll up).
  f32 wheel = 0.0f;

  /// Number of WASD keys held, 0..4.
  int movementHeld() const {
    return (forward ? 1 : 0) + (back ? 1 : 0) + (left ? 1 : 0) + (right ? 1 : 0);
  }
};

/// Tracks key and mouse state and derives click rate.
///
/// Single-threaded by construction: the owner delivers events (`OnKey`,
/// `OnPointerMove`, ...) and reads state (`state()`, `ConsumeWheel`, ...) on the
/// same thread. The engine's input hook runs on the render thread, so that is
/// where the immediate-mode UI layer reads it from during `berry:frame`; an
/// embedder that delivers input elsewhere must snapshot into a frame-local
/// `InputState` rather than share the observer across threads.
class BERRY_API InputObserver {
 public:
  InputObserver() = default;

  /// Records a key event by name ("w", "space", "shift", "ctrl", "a"...).
  /// Unknown names are ignored (fail closed) rather than stored.
  void OnKey(const char* key, bool down);
  void OnMouse(MouseButton button, bool down);

  /// Records a pointer move, in physical pixels.
  void OnPointerMove(f32 x, f32 y);
  /// Records a wheel notch (positive = scroll up). Accumulated until a consumer
  /// takes it with ConsumeWheel().
  void OnWheel(f32 notches);

  /// Records typed UTF-8 text (one or more bytes). Accumulated until a consumer
  /// takes it with ConsumeText(). Control bytes are rejected, so a stray newline
  /// or escape never lands in a text field.
  void OnTextInput(const char* utf8);

  /// Advances the click-rate window. Call once per frame/tick.
  void Tick(u64 nowMs);

  /// Takes the wheel delta accumulated since the last call and zeroes it. A UI
  /// layer calls this once per frame so a scroll is delivered exactly once.
  f32 ConsumeWheel();

  /// Takes the typed text accumulated since the last call and clears it.
  std::string ConsumeText();

  /// Edge-triggered key press: true exactly once per press of `key`, then the
  /// press is consumed. Used for one-shot keys (backspace, enter) that a UI must
  /// act on once rather than while held. Held state stays in `state()`.
  bool ConsumeKeyPress(const char* key);

  const InputState& state() const { return state_; }

  /// Left/right clicks per second over the rolling window.
  f64 clicksPerSecond(MouseButton button) const;
  /// Total clicks recorded since construction (never decays).
  u64 totalClicks(MouseButton button) const;

  /// How long a key has been held, in ms (0 when released).
  u64 heldMs(const char* key, u64 nowMs) const;

  void Reset();

  static constexpr u64 kWindowMs = 1000;

 private:
  struct ClickWindow {
    std::deque<u64> stamps;
    u64 total = 0;
  };

  InputState state_;
  std::array<ClickWindow, 3> clicks_;
  struct Held {
    std::string key;
    u64 sinceMs = 0;
  };
  std::array<Held, 8> held_;
  u64 nowMs_ = 0;
  std::string typed_;
  /// Keys pressed since the last ConsumeKeyPress, so a one-shot key is seen
  /// exactly once. A small fixed set: backspace, enter.
  std::array<std::string, 8> pressed_;
  size_t pressedCount_ = 0;
};

}  // namespace Berry
