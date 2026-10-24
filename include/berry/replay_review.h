#pragma once
// Fight-review overlay: draw a replay back through the one render path.
//
// This is the "review" half of the replay mod — the piece that ships first
// because it exercises the whole format end to end. It takes a `ReplayWorld`
// (the reconstructed state at a tick) and draws it with the existing
// `Berry::Render` primitives: an ESP-style box and nameplate per entity, a
// line back to the local player, and a marker notch for each recorded combat
// event. Nothing new enters the renderer; a replay box is the same projected
// quad a live ESP box is, so it presents through the one RenderSeam.
//
// It is a *client feature*, not a new strict Berry:: module (the strict set
// stays Combat/Render/World/Network). It is read-only with respect to the
// world: it draws a replay the client already recorded, and it reveals nothing
// the client was not already sent — a replay is only ever what this client saw.
//
// The overlay owns no clock. `Advance(dtSeconds)` is the whole input, so a host
// test drives it deterministically and a paused client does not advance. It
// holds no engine pointers: it reads a `ReplayWorld`, which reads a
// `ReplayFile`.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/draw.h"
#include "berry/render.h"
#include "berry/replay_world.h"

#include <mutex>
#include <string>
#include <vector>

namespace Berry {

/// What the overlay draws.
struct BERRY_API ReplayReviewStyle {
  bool boxes = true;          // a box around each entity
  bool names = true;          // a nameplate above each entity
  bool lineToLocal = true;    // a line from the local player to each entity
  bool markers = true;        // a notch per recorded event
  Rgba boxColor = Rgba(90, 200, 255, 220);
  Rgba localColor = Rgba(120, 255, 120, 220);
  Rgba markerColor = Rgba(255, 210, 60, 220);
  f32 markerBandHeight = 6.0f;
  f32 lineThickness = 1.0f;
  f32 nameScale = 1.0f;
};

/// One drawn entity, so a caller (or a test) can see what was rendered.
struct BERRY_API ReplayReviewItem {
  EntityId id;
  Vec2 screenMin;
  Vec2 screenMax;
  std::string label;
};

/// The fight-review overlay. Thread-safe.
class BERRY_API ReplayReview {
 public:
  // Bounds: a replay can hold many entities, but one frame draws at most this
  // many, so a pathological replay cannot stall the render thread.
  static constexpr size_t kMaxDrawn = 256;

  ReplayReview() = default;

  ReplayReviewStyle style() const;
  void SetStyle(const ReplayReviewStyle& s);

  /// True when the overlay draws. Off by default, so binding a review never
  /// hijacks the frame until a script asks it to.
  bool active() const;
  void SetActive(bool on);

  /// The local player's id, so the overlay can mark them and draw the lines
  /// that anchor the review. 0 means "unknown", which just skips the anchor.
  void SetLocalPlayer(EntityId id);
  EntityId localPlayer() const;

  /// Plays a replay: begins at the first keyframe and advances on the frame
  /// clock. A no-op when the world holds no replay.
  bool Play(ReplayWorld* world);
  /// Stops advancing but keeps the reconstructed state, so the view holds.
  void Pause();
  bool playing() const;
  /// Stops and releases the world.
  void Stop();

  /// Advances the review by `dtSeconds` of wall time at the replay's tick rate
  /// (20 tps), clamped so a stall cannot fast-forward it. A non-finite or
  /// negative delta is ignored. Returns the tick now shown.
  u64 Advance(f32 dtSeconds);

  /// Seeks to an absolute tick (a scrubber drag). False when there is no world.
  bool Seek(u64 tick);
  u64 tick() const;
  bool activeWorld() const;

  /// The playback speed multiplier. Must be finite and in `(0, kMaxSpeed]`; a
  /// bad value is refused.
  static constexpr f32 kMaxSpeed = 32.0f;
  f32 speed() const;
  bool SetSpeed(f32 speed);

  /// Draws the current state. `cam` is the frame's camera (the replay camera's,
  /// when one is active, so a scripted viewpoint projects the review). Returns
  /// how many items were drawn. Must be called between BeginFrame and EndFrame.
  size_t Present(const CameraState& cam, Renderer& r, const Vec2& screen);

  /// The items drawn by the last `Present`, for a test.
  std::vector<ReplayReviewItem> lastItems() const;

 private:
  mutable std::mutex mutex_;
  ReplayReviewStyle style_{};
  bool active_ = false;
  bool playing_ = false;
  EntityId localPlayer_;
  ReplayWorld* world_ = nullptr;
  f32 speed_ = 1.0f;
  f32 accumTicks_ = 0.0f;
  std::vector<ReplayReviewItem> lastItems_;
};

}  // namespace Berry
