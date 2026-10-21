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

  /// Number of WASD keys held, 0..4.
  int movementHeld() const {
    return (forward ? 1 : 0) + (back ? 1 : 0) + (left ? 1 : 0) + (right ? 1 : 0);
  }
};

/// Tracks key and mouse state and derives click rate. Thread-confined: the
/// caller (engine input hook / script worker) owns the instance.
class BERRY_API InputObserver {
 public:
  InputObserver() = default;

  /// Records a key event by name ("w", "space", "shift", "ctrl", "a"...).
  /// Unknown names are ignored (fail closed) rather than stored.
  void OnKey(const char* key, bool down);
  void OnMouse(MouseButton button, bool down);

  /// Advances the click-rate window. Call once per frame/tick.
  void Tick(u64 nowMs);

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
};

}  // namespace Berry
