#pragma once
// BerryAPI local combat-state telemetry (CSHR).
//
// A strictly local derivation over data the client already has:
//
//   * hits the engine already APPLIED (the same `berry:combat` observations
//     Berry::Combat sees),
//   * entity snapshots the LevelSeam already delivered,
//   * the local player's own click stream.
//
// It answers the diagnostic questions a fighting player asks: was my hit within
// reach, how fast am I swinging, where did that projectile go, did my prediction
// hold up, and am I clicking faster than a human. Nothing here is sent, nothing
// is written to the game, and no other player is revealed beyond what the fight
// already showed. There is deliberately no reach extension, no aim assist and no
// packet injection: this layer only MEASURES.
//
// It is a pure, deterministic data structure — no clocks and no globals. Every
// feed takes an explicit tick or millisecond, so a host test drives it with no
// device and the device adapter feeds it real values.
//
// Event payloads are collected under the lock and published after it is
// released, so a listener that re-enters the tracker cannot deadlock.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/game.h"
#include "berry/combat.h"
#include "berry/input.h"

#include <array>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {

/// The script-visible channels the combat-state layer publishes. Each carries a
/// short "<attacker>:<victim>" line, so a listener filters by id with a split
/// and never receives a pointer into game memory.
namespace combat_event {
/// A hit the local client predicted (the swing it believes will land).
///
/// These are the script-facing channel names, so they match the IDL `event`
/// declarations exactly and follow the same bare-camelCase convention as the
/// world-diff channels (`entityEntered`, `blockChanged`). A script awaits
/// `Events.once("combatPredicted")`; it never sees the internal bus name.
inline const char* Predicted() { return "combatPredicted"; }
/// A predicted hit the engine then confirmed.
inline const char* Confirmed() { return "combatConfirmed"; }
/// A predicted hit that was never confirmed within the window.
inline const char* Missed() { return "combatMissed"; }
}  // namespace combat_event

/// The outcome of checking one applied hit against the reach limit. `valid` is
/// false when a position was unknown, so a caller can tell "within reach" from
/// "could not tell" — the check never fabricates a distance.
struct BERRY_API ReachCheck {
  EntityId attacker;
  EntityId victim;
  f32 distance = 0;
  f32 maxReach = 3.0f;
  bool withinReach = false;
  bool valid = false;
  u64 tick = 0;
};

/// The timing between one swing and the previous swing by the same attacker.
struct BERRY_API SwingSample {
  EntityId attacker;
  u64 tick = 0;
  u64 deltaTicks = 0;
  f64 cps = 0;          // swings per second, 20 tps assumed
  bool valid = false;   // false for the first swing (no delta yet)
};

/// One recorded hitbox history sample.
struct BERRY_API HitboxSample {
  u64 tick = 0;
  Aabb box;
};

/// The result of resolving a projectile prediction against the real position.
struct BERRY_API PredictionRecord {
  EntityId id;
  Vec3 predicted;
  Vec3 actual;
  f32 error = 0;   // blocks between predicted and actual
  u64 tick = 0;
};

/// A hit the local client predicted, awaiting confirmation by the engine.
struct BERRY_API HitPrediction {
  EntityId attacker;
  EntityId victim;
  u64 tick = 0;
};

/// The combat-state tracker. Thread-safe; every method takes the lock.
class BERRY_API CombatStateTracker {
 public:
  /// Hitbox samples kept per entity: two seconds at 20 tps, longer than any
  /// reasonable hit-validation window.
  static constexpr size_t kHistoryPerEntity = 40;
  /// Cap on tracked entities, so a script cannot grow the map without bound.
  static constexpr size_t kMaxEntities = 256;
  /// Cap on recorded predictions kept for the mean-error read-out.
  static constexpr size_t kMaxPredictions = 128;
  /// Cap on per-attacker swing ticks kept for the average.
  static constexpr size_t kMaxSwingTicks = 32;
  /// A pending hit prediction older than this many ticks is a miss.
  static constexpr u64 kPredictionWindowTicks = 20;
  /// Default reach limit in blocks (survival melee reach).
  static constexpr f32 kDefaultMaxReach = 3.0f;
  /// Default click budget: 20 clicks/s sustained, burst of 20.
  static constexpr f32 kDefaultClickPerSecond = 20.0f;
  static constexpr f32 kDefaultClickBurst = 20.0f;
  /// Ticks per second the swing/CPC maths assumes.
  static constexpr f64 kTicksPerSecond = 20.0;

  CombatStateTracker();

  //--- configuration --------------------------------------------------------
  /// Sets the maximum legitimate reach. Must be finite and positive.
  bool SetMaxReach(f32 blocks);
  f32 maxReach() const;

  /// Sets the click budget. Both must be finite and positive; `burst` >= 1.
  bool SetClickBudget(f32 perSecond, f32 burst);

  //--- hitbox history -------------------------------------------------------
  /// Records one entity's hitbox for `tick`, derived from its snapshot with the
  /// same `EntityHitbox` the Entity bindings use. Bounded per entity and in the
  /// number of entities (least-recently-updated evicted first).
  void ObserveEntity(const EntitySnapshot& snap, u64 tick);
  size_t HitboxCount(EntityId id) const;
  /// The recorded box for `id` at exactly `tick`. Returns false when there is no
  /// sample, so a caller fails closed rather than reusing a stale box.
  bool HitboxAt(EntityId id, u64 tick, Aabb* out) const;
  /// The newest recorded box for `id` at or before `tick`, for reach checks that
  /// run one tick behind the snapshot feed.
  bool HitboxAtOrBefore(EntityId id, u64 tick, Aabb* out, u64* outTick) const;
  std::vector<u64> HitboxTicks(EntityId id) const;
  size_t TrackedEntities() const;
  size_t HitboxSamples() const;

  //--- reach validation -----------------------------------------------------
  /// Validates one applied hit against the reach limit, using the attacker's and
  /// victim's recorded hitboxes. When either position is unknown the check is
  /// recorded with `valid = false` rather than a guessed distance. Also confirms
  /// any matching pending hit prediction.
  ReachCheck ObserveHit(const CombatEvent& evt, u64 tick);
  ReachCheck lastReach() const;
  u64 reachViolations() const;

  //--- swing timing ---------------------------------------------------------
  /// Records a swing by `attacker` at `tick`; the delta against the previous
  /// swing yields the CPS.
  SwingSample ObserveSwing(EntityId attacker, u64 tick);
  SwingSample lastSwing() const;
  f64 AverageSwingCps(EntityId attacker) const;

  //--- projectile prediction ------------------------------------------------
  /// Extrapolates a projectile forward `ticks` ticks from its position and
  /// velocity under `gravity` (blocks/s^2, e.g. -20). Pure: reads no state.
  static Vec3 Extrapolate(const Vec3& pos, const Vec3& vel, f32 gravity, u32 ticks);
  /// Records a prediction for `id` so `ResolveProjectile` can score it.
  void NoteProjectile(EntityId id, const Vec3& predicted, u64 tick);
  /// Scores the pending prediction for `id` against `actual`. Returns the error
  /// in blocks, or -1 when there was no pending prediction.
  f32 ResolveProjectile(EntityId id, const Vec3& actual, u64 tick);
  /// Mean error across resolved predictions, or -1 when none.
  f32 MeanPredictionError() const;
  size_t PredictionCount() const;

  //--- hit prediction accuracy ----------------------------------------------
  /// Records a predicted hit and publishes the predicted event.
  void PredictHit(EntityId attacker, EntityId victim, u64 tick);
  /// Confirms a hit the engine applied: matches the oldest pending prediction
  /// for the same attacker/victim and publishes the confirmed event. Returns
  /// false when there was no matching prediction.
  bool ConfirmHit(const CombatEvent& evt, u64 tick);
  /// Ages pending predictions, counting any older than the window as misses and
  /// publishing the missed event. Call once per tick.
  void Tick(u64 nowTick);
  u64 PredictedHits() const;
  u64 ConfirmedHits() const;
  u64 MissedHits() const;
  /// Confirmed / predicted, 0 when nothing was predicted.
  f64 HitAccuracy() const;

  //--- click budget (token bucket) ------------------------------------------
  /// Feeds one click into `button`'s bucket. Returns true when the click is
  /// within budget. Never blocks or drops the click: it only reports.
  bool NoteClick(MouseButton button, u64 nowMs);
  /// Rolling clicks/s for `button` over the last second.
  f64 ClickRate(MouseButton button) const;
  /// True when the most recent `NoteClick` was over budget.
  bool ClickLimited() const;
  /// Tokens available in `button`'s bucket.
  f32 ClickTokens(MouseButton button) const;

  void Clear();

 private:
  struct ClickBucket {
    f32 tokens = 0;
    u64 lastMs = 0;
    std::deque<u64> stamps;   // for the rolling rate
  };

  static u64 Key(EntityId id) { return id.raw ? id.raw : id.runtime; }
  /// Lock-free cores, for callers that already hold `mutex_`.
  bool HitboxAtOrBeforeLocked(EntityId id, u64 tick, Aabb* out, u64* outTick) const;
  /// Matches and removes the oldest pending prediction for (attacker, victim).
  /// Caller holds the lock. Returns false when there is none.
  bool ConfirmLocked(EntityId attacker, EntityId victim, HitPrediction* out);
  /// Drops pending predictions older than the window. Caller holds the lock.
  /// Returns the expired ones so the caller can publish after unlocking.
  std::vector<HitPrediction> ExpirePredictionsLocked(u64 nowTick);
  /// Distance from a point to the nearest face of a box (0 when inside).
  static f32 DistanceToBox(const Vec3& from, const Aabb& box);
  static void Publish(const char* channel, const HitPrediction& p);

  mutable std::mutex mutex_;
  f32 maxReach_ = kDefaultMaxReach;

  std::unordered_map<u64, std::deque<HitboxSample>> history_;
  std::deque<u64> historyOrder_;   // least-recently-updated first

  ReachCheck lastReach_;
  u64 reachViolations_ = 0;

  std::unordered_map<u64, std::deque<u64>> swingTicks_;
  SwingSample lastSwing_;

  struct PendingProjectile {
    Vec3 predicted;
    u64 tick = 0;
  };
  std::unordered_map<u64, PendingProjectile> pendingProjectiles_;
  std::deque<PredictionRecord> predictions_;

  std::deque<HitPrediction> pendingHits_;
  u64 predictedHits_ = 0;
  u64 confirmedHits_ = 0;
  u64 missedHits_ = 0;

  f32 clickPerSecond_ = kDefaultClickPerSecond;
  f32 clickBurst_ = kDefaultClickBurst;
  std::array<ClickBucket, (size_t)MouseButton::Count> clicks_;
  bool clickLimited_ = false;
};

}  // namespace Berry
