#pragma once
#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/game.h"
#include "berry/event.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace Berry {

//----------------------------------------------------------------------------
// Berry::Combat module types.
//
// v2 (0.3): the combat module is strictly read-only. It OBSERVES damage that
// the engine applied; it cannot apply, cancel, or change damage. The rules
// pipeline, Kill/AreaDamage helpers and the Berry-specific damage causes were
// removed. CombatMetrics (DPS, average hit), KillAttribution, OnApplied and
// the "berry:combat" event bus remain.
//----------------------------------------------------------------------------

// A single resolved combat fact: a hit that happened in-game.
struct BERRY_API CombatEvent {
  EntityId victim;
  EntityId attacker;        // may be 0 for environmental causes
  f32 baseDamage = 0;
  f32 finalDamage = 0;      // the damage the engine actually applied
  DamageCause cause = DamageCause::None;
  u64 tick = 0;
};

enum class CombatOutcome { Observed, Ignored };

class BERRY_API DamageSeam {
 public:
  virtual ~DamageSeam() = default;
  /// Receives a read-only damage observation after the engine applied it.
  /// Returns true when the event was accepted.
  virtual bool OnDamage(const CombatEvent& evt) = 0;
  /// Refreshes the combat pipeline (called each tick).
  virtual void OnTick(u64 tick) = 0;
};

struct BERRY_API KillAttribution {
  EntityId killer;
  EntityId victim;
  time_t timeUtc = 0;
  u64 tick = 0;
  DamageCause cause = DamageCause::None;
};

// Rolling DPS/combat metrics for a single attacker.
struct BERRY_API CombatMetrics {
  f64 windowSeconds = 0;
  f64 damageDealt = 0;
  u64 hits = 0;
  u64 kills = 0;
  f32 lastHitDamage = 0;
  f64 Dps() const { return windowSeconds > 0 ? damageDealt / windowSeconds : 0; }
  f64 AverageHit() const { return hits ? damageDealt / (f64)hits : 0; }
};

//----------------------------------------------------------------------------
// Berry::Combat — the strict module itself (read-only observer).
//----------------------------------------------------------------------------

class BERRY_API Combat {
 public:
  Combat();
  ~Combat();

  // Lifecycle (called by engine through the module seam).
  bool Attach(DamageSeam* seam);
  void Detach();
  bool attached() const { return attached_; }

  /// Re-binds the damage seam at runtime (test harness or device adapter swap).
  void BindSeam(DamageSeam* seam) { seam_ = seam; }

  /// Observe a damage event that the engine already applied. Read-only: never
  /// applies, cancels, or modifies damage in the game. Records metrics and
  /// notifies the seam + OnApplied/event-bus observers. Invalid events
  /// (zero victim, non-finite or negative damage) are ignored — fail closed.
  CombatOutcome Observe(const CombatEvent& evt);

  // Metrics
  void RecordHit(EntityId attacker, f32 damage);
  void RecordHitUnlocked(EntityId attacker, f32 damage);  // caller holds mutex
  void RecordKill(EntityId killer, EntityId victim, DamageCause cause);
  CombatMetrics Metrics(EntityId attacker) const;
  std::vector<KillAttribution> RecentKills(size_t count = 16) const;

  /// Called for every observed (applied) hit.
  void OnApplied(std::function<void(const CombatEvent&)> fn);

 private:
  struct AttackerEntry {
    CombatMetrics metrics;
    u64 firstHitNs = 0;   // for the rolling DPS window
  };

  DamageSeam* seam_ = nullptr;
  bool attached_ = false;

  mutable std::mutex mutex_;
  std::unordered_map<u64, AttackerEntry> metrics_;  // by attacker raw id
  std::vector<KillAttribution> kills_;
  std::function<void(const CombatEvent&)> onApplied_;
};

}  // namespace Berry