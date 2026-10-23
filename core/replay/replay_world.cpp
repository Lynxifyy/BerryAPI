// Replay playback. See include/berry/replay_world.h for the model.
#include "berry/replay_world.h"

#include "berry/replay_capture.h"

#include <algorithm>
#include <cmath>

namespace Berry {

ReplayWorld::ReplayWorld(const ReplayFile* file) : file_(file) {
  if (!file_ || file_->sections.empty()) return;
  first_ = file_->firstTick();
  last_ = file_->lastTick();
  for (const ReplaySection& s : file_->sections) {
    if (s.kind == ReplaySectionKind::Keyframe) keyframeTicks_.push_back(s.startTick);
  }
  // Index the markers once, so a scrubber does not walk the sections per frame.
  for (const ReplaySection& s : file_->sections) {
    for (const ReplayRecord& r : s.records) {
      if (r.kind != ReplayRecordKind::Event) continue;
      std::string ch, pl;
      if (!ReplayCodec::DecodeEvent(r.payload, &ch, &pl)) continue;
      ReplayMarker m;
      m.tick = r.tick;
      m.channel = std::move(ch);
      m.payload = std::move(pl);
      markers_.push_back(std::move(m));
    }
  }
  std::stable_sort(markers_.begin(), markers_.end(),
                   [](const ReplayMarker& a, const ReplayMarker& b) {
                     return a.tick < b.tick;
                   });
}

size_t ReplayWorld::MarkerCount(u64 from, u64 to) const {
  if (from > to) std::swap(from, to);
  size_t n = 0;
  for (const ReplayMarker& m : markers_) {
    if (m.tick >= from && m.tick <= to) ++n;
  }
  return n;
}

size_t ReplayWorld::KeyframeIndexFor(u64 tick) const {
  if (!file_) return 0;
  size_t best = 0;
  for (size_t i = 0; i < file_->sections.size(); ++i) {
    const ReplaySection& s = file_->sections[i];
    if (s.kind != ReplaySectionKind::Keyframe) continue;
    if (s.startTick <= tick) best = i;
    else break;
  }
  return best;
}

bool ReplayWorld::Seek(u64 tick) {
  if (!file_ || file_->sections.empty()) return false;
  if (tick < first_) tick = first_;
  if (tick > last_) tick = last_;

  entities_.clear();
  time_ = LevelTime{};

  // Start at the keyframe at or before `tick` and apply forward. A keyframe
  // resets the state; a delta is applied on top.
  const size_t start = KeyframeIndexFor(tick);
  for (size_t i = start; i < file_->sections.size(); ++i) {
    const ReplaySection& s = file_->sections[i];
    if (s.startTick > tick) break;
    for (const ReplayRecord& r : s.records) {
      if (r.tick > tick) continue;
      switch (r.kind) {
        case ReplayRecordKind::EntitySpawn:
        case ReplayRecordKind::EntityMove:
        case ReplayRecordKind::EntityMetadata:
        case ReplayRecordKind::PlayerInput: {
          EntitySnapshot snap;
          bool full = false;
          if (!ReplayCodec::DecodeEntity(r.payload, &snap, &full)) break;
          entities_[snap.id.raw] = std::move(snap);
          break;
        }
        case ReplayRecordKind::EntityLeave:
        case ReplayRecordKind::Equipment:
        case ReplayRecordKind::Skin: {
          // A leave removes the entity; equipment/skin carry the entity's id in
          // the payload, so decode it and drop it from the drawn set only for a
          // real leave. Equipment/skin changes are recorded as metadata today,
          // so this is only the leave path.
          if (r.kind == ReplayRecordKind::EntityLeave) {
            EntitySnapshot snap;
            bool full = false;
            if (!ReplayCodec::DecodeEntity(r.payload, &snap, &full)) break;
            entities_.erase(snap.id.raw);
          }
          break;
        }
        case ReplayRecordKind::TimeWeather: {
          LevelTime t;
          if (ReplayCodec::DecodeTime(r.payload, &t)) time_ = t;
          break;
        }
        default:
          break;
      }
    }
  }
  tick_ = tick;
  return true;
}

bool ReplayWorld::GetEntity(EntityId id, EntitySnapshot* out) const {
  auto it = entities_.find(id.raw);
  if (it == entities_.end()) return false;
  if (out) *out = it->second;
  return true;
}

std::vector<EntityId> ReplayWorld::AllEntities() const {
  std::vector<EntityId> out;
  out.reserve(entities_.size());
  for (const auto& [k, v] : entities_) out.push_back(v.id);
  return out;
}

std::vector<EntityId> ReplayWorld::EntitiesInAabb(const Aabb& box,
                                                 EntityType filter) const {
  std::vector<EntityId> out;
  for (const auto& [k, v] : entities_) {
    if (filter != EntityType::Unknown && v.type != filter) continue;
    if (box.Contains(v.position)) out.push_back(v.id);
  }
  return out;
}

std::vector<EntityId> ReplayWorld::EntitiesInRadius(const Vec3& center,
                                                    f32 radius) const {
  std::vector<EntityId> out;
  for (const auto& [k, v] : entities_) {
    if (v.alive && (v.position - center).Length() <= radius) out.push_back(v.id);
  }
  return out;
}

bool ReplayWorld::RaycastEntity(const Vec3& origin, const Vec3& dir, f32 maxDist,
                                EntityId* outId, f32* outDist) const {
  f32 best = maxDist;
  bool hit = false;
  EntityId bestId;
  for (const auto& [k, v] : entities_) {
    if (!v.alive) continue;
    f32 d = 0;
    if (RayHitsAabb(origin, dir, EntityHitbox(v), maxDist, &d) && d < best) {
      best = d;
      bestId = v.id;
      hit = true;
    }
  }
  if (hit) {
    if (outId) *outId = bestId;
    if (outDist) *outDist = best;
  }
  return hit;
}

bool ReplayWorld::RaycastBlock(const Vec3&, const Vec3&, f32, Vec3*, u32*) const {
  // A replay does not store blocks (it stores entities, time and packets), so a
  // block raycast fails closed rather than fabricating a hit.
  return false;
}

u32 ReplayWorld::GetBlock(const Vec3&) const {
  return 0;   // no block data in a replay; air, not a guess
}

LevelTime ReplayWorld::GetTime() const { return time_; }

}  // namespace Berry
