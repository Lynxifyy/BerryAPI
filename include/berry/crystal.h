#pragma once
// Client-side crystal-placement advisor.
//
// This is a *client feature*, not a new strict Berry:: module (the strict set
// stays Combat/Render/World/Network). It answers one question a crystal-PvP
// player asks every fight: where is the best place to drop a crystal from here?
//
// It is advisory and read-only. It reads the entities the world grid already
// delivered and the blocks the level seam already answers, ranks candidate
// spots by the damage they would deal, and reports the ranking to a script. It
// never places a block, never sends a packet, and never writes to the game —
// there is deliberately no auto-place, no aim assist and no packet path here.
// A crystal blast is an explosion the player triggers; this only measures where
// it would hurt most.
//
// The damage figure is an ESTIMATE, not the engine's number: a crystal's blast
// falls off linearly with distance, so the model is
// `damage = maxDamage * clamp(1 - distance / radius, 0, 1)`, scaled by a
// per-target factor. The docs and the binding say so; a script must not treat it
// as authoritative. The self-damage term uses the same falloff from the placer,
// so a spot that would blow the placer up ranks below one that will not.
//
// It is a pure, deterministic structure: no clocks and no globals. `Analyze`
// takes an explicit tick, so a host test drives it with no device and the
// device adapter feeds it real values. Candidate generation is bounded (range,
// level count, total candidates) so a script cannot make one call walk the
// world.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/game.h"

#include <mutex>
#include <string>
#include <vector>

namespace Berry {

/// Read-only block query the advisor needs. The core never touches engine
/// memory: the embedder binds a seam over `LevelSeam`, and a host test binds a
/// lambda-free stub.
class BERRY_API CrystalWorld {
 public:
  virtual ~CrystalWorld() = default;
  /// Block id at a world position (0 = air). Read-only.
  virtual u32 BlockAt(const Vec3& p) const = 0;
};

/// The advisor's tunables. Every one is validated on the way in: a bad range or
/// radius is refused, not clamped silently.
struct BERRY_API CrystalConfig {
  /// How far from the placer a crystal can be placed, in blocks. This is the
  /// client's own reach, not an extension of it.
  f32 placeRange = 4.5f;
  /// The blast's effect radius in blocks.
  f32 blastRadius = 6.0f;
  /// The damage at the epicentre, before falloff. The estimate's only
  /// magnitude; the falloff shape is fixed.
  f32 maxDamage = 12.0f;
  /// A spot is only recommended when it would hit at least this many targets.
  i32 minTargets = 1;
  /// How much the damage a placement would deal to the placer is weighted
  /// against the damage it deals to enemies. 1.0 is a straight subtraction.
  f32 selfDamageWeight = 1.0f;
  /// A placement that would deal more than this to the placer is never
  /// recommended, however good it looks.
  f32 maxSelfDamage = 6.0f;
  /// The number of block levels above and below the placer's feet that are
  /// considered. 1 covers feet, one up and one down.
  i32 levels = 1;
  /// How many recommendations `Analyze` keeps. Bounded so the result is small.
  size_t maxResults = 8;
};

/// One ranked placement.
struct BERRY_API CrystalPlacement {
  Vec3 position;         // where the crystal would sit (block space)
  i32 targetsHit = 0;
  f32 totalDamage = 0;   // summed over the targets hit
  f32 bestDamage = 0;    // damage to the most-affected target
  f32 selfDamage = 0;    // damage this placement would deal to the placer
  f32 score = 0;         // totalDamage - selfDamageWeight * selfDamage
};

struct BERRY_API CrystalStats {
  u64 analyses = 0;
  u64 candidates = 0;    // candidate spots tested across all analyses
  u64 placements = 0;    // placements returned across all analyses
  f32 bestScore = 0;
};

/// Owns the configuration and the last ranking. Thread-safe.
class BERRY_API CrystalOptimizer {
 public:
  // Bounds: a script cannot make one analysis test an unbounded number of
  // spots, and a radius/range cannot be absurd.
  static constexpr size_t kMaxCandidates = 4096;
  static constexpr f32 kMaxRange = 16.0f;
  static constexpr f32 kMaxBlastRadius = 32.0f;
  static constexpr i32 kMaxLevels = 4;

  CrystalOptimizer() = default;

  CrystalConfig config() const;
  /// Replaces the configuration. Returns false (and changes nothing) when any
  /// value is out of range: a non-finite/negative range or radius, a range
  /// above the bound, more levels than allowed, a negative result count, or a
  /// negative self-damage weight.
  bool SetConfig(const CrystalConfig& cfg);

  /// Ranks the candidate placements around `origin` (the placer's feet) against
  /// `targets`, using `world` to test each spot. `self` is the placer's id; a
  /// target with that id contributes self-damage instead of enemy damage.
  ///
  /// Pure and deterministic: the same inputs produce the same ranking, ordered
  /// by score and then by position, so a test can pin it exactly.
  std::vector<CrystalPlacement> Analyze(const Vec3& origin, EntityId self,
                                        const std::vector<EntitySnapshot>& targets,
                                        const CrystalWorld& world, u64 tick);

  /// The ranking from the last `Analyze`, without recomputing.
  std::vector<CrystalPlacement> recommendations() const;
  /// The best placement from the last analysis, or a default (score 0,
  /// position 0) when there is none. `ok` (optional) reports whether one exists.
  CrystalPlacement best(bool* ok = nullptr) const;

  CrystalStats stats() const;
  /// Forgets the last ranking and its statistics.
  void Reset();

  /// Whether the advisor is armed to run its advisory pass on the game tick.
  /// Disarmed by default, so an advisor no script asked for costs nothing.
  bool armed() const;
  /// Arms the advisor. Returns false when it is already armed.
  bool Arm();
  /// Disarms the advisor.
  void Disarm();

  /// The estimated damage a blast at `center` would deal to a target at
  /// `targetPos` under `cfg`. Exposed so the model is testable on its own and a
  /// script can ask "what would this do?" without a full analysis.
  static f32 EstimateDamage(const Vec3& center, const Vec3& targetPos,
                            const CrystalConfig& cfg);

 private:
  mutable std::mutex mutex_;
  CrystalConfig cfg_{};
  std::vector<CrystalPlacement> results_;
  CrystalStats stats_{};
  u64 lastTick_ = 0;
  bool armed_ = false;
};

}  // namespace Berry
