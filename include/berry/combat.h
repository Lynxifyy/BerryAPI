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
// The combat module sits on a DamageSeam that hooks the engine's damage
// function. Rules run before and after damage is applied; kill attribution
// tracks who killed whom; the DPS tracker measures real combat throughput
// (for performance debugging / damage tuning). Everything is testable
// against the shim's fake Level.
//----------------------------------------------------------------------------

// A single resolved combat fact passed through the pipeline.
struct BERRY_API CombatEvent {
  EntityId victim;
  EntityId attacker;        // may be 0 for environmental causes
  f32 baseDamage = 0;
  f32 finalDamage = 0;      // after rules, before engine applies
  DamageCause cause = DamageCause::None;
  bool cancelled = false;   // set by a rule to block the hit
  f32 knockbackX = 0, knockbackY = 0, knockbackZ = 0;
  u64 tick = 0;
};

using DamageRule = std::function<bool(CombatEvent&)>;

enum class CombatOutcome { Cancelled, Applied };

class BERRY_API DamageSeam {
 public:
  virtual ~DamageSeam() = default;
  /// Invoked by Combat module when it wants to apply a hit through the engine.
  virtual bool Apply(CombatEvent& evt) = 0;
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
// Berry::Combat — the strict module itself.
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

  // Rules engine
  u64 AddPreRule(DamageRule rule);   // before engine applies
  u64 AddPostRule(DamageRule rule);  // after engine applies
  bool RemoveRule(u64 id);

  // Apply a hit through the pipeline.
  CombatOutcome Damage(EntityId victim, EntityId attacker, f32 amt,
                       DamageCause cause);
  // One-shot kill.
  bool Kill(EntityId victim, EntityId attacker, DamageCause cause = DamageCause::BerryTech);
  // Area damage at a point.
  size_t AreaDamage(const Vec3& center, f32 radius, f32 amt,
                    DamageCause cause, EntityId source);

  // Metrics
  void RecordHit(EntityId attacker, f32 damage);
  void RecordHitUnlocked(EntityId attacker, f32 damage);  // caller holds mutex
  void RecordKill(EntityId killer, EntityId victim, DamageCause cause);
  CombatMetrics Metrics(EntityId attacker) const;
  std::vector<KillAttribution> RecentKills(size_t count = 16) const;

  // Quest-style hooks: called when your damage rule cancels a hit.
  void OnCancelled(std::function<void(const CombatEvent&)> fn);
  void OnApplied(std::function<void(const CombatEvent&)> fn);

 private:
  DamageSeam* seam_ = nullptr;
  bool attached_ = false;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, DamageRule> preRules_;
  std::unordered_map<std::string, DamageRule> postRules_;
  uint64_t nextRuleId_ = 1;

  std::unordered_map<u64, CombatMetrics> metrics_;  // by attacker raw id
  std::vector<KillAttribution> kills_;
  std::function<void(const CombatEvent&)> onCancelled_;
  std::function<void(const CombatEvent&)> onApplied_;

  static void FirePipeline(std::unordered_map<std::string, DamageRule>& rules,
                           CombatEvent& evt, bool* cancelled);
};

}  // namespace Berry