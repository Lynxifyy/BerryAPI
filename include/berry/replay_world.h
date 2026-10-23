#pragma once
// Replay playback: reconstruct the recorded world at any tick.
//
// A replay stores keyframes (full state) anchored every few seconds and deltas
// (what changed) in between. To show tick N, a player must find the keyframe at
// or before N and apply the deltas up to N — which is exactly what a seek is,
// and why the container keeps a keyframe index. This class does that and then
// answers `LevelSeam` reads from the reconstructed state, so the fight-review
// overlay and the loopback replay server share one reconstruction and cannot
// disagree about what tick N looked like.
//
// It is a pure reader over a `ReplayFile`: it never writes to game memory and
// never mutates the file. A seek decodes one keyframe plus the deltas to the
// target tick, so seeking is O(keyframe interval) whatever the replay's length.
//
// The state is bounded by the replay's own caps (the number of entities a
// keyframe can carry), and every record is decoded fail-closed, so a corrupt
// replay yields "no state" rather than a bad one.

#include "berry/export.h"
#include "berry/game.h"
#include "berry/replay_format.h"
#include "berry/types.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {

/// A timeline marker: an event recorded at a tick, for a scrubber's notches.
struct BERRY_API ReplayMarker {
  u64 tick = 0;
  std::string channel;
  std::string payload;
};

/// A `LevelSeam` over a replay at one tick. Borrows the `ReplayFile`; the file
/// must outlive the world.
class BERRY_API ReplayWorld final : public LevelSeam {
 public:
  explicit ReplayWorld(const ReplayFile* file);

  const char* Name() const override { return "replay-world"; }

  //--- timeline -------------------------------------------------------------

  /// Reconstructs the world at `tick`. Clamped into `[firstTick, lastTick]`.
  /// Returns false only when the replay is empty.
  bool Seek(u64 tick);
  /// The tick the current state is at, or 0 when nothing is loaded.
  u64 tick() const { return tick_; }
  u64 firstTick() const { return first_; }
  u64 lastTick() const { return last_; }
  bool loaded() const { return file_ != nullptr && !file_->sections.empty(); }
  /// The ticks the keyframes sit at, for a UI that shows the seek anchors.
  const std::vector<u64>& keyframeTicks() const { return keyframeTicks_; }

  /// The markers (event records) across the whole replay, in tick order.
  const std::vector<ReplayMarker>& markers() const { return markers_; }
  /// How many markers fall in `[from, to]`.
  size_t MarkerCount(u64 from, u64 to) const;

  //--- LevelSeam ------------------------------------------------------------

  bool GetEntity(EntityId id, EntitySnapshot* out) const override;
  std::vector<EntityId> AllEntities() const override;
  std::vector<EntityId> EntitiesInAabb(const Aabb& box,
                                       EntityType filter = EntityType::Unknown) const override;
  std::vector<EntityId> EntitiesInRadius(const Vec3& center, f32 radius) const override;
  bool RaycastEntity(const Vec3& origin, const Vec3& dir, f32 maxDist,
                     EntityId* outId, f32* outDist) const override;
  bool RaycastBlock(const Vec3& origin, const Vec3& dir, f32 maxDist,
                    Vec3* outPos, u32* outBlockType) const override;
  u32 GetBlock(const Vec3& worldPos) const override;
  LevelTime GetTime() const override;

  /// The entities currently reconstructed (the live state a caller draws).
  const std::unordered_map<u64, EntitySnapshot>& entities() const {
    return entities_;
  }

 private:
  /// The section index of the keyframe at or before `tick`, or 0.
  size_t KeyframeIndexFor(u64 tick) const;

  const ReplayFile* file_ = nullptr;
  std::unordered_map<u64, EntitySnapshot> entities_;
  LevelTime time_;
  u64 tick_ = 0;
  u64 first_ = 0;
  u64 last_ = 0;
  std::vector<ReplayMarker> markers_;
  std::vector<u64> keyframeTicks_;
};

}  // namespace Berry
