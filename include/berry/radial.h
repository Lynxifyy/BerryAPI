#pragma once
// Client-side radial (pie) menu.
//
// This is a *client feature*, not a new strict Berry:: module (the strict set
// stays Combat/Render/World/Network). It is a local-only UI element: a script
// fills it with items, opens it, and reads which item the player picked. It
// draws through Berry::Render like the Hud and the immediate-mode UI do, and it
// holds no engine pointers of its own.
//
// The interaction is the standard one for a radial menu: the menu opens at a
// screen centre, the pointer's angle picks a sector, and releasing the button
// over a sector commits that item. Nothing here reveals anything the client was
// not already shown — it is the player's own pointer over the player's own
// menu.
//
// Threading follows the other client services: the script worker owns the item
// list and the open/close requests (guarded by one mutex), while the render
// thread drives `Update` (animation + hit-test) and `Draw` each frame from the
// frame hook. `Update` snapshots nothing the caller can race on: it takes the
// lock, advances the animation, resolves the hovered sector, and — on a release
// over an enabled sector — records the selection and publishes it once. `Draw`
// copies the item labels under the lock, then draws outside it, so a backend
// that calls back in never deadlocks.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/draw.h"

#include <mutex>
#include <string>
#include <vector>

namespace Berry {

class Renderer;

/// One sector of the menu.
struct BERRY_API RadialItem {
  std::string label;
  bool enabled = true;
};

/// The menu's tunables. Every one is validated on the way in: a bad radius or
/// duration is refused, not clamped silently, so a script cannot leave the menu
/// in a shape the renderer cannot draw.
struct BERRY_API RadialConfig {
  f32 radius = 120.0f;       // outer radius, pixels
  f32 innerRadius = 40.0f;   // dead zone at the centre, pixels
  f32 openMs = 140.0f;       // open/close animation duration
  f32 labelScale = 1.0f;
};

/// A read-only snapshot of the menu for a script or a test.
struct BERRY_API RadialState {
  bool open = false;
  i32 hovered = -1;    // sector under the pointer, or -1
  i32 selected = -1;   // the sector committed this frame, or -1
  f32 progress = 0.0f; // 0 (closed) .. 1 (fully open)
  size_t items = 0;
};

/// Owns the item list and the interaction state. Thread-safe.
class BERRY_API RadialMenu {
 public:
  // Bounds: a script cannot grow the menu without limit, and a label cannot be
  // unbounded, so the draw and the event payload stay small.
  static constexpr size_t kMaxItems = 12;
  static constexpr size_t kMaxLabelLen = 32;

  RadialMenu() = default;

  //--- authoring (script thread) -------------------------------------------

  /// Appends an item. False when the label is empty/too long or the item cap is
  /// reached.
  bool AddItem(const std::string& label, bool enabled = true);
  /// Removes the item at `index`. False when out of range. Resets the hover and
  /// selection if they pointed at or past the removed slot.
  bool RemoveItem(size_t index);
  /// Removes every item and closes the menu.
  void Clear();

  size_t itemCount() const;
  /// The label at `index`, or "" when out of range.
  std::string itemLabel(size_t index) const;
  /// Whether the item at `index` is enabled; false when out of range.
  bool itemEnabled(size_t index) const;
  /// Enables or disables an item. False when out of range.
  bool SetItemEnabled(size_t index, bool enabled);

  /// True when `label` is a valid menu label (non-empty, bounded, printable).
  static bool ValidLabel(const std::string& label);

  //--- configuration -------------------------------------------------------

  RadialConfig config() const;
  /// Sets the outer radius. Must be finite and positive and greater than the
  /// inner radius; a bad value is refused.
  bool SetRadius(f32 radius);
  /// Sets the inner (dead-zone) radius. Must be finite, non-negative and less
  /// than the outer radius; a bad value is refused.
  bool SetInnerRadius(f32 radius);
  /// Sets the open animation duration. Must be finite and positive.
  bool SetOpenMs(f32 ms);

  //--- open / close (script thread) ----------------------------------------

  /// Opens the menu. False when it has no items (an empty menu is not worth
  /// showing) or is already open.
  bool Open();
  /// Closes the menu and clears the hover/selection.
  void Close();
  bool open() const;

  //--- render-thread half --------------------------------------------------

  /// Advances the animation and resolves the interaction for one frame.
  ///
  /// `pointer` and `center` are in screen pixels. `held` is whether the select
  /// button is down this frame; the menu commits an item on the held→released
  /// edge while the pointer is over an enabled sector, and publishes
  /// `radialSelected` once. Must be called before `Draw`.
  void Update(f32 dtSeconds, const Vec2& center, const Vec2& pointer, bool held);

  /// Draws the open menu centred at `center`. A no-op when the menu is closed
  /// or has no items.
  void Draw(Renderer& r, const Vec2& center) const;

  /// The current state. Takes the lock.
  RadialState state() const;

  /// Which sector a pointer at `pointer`, relative to `center`, is over, for
  /// `count` items: -1 for the dead zone or outside the outer ring, otherwise
  /// the sector index. Exposed so the tests can pin the geometry without a
  /// renderer.
  static i32 SectorFor(const Vec2& pointer, const Vec2& center, f32 innerRadius,
                       f32 outerRadius, size_t count);

 private:
  mutable std::mutex mutex_;
  std::vector<RadialItem> items_;
  RadialConfig cfg_;
  bool open_ = false;
  f32 progress_ = 0.0f;      // 0..1
  i32 hovered_ = -1;
  i32 selected_ = -1;
  bool wasHeld_ = false;
};

}  // namespace Berry
