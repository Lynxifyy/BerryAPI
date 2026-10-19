#include "berry/combat.h"
#include "berry/logger.h"
#include <chrono>
#include <algorithm>

namespace Berry {

using Clock = std::chrono::system_clock;

Combat::Combat() = default;
Combat::~Combat() = default;

bool Combat::Attach(DamageSeam* seam) {
  if (attached_) return false;
  seam_ = seam;
  attached_ = true;
  Log::Info("Berry::Combat attached (%s)",
            seam_ ? "damage seam present" : "no seam — metrics only");
  return true;
}

void Combat::Detach() {
  attached_ = false;
  seam_ = nullptr;
  Log::Info("Berry::Combat detached");
}

u64 Combat::AddPreRule(DamageRule rule) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto id = nextRuleId_++;
  preRules_[std::to_string(id)] = std::move(rule);
  return id;
}

u64 Combat::AddPostRule(DamageRule rule) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto id = nextRuleId_++;
  postRules_[std::to_string(id)] = std::move(rule);
  return id;
}

bool Combat::RemoveRule(u64 id) {
  std::lock_guard<std::mutex> lk(mutex_);
  std::string key = std::to_string(id);
  bool a = preRules_.erase(key) > 0;
  bool b = postRules_.erase(key) > 0;
  return a || b;
}

void Combat::FirePipeline(std::unordered_map<std::string, DamageRule>& rules,
                          CombatEvent& evt, bool* cancelled) {
  for (auto& [key, rule] : rules) {
    if (rule) {
      rule(evt);
      if (evt.cancelled) {
        *cancelled = true;
        break;
      }
    }
  }
}

CombatOutcome Combat::Damage(EntityId victim, EntityId attacker, f32 amt,
                             DamageCause cause) {
  if (!attached_) return CombatOutcome::Applied;

  CombatEvent evt;
  evt.victim = victim;
  evt.attacker = attacker;
  evt.baseDamage = amt;
  evt.finalDamage = amt;
  evt.cause = cause;

  bool cancelled = false;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    FirePipeline(preRules_, evt, &cancelled);
  }
  if (cancelled) {
    if (onCancelled_) onCancelled_(evt);
    EventBus::Instance().Emit("berry:combat", &evt);
    return CombatOutcome::Cancelled;
  }

  // Apply via seam, then run post rules.
  bool applied = !seam_ || seam_->Apply(evt);
  {
    std::lock_guard<std::mutex> lk(mutex_);
    FirePipeline(postRules_, evt, &cancelled);
  }
  if (applied && !cancelled) {
    if (onApplied_) onApplied_(evt);
    RecordHitUnlocked(attacker, evt.finalDamage > 0 ? evt.finalDamage : evt.baseDamage);
    EventBus::Instance().Emit("berry:combat", &evt);
    return CombatOutcome::Applied;
  }
  return CombatOutcome::Cancelled;
}

bool Combat::Kill(EntityId victim, EntityId attacker, DamageCause cause) {
  auto outcome = Damage(victim, attacker, 99999.0f, cause);
  if (outcome == CombatOutcome::Applied) {
    RecordKill(attacker, victim, cause);
    return true;
  }
  return false;
}

size_t Combat::AreaDamage(const Vec3& center, f32 radius, f32 amt,
                          DamageCause cause, EntityId source) {
  // The seam provides the entity list; without it, no entities to hit.
  size_t hits = 0;
  if (seam_) {
    // Combat does not own a LevelSeam; if the World module registered one it
    // is reachable via the engine, but to keep modules strict, damage here is
    // driven by whatever entities the caller already knows about. Callers can
    // use World::EntitiesInRadius + Combat::Damage per entity.
  }
  return hits;
}

void Combat::RecordHit(EntityId attacker, f32 damage) {
  std::lock_guard<std::mutex> lk(mutex_);
  RecordHitUnlocked(attacker, damage);
}

void Combat::RecordHitUnlocked(EntityId attacker, f32 damage) {
  if (!attacker.raw && !attacker.runtime) return;
  auto& m = metrics_[attacker.raw ? attacker.raw : attacker.runtime];
  m.damageDealt += damage;
  m.hits++;
  m.lastHitDamage = damage;
}

void Combat::RecordKill(EntityId killer, EntityId victim, DamageCause cause) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto& m = metrics_[killer.raw ? killer.raw : killer.runtime];
  m.kills++;
  KillAttribution ka;
  ka.killer = killer;
  ka.victim = victim;
  ka.timeUtc = Clock::to_time_t(Clock::now());
  ka.cause = cause;
  kills_.push_back(ka);
  if (kills_.size() > 64) kills_.erase(kills_.begin());
}

CombatMetrics Combat::Metrics(EntityId attacker) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = metrics_.find(attacker.raw ? attacker.raw : attacker.runtime);
  if (it == metrics_.end()) return {};
  return it->second;
}

std::vector<KillAttribution> Combat::RecentKills(size_t count) const {
  std::lock_guard<std::mutex> lk(mutex_);
  size_t start = kills_.size() > count ? kills_.size() - count : 0;
  return std::vector<KillAttribution>(kills_.begin() + start, kills_.end());
}

void Combat::OnCancelled(std::function<void(const CombatEvent&)> fn) {
  onCancelled_ = std::move(fn);
}
void Combat::OnApplied(std::function<void(const CombatEvent&)> fn) {
  onApplied_ = std::move(fn);
}

}  // namespace Berry