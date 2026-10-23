#include "berry/crystal.h"

#include <algorithm>
#include <cmath>

namespace Berry {

namespace {

/// Whether two entity ids name the same entity, using the raw id when either
/// carries one and the runtime id otherwise — the same rule the rest of the
/// codebase uses to compare ids that may have come from different sources.
bool SameEntity(EntityId a, EntityId b) {
  if (a.raw && b.raw) return a.raw == b.raw;
  return a.runtime == b.runtime;
}

/// The distance from a point to an entity's hitbox (0 when inside it), so a
/// large target is hit as the near face rather than its centre. Uses the
/// canonical `EntityHitbox`.
f32 DistanceToHitbox(const Vec3& p, const EntitySnapshot& snap) {
  const Aabb box = EntityHitbox(snap);
  const f32 dx = std::max(std::max(box.min.x - p.x, p.x - box.max.x), 0.0f);
  const f32 dy = std::max(std::max(box.min.y - p.y, p.y - box.max.y), 0.0f);
  const f32 dz = std::max(std::max(box.min.z - p.z, p.z - box.max.z), 0.0f);
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/// The one falloff formula: full damage at the epicentre, linearly to zero at
/// the blast radius. `EstimateDamage` and `Analyze` both route through it, so
/// the model can never disagree with itself.
f32 FalloffDamage(f32 distance, const CrystalConfig& cfg) {
  if (!std::isfinite(cfg.blastRadius) || cfg.blastRadius <= 0.0f) return 0.0f;
  if (distance >= cfg.blastRadius) return 0.0f;
  return cfg.maxDamage * (1.0f - distance / cfg.blastRadius);
}

}  // namespace

f32 CrystalOptimizer::EstimateDamage(const Vec3& center, const Vec3& targetPos,
                                     const CrystalConfig& cfg) {
  return FalloffDamage((targetPos - center).Length(), cfg);
}

CrystalConfig CrystalOptimizer::config() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return cfg_;
}

bool CrystalOptimizer::SetConfig(const CrystalConfig& cfg) {
  // Validate everything before taking the lock, so a bad value changes nothing.
  if (!std::isfinite(cfg.placeRange) || cfg.placeRange <= 0.0f) return false;
  if (cfg.placeRange > kMaxRange) return false;
  if (!std::isfinite(cfg.blastRadius) || cfg.blastRadius <= 0.0f) return false;
  if (cfg.blastRadius > kMaxBlastRadius) return false;
  if (!std::isfinite(cfg.maxDamage) || cfg.maxDamage < 0.0f) return false;
  if (cfg.minTargets < 0) return false;
  if (!std::isfinite(cfg.selfDamageWeight) || cfg.selfDamageWeight < 0.0f) return false;
  if (!std::isfinite(cfg.maxSelfDamage) || cfg.maxSelfDamage < 0.0f) return false;
  if (cfg.levels < 0 || cfg.levels > kMaxLevels) return false;
  if (cfg.maxResults == 0 || cfg.maxResults > kMaxCandidates) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  cfg_ = cfg;
  return true;
}

std::vector<CrystalPlacement> CrystalOptimizer::Analyze(
    const Vec3& origin, EntityId self, const std::vector<EntitySnapshot>& targets,
    const CrystalWorld& world, u64 tick) {
  CrystalConfig cfg;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    cfg = cfg_;
  }

  // The candidate grid: every block cell within `placeRange` horizontally, at
  // each level around the placer's feet. Bounded by the range and the level
  // count, so one call can never walk the world.
  const i32 r = (i32)std::floor(cfg.placeRange);
  const i32 baseY = (i32)std::floor(origin.y);
  std::vector<CrystalPlacement> found;
  u64 candidates = 0;
  bool full = false;

  for (i32 dy = -cfg.levels; dy <= cfg.levels && !full; ++dy) {
    const i32 y = baseY + dy;
    for (i32 dx = -r; dx <= r && !full; ++dx) {
      for (i32 dz = -r; dz <= r; ++dz) {
        const Vec3 cell((f32)dx + std::floor(origin.x), (f32)y,
                        (f32)dz + std::floor(origin.z));
        // Horizontal reach from the placer's feet, so a spot must be reachable.
        const f32 horiz = Vec2(cell.x - origin.x, cell.z - origin.z).Length();
        if (horiz > cfg.placeRange) continue;
        // The cap counts only spots actually tested, so it is exact.
        if (candidates >= kMaxCandidates) { full = true; break; }
        ++candidates;
        // A crystal only sits on an empty cell; a solid cell is occupied.
        if (world.BlockAt(cell) != 0) continue;

        CrystalPlacement p;
        p.position = cell;
        const Vec3 blastCenter = cell + Vec3(0.5f, 0.5f, 0.5f);
        for (const EntitySnapshot& t : targets) {
          // Measure to the target's hitbox face, not its centre, so a wide
          // target is not under-counted by a blast that lands beside it.
          const f32 dmg = FalloffDamage(DistanceToHitbox(blastCenter, t), cfg);
          if (dmg <= 0.0f) continue;
          if (SameEntity(t.id, self)) {
            p.selfDamage += dmg;
          } else {
            p.targetsHit++;
            p.totalDamage += dmg;
            if (dmg > p.bestDamage) p.bestDamage = dmg;
          }
        }
        // Also account for the blast hurting the placer when the placer is not
        // in `targets`: a spot at the placer's own feet still hurts them.
        if (p.selfDamage == 0.0f) {
          p.selfDamage = FalloffDamage((origin - blastCenter).Length(), cfg);
        }
        p.score = p.totalDamage - cfg.selfDamageWeight * p.selfDamage;

        if (p.targetsHit >= cfg.minTargets && p.selfDamage <= cfg.maxSelfDamage) {
          found.push_back(p);
        }
      }
    }
  }

  // Rank by score, then by the number of targets hit, then by position, so the
  // order is total and the same inputs always produce the same ranking.
  std::sort(found.begin(), found.end(),
            [](const CrystalPlacement& a, const CrystalPlacement& b) {
              if (a.score != b.score) return a.score > b.score;
              if (a.targetsHit != b.targetsHit) return a.targetsHit > b.targetsHit;
              if (a.position.x != b.position.x) return a.position.x < b.position.x;
              if (a.position.y != b.position.y) return a.position.y < b.position.y;
              return a.position.z < b.position.z;
            });
  if (found.size() > cfg.maxResults) found.resize(cfg.maxResults);

  std::lock_guard<std::mutex> lk(mutex_);
  results_ = found;
  lastTick_ = tick;
  stats_.analyses++;
  stats_.candidates += candidates;
  stats_.placements += found.size();
  if (!found.empty() && found.front().score > stats_.bestScore) {
    stats_.bestScore = found.front().score;
  }
  return found;
}

std::vector<CrystalPlacement> CrystalOptimizer::recommendations() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return results_;
}

CrystalPlacement CrystalOptimizer::best(bool* ok) const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (ok) *ok = !results_.empty();
  if (results_.empty()) return CrystalPlacement{};
  return results_.front();
}

CrystalStats CrystalOptimizer::stats() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return stats_;
}

void CrystalOptimizer::Reset() {
  std::lock_guard<std::mutex> lk(mutex_);
  results_.clear();
  stats_ = CrystalStats{};
  lastTick_ = 0;
}

bool CrystalOptimizer::armed() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return armed_;
}

bool CrystalOptimizer::Arm() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (armed_) return false;
  armed_ = true;
  return true;
}

void CrystalOptimizer::Disarm() {
  std::lock_guard<std::mutex> lk(mutex_);
  armed_ = false;
}

}  // namespace Berry
