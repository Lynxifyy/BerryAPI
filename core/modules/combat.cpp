#include "berry/combat.h"
#include "berry/logger.h"
#include <chrono>
#include <algorithm>
#include <cmath>

namespace Berry {

using Clock = std::chrono::system_clock;

Combat::Combat() = default;
Combat::~Combat() = default;

bool Combat::Attach(DamageSeam* seam) {
  if (attached_) return false;
  seam_ = seam;
  attached_ = true;
  Log::Info("Berry::Combat attached (read-only observer; %s)",
            seam_ ? "damage seam present" : "no seam — metrics only");
  return true;
}

void Combat::Detach() {
  attached_ = false;
  seam_ = nullptr;
  Log::Info("Berry::Combat detached");
}

CombatOutcome Combat::Observe(const CombatEvent& evt) {
  // Fail closed: ignore invalid or nonsensical observations rather than
  // recording wrong data. A zero victim, non-finite damage, or negative
  // damage is not a real applied hit.
  if (!evt.victim.raw && !evt.victim.runtime) return CombatOutcome::Ignored;
  if (!std::isfinite(evt.finalDamage) || evt.finalDamage < 0) return CombatOutcome::Ignored;

  if (seam_ && !seam_->OnDamage(evt)) return CombatOutcome::Ignored;

  if (onApplied_) onApplied_(evt);
  RecordHit(evt.attacker, evt.finalDamage > 0 ? evt.finalDamage : evt.baseDamage);
  EventBus::Instance().Emit("berry:combat", const_cast<CombatEvent*>(&evt));
  return CombatOutcome::Observed;
}

void Combat::RecordHit(EntityId attacker, f32 damage) {
  std::lock_guard<std::mutex> lk(mutex_);
  RecordHitUnlocked(attacker, damage);
}

void Combat::RecordHitUnlocked(EntityId attacker, f32 damage) {
  if (!attacker.raw && !attacker.runtime) return;
  u64 key = attacker.raw ? attacker.raw : attacker.runtime;
  AttackerEntry& e = metrics_[key];
  if (e.metrics.hits == 0) e.firstHitNs = NowNs();
  e.metrics.damageDealt += damage;
  e.metrics.hits++;
  e.metrics.lastHitDamage = damage;
  // Rolling DPS window: at least one second of collected data before DPS is
  // meaningful.
  double elapsed = (double)((NowNs() - e.firstHitNs) / 1000000000ULL);
  e.metrics.windowSeconds = elapsed < 1.0 ? 0.0 : elapsed;
}

void Combat::RecordKill(EntityId killer, EntityId victim, DamageCause cause) {
  std::lock_guard<std::mutex> lk(mutex_);
  AttackerEntry& e = metrics_[killer.raw ? killer.raw : killer.runtime];
  e.metrics.kills++;
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
  return it->second.metrics;
}

std::vector<KillAttribution> Combat::RecentKills(size_t count) const {
  std::lock_guard<std::mutex> lk(mutex_);
  size_t start = kills_.size() > count ? kills_.size() - count : 0;
  return std::vector<KillAttribution>(kills_.begin() + start, kills_.end());
}

void Combat::OnApplied(std::function<void(const CombatEvent&)> fn) {
  onApplied_ = std::move(fn);
}

}  // namespace Berry