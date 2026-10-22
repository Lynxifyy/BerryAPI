// Local combat-state telemetry. See include/berry/combat_state.h.
//
// Pure data: no clocks, no globals, no game writes. Every feed carries an
// explicit tick or millisecond so the host tests drive it deterministically.
#include "berry/combat_state.h"

#include "berry/event.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Berry {

CombatStateTracker::CombatStateTracker() { Clear(); }

//----------------------------------------------------------------------------
// Configuration
//----------------------------------------------------------------------------

bool CombatStateTracker::SetMaxReach(f32 blocks) {
  if (!std::isfinite(blocks) || blocks <= 0) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  maxReach_ = blocks;
  return true;
}

f32 CombatStateTracker::maxReach() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return maxReach_;
}

bool CombatStateTracker::SetClickBudget(f32 perSecond, f32 burst) {
  if (!std::isfinite(perSecond) || perSecond <= 0) return false;
  if (!std::isfinite(burst) || burst < 1.0f) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  clickPerSecond_ = perSecond;
  clickBurst_ = burst;
  for (ClickBucket& b : clicks_) {
    if (b.tokens > clickBurst_) b.tokens = clickBurst_;
  }
  return true;
}

//----------------------------------------------------------------------------
// Hitbox history
//----------------------------------------------------------------------------

void CombatStateTracker::ObserveEntity(const EntitySnapshot& snap, u64 tick) {
  const u64 key = Key(snap.id);
  if (!key) return;   // an id-less snapshot is not something to record

  std::lock_guard<std::mutex> lk(mutex_);

  auto it = history_.find(key);
  if (it == history_.end()) {
    // A new entity: evict the least-recently-updated one when at the cap, so the
    // map cannot grow without bound.
    if (history_.size() >= kMaxEntities) {
      if (!historyOrder_.empty()) {
        history_.erase(historyOrder_.front());
        historyOrder_.pop_front();
      }
    }
    historyOrder_.push_back(key);
    it = history_.emplace(key, std::deque<HitboxSample>{}).first;
  } else {
    // Move this key to the back of the recency order.
    historyOrder_.erase(
        std::remove(historyOrder_.begin(), historyOrder_.end(), key),
        historyOrder_.end());
    historyOrder_.push_back(key);
  }

  std::deque<HitboxSample>& q = it->second;
  HitboxSample s;
  s.tick = tick;
  s.box = EntityHitbox(snap);
  // Samples for one tick replace rather than accumulate, so a repeated feed for
  // the same tick does not fill the window with duplicates.
  if (!q.empty() && q.back().tick == tick) {
    q.back() = s;
  } else {
    q.push_back(s);
  }
  while (q.size() > kHistoryPerEntity) q.pop_front();
}

size_t CombatStateTracker::HitboxCount(EntityId id) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = history_.find(Key(id));
  return it == history_.end() ? 0 : it->second.size();
}

bool CombatStateTracker::HitboxAt(EntityId id, u64 tick, Aabb* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = history_.find(Key(id));
  if (it == history_.end()) return false;
  for (const HitboxSample& s : it->second) {
    if (s.tick == tick) {
      if (out) *out = s.box;
      return true;
    }
  }
  return false;
}

bool CombatStateTracker::HitboxAtOrBeforeLocked(EntityId id, u64 tick, Aabb* out,
                                               u64* outTick) const {
  auto it = history_.find(Key(id));
  if (it == history_.end()) return false;
  const HitboxSample* best = nullptr;
  for (const HitboxSample& s : it->second) {
    if (s.tick > tick) continue;
    if (!best || s.tick > best->tick) best = &s;
  }
  if (!best) return false;
  if (out) *out = best->box;
  if (outTick) *outTick = best->tick;
  return true;
}

bool CombatStateTracker::HitboxAtOrBefore(EntityId id, u64 tick, Aabb* out,
                                          u64* outTick) const {
  std::lock_guard<std::mutex> lk(mutex_);
  return HitboxAtOrBeforeLocked(id, tick, out, outTick);
}

std::vector<u64> CombatStateTracker::HitboxTicks(EntityId id) const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<u64> out;
  auto it = history_.find(Key(id));
  if (it == history_.end()) return out;
  out.reserve(it->second.size());
  for (const HitboxSample& s : it->second) out.push_back(s.tick);
  return out;
}

size_t CombatStateTracker::TrackedEntities() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return history_.size();
}

size_t CombatStateTracker::HitboxSamples() const {
  std::lock_guard<std::mutex> lk(mutex_);
  size_t n = 0;
  for (const auto& kv : history_) n += kv.second.size();
  return n;
}

//----------------------------------------------------------------------------
// Reach validation
//----------------------------------------------------------------------------

f32 CombatStateTracker::DistanceToBox(const Vec3& from, const Aabb& box) {
  // Closest point on the box to `from`; zero distance when `from` is inside.
  const f32 cx = std::min(std::max(from.x, box.min.x), box.max.x);
  const f32 cy = std::min(std::max(from.y, box.min.y), box.max.y);
  const f32 cz = std::min(std::max(from.z, box.min.z), box.max.z);
  return Vec3(cx, cy, cz).Distance(from);
}

ReachCheck CombatStateTracker::ObserveHit(const CombatEvent& evt, u64 tick) {
  ReachCheck rc;
  rc.attacker = evt.attacker;
  rc.victim = evt.victim;
  rc.tick = tick;

  std::vector<HitPrediction> expired;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    rc.maxReach = maxReach_;

    Aabb atkBox, vicBox;
    const bool haveAtk = HitboxAtOrBeforeLocked(evt.attacker, tick, &atkBox, nullptr);
    const bool haveVic = HitboxAtOrBeforeLocked(evt.victim, tick, &vicBox, nullptr);
    if (haveAtk && haveVic) {
      // The gap between the two boxes: the attacker's body centre to the
      // victim's box, 0 when they overlap. Deterministic and honest about what
      // the client actually knows.
      rc.distance = DistanceToBox(atkBox.Center(), vicBox);
      rc.valid = true;
      rc.withinReach = rc.distance <= maxReach_;
      if (!rc.withinReach) ++reachViolations_;
    }
    // A position we do not have is recorded as invalid, never as "in reach".
    lastReach_ = rc;

    // Expire anything past the window so a stale prediction cannot be confirmed
    // by a much later hit. (Confirmation itself is `ConfirmHit`, which also
    // publishes the confirmed event; this call only ages the queue.)
    expired = ExpirePredictionsLocked(tick);
  }

  for (const HitPrediction& p : expired) Publish(combat_event::Missed(), p);
  return rc;
}

ReachCheck CombatStateTracker::lastReach() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return lastReach_;
}

u64 CombatStateTracker::reachViolations() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return reachViolations_;
}

//----------------------------------------------------------------------------
// Swing timing
//----------------------------------------------------------------------------

SwingSample CombatStateTracker::ObserveSwing(EntityId attacker, u64 tick) {
  SwingSample s;
  s.attacker = attacker;
  s.tick = tick;

  std::lock_guard<std::mutex> lk(mutex_);
  std::deque<u64>& q = swingTicks_[Key(attacker)];
  if (!q.empty()) {
    const u64 prev = q.back();
    if (tick > prev) {
      s.deltaTicks = tick - prev;
      s.cps = kTicksPerSecond / (f64)s.deltaTicks;
      s.valid = true;
    }
  }
  q.push_back(tick);
  while (q.size() > kMaxSwingTicks) q.pop_front();
  lastSwing_ = s;
  return s;
}

SwingSample CombatStateTracker::lastSwing() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return lastSwing_;
}

f64 CombatStateTracker::AverageSwingCps(EntityId attacker) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = swingTicks_.find(Key(attacker));
  if (it == swingTicks_.end() || it->second.size() < 2) return 0;
  const std::deque<u64>& q = it->second;
  const u64 span = q.back() - q.front();
  if (span == 0) return 0;
  const u64 intervals = (u64)q.size() - 1;
  // Swings per second across the whole retained window.
  return (f64)intervals * kTicksPerSecond / (f64)span;
}

//----------------------------------------------------------------------------
// Projectile prediction
//----------------------------------------------------------------------------

Vec3 CombatStateTracker::Extrapolate(const Vec3& pos, const Vec3& vel, f32 gravity,
                                     u32 ticks) {
  // Semi-implicit integration at 20 tps: position advances by the velocity, then
  // the velocity is pulled by gravity for the next step. Matches how a Bedrock
  // projectile is stepped, so the prediction stays honest over a short flight.
  const f32 dt = 1.0f / 20.0f;
  Vec3 p = pos;
  Vec3 v = vel;
  for (u32 i = 0; i < ticks; ++i) {
    p = p + v * dt;
    v = v + Vec3(0.0f, gravity * dt, 0.0f);
  }
  return p;
}

void CombatStateTracker::NoteProjectile(EntityId id, const Vec3& predicted,
                                        u64 tick) {
  std::lock_guard<std::mutex> lk(mutex_);
  pendingProjectiles_[Key(id)] = PendingProjectile{predicted, tick};
}

f32 CombatStateTracker::ResolveProjectile(EntityId id, const Vec3& actual,
                                          u64 tick) {
  std::lock_guard<std::mutex> lk(mutex_);
  const u64 key = Key(id);
  auto it = pendingProjectiles_.find(key);
  if (it == pendingProjectiles_.end()) return -1.0f;

  PredictionRecord r;
  r.id = id;
  r.predicted = it->second.predicted;
  r.actual = actual;
  r.error = r.predicted.Distance(actual);
  r.tick = tick;
  pendingProjectiles_.erase(it);

  predictions_.push_back(r);
  while (predictions_.size() > kMaxPredictions) predictions_.pop_front();
  return r.error;
}

f32 CombatStateTracker::MeanPredictionError() const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (predictions_.empty()) return -1.0f;
  f64 sum = 0;
  for (const PredictionRecord& r : predictions_) sum += r.error;
  return (f32)(sum / (f64)predictions_.size());
}

size_t CombatStateTracker::PredictionCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return predictions_.size();
}

//----------------------------------------------------------------------------
// Hit prediction accuracy
//----------------------------------------------------------------------------

void CombatStateTracker::Publish(const char* channel, const HitPrediction& p) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%llu:%llu",
                (unsigned long long)p.attacker.raw,
                (unsigned long long)p.victim.raw);
  std::string payload(buf);
  const char* ptr = payload.c_str();
  EventBus::Instance().Emit(channel, &ptr);
}

void CombatStateTracker::PredictHit(EntityId attacker, EntityId victim, u64 tick) {
  HitPrediction p;
  p.attacker = attacker;
  p.victim = victim;
  p.tick = tick;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    ++predictedHits_;
    pendingHits_.push_back(p);
    while (pendingHits_.size() > kMaxPredictions) {
      // A prediction pushed out by volume can never be confirmed; count it a
      // miss so accuracy stays honest rather than silently dropping it.
      ++missedHits_;
      pendingHits_.pop_front();
    }
  }
  Publish(combat_event::Predicted(), p);
}

bool CombatStateTracker::ConfirmLocked(EntityId attacker, EntityId victim,
                                       HitPrediction* out) {
  for (auto it = pendingHits_.begin(); it != pendingHits_.end(); ++it) {
    if (it->attacker == attacker && it->victim == victim) {
      if (out) *out = *it;
      pendingHits_.erase(it);
      ++confirmedHits_;
      return true;
    }
  }
  return false;
}

bool CombatStateTracker::ConfirmHit(const CombatEvent& evt, u64 tick) {
  HitPrediction hit;
  hit.attacker = evt.attacker;
  hit.victim = evt.victim;
  hit.tick = tick;
  bool matched = false;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    matched = ConfirmLocked(evt.attacker, evt.victim, &hit);
  }
  if (matched) Publish(combat_event::Confirmed(), hit);
  return matched;
}

std::vector<HitPrediction> CombatStateTracker::ExpirePredictionsLocked(u64 nowTick) {
  std::vector<HitPrediction> expired;
  while (!pendingHits_.empty()) {
    const HitPrediction& front = pendingHits_.front();
    if (nowTick <= front.tick + kPredictionWindowTicks) break;
    ++missedHits_;
    expired.push_back(front);
    pendingHits_.pop_front();
  }
  return expired;
}

void CombatStateTracker::Tick(u64 nowTick) {
  std::vector<HitPrediction> expired;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    expired = ExpirePredictionsLocked(nowTick);
  }
  for (const HitPrediction& p : expired) Publish(combat_event::Missed(), p);
}

u64 CombatStateTracker::PredictedHits() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return predictedHits_;
}

u64 CombatStateTracker::ConfirmedHits() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return confirmedHits_;
}

u64 CombatStateTracker::MissedHits() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return missedHits_;
}

f64 CombatStateTracker::HitAccuracy() const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (predictedHits_ == 0) return 0;
  return (f64)confirmedHits_ / (f64)predictedHits_;
}

//----------------------------------------------------------------------------
// Click budget
//----------------------------------------------------------------------------

bool CombatStateTracker::NoteClick(MouseButton button, u64 nowMs) {
  const size_t idx = (size_t)button;
  if (idx >= clicks_.size()) return false;

  std::lock_guard<std::mutex> lk(mutex_);
  ClickBucket& b = clicks_[idx];
  if (b.lastMs == 0) {
    // First click: fill the bucket and stamp the time, so the first burst is not
    // immediately penalised for a bucket that was never refilled.
    b.tokens = clickBurst_;
    b.lastMs = nowMs;
  } else if (nowMs > b.lastMs) {
    const f64 elapsedSec = (f64)(nowMs - b.lastMs) / 1000.0;
    b.tokens = std::min(clickBurst_, b.tokens + (f32)(elapsedSec * clickPerSecond_));
    b.lastMs = nowMs;
  }

  b.stamps.push_back(nowMs);
  while (!b.stamps.empty() && nowMs > b.stamps.front() + 1000) b.stamps.pop_front();

  if (b.tokens >= 1.0f) {
    b.tokens -= 1.0f;
    clickLimited_ = false;
    return true;
  }
  clickLimited_ = true;
  return false;
}

f64 CombatStateTracker::ClickRate(MouseButton button) const {
  const size_t idx = (size_t)button;
  if (idx >= clicks_.size()) return 0;
  std::lock_guard<std::mutex> lk(mutex_);
  const ClickBucket& b = clicks_[idx];
  const u64 now = b.lastMs;
  size_t n = 0;
  for (u64 stamp : b.stamps) {
    if (now >= stamp && now - stamp <= 1000) ++n;
  }
  return (f64)n;
}

bool CombatStateTracker::ClickLimited() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return clickLimited_;
}

f32 CombatStateTracker::ClickTokens(MouseButton button) const {
  const size_t idx = (size_t)button;
  if (idx >= clicks_.size()) return 0;
  std::lock_guard<std::mutex> lk(mutex_);
  return clicks_[idx].tokens;
}

//----------------------------------------------------------------------------
// Reset
//----------------------------------------------------------------------------

void CombatStateTracker::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  history_.clear();
  historyOrder_.clear();
  lastReach_ = ReachCheck{};
  reachViolations_ = 0;
  swingTicks_.clear();
  lastSwing_ = SwingSample{};
  pendingProjectiles_.clear();
  predictions_.clear();
  pendingHits_.clear();
  predictedHits_ = confirmedHits_ = missedHits_ = 0;
  clicks_ = {};
  clickLimited_ = false;
}

}  // namespace Berry
