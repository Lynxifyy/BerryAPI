#include "berry/game.h"
#include "berry/logger.h"

#include <algorithm>
#include <cmath>

namespace Berry {

//----------------------------------------------------------------------------
// EntityType names.
//----------------------------------------------------------------------------

const char* EntityTypeName(EntityType t) {
  switch (t) {
    case EntityType::Player: return "player";
    case EntityType::Mob: return "mob";
    case EntityType::Projectile: return "projectile";
    case EntityType::Item: return "item";
    case EntityType::Vehicle: return "vehicle";
    case EntityType::Unknown: return "unknown";
  }
  return "unknown";
}

bool ParseEntityType(const std::string& name, EntityType* out) {
  static const struct { const char* n; EntityType t; } kTable[] = {
      {"player", EntityType::Player},     {"mob", EntityType::Mob},
      {"projectile", EntityType::Projectile}, {"item", EntityType::Item},
      {"vehicle", EntityType::Vehicle},   {"unknown", EntityType::Unknown},
  };
  for (const auto& e : kTable) {
    if (name == e.n) { *out = e.t; return true; }
  }
  return false;
}

const char* ChunkStateName(ChunkState s) {
  switch (s) {
    case ChunkState::Unloaded: return "unloaded";
    case ChunkState::Loaded: return "loaded";
    case ChunkState::Rendered: return "rendered";
  }
  return "unknown";
}

bool ParseChunkState(const std::string& name, ChunkState* out) {
  if (name == "unloaded") { *out = ChunkState::Unloaded; return true; }
  if (name == "loaded") { *out = ChunkState::Loaded; return true; }
  if (name == "rendered") { *out = ChunkState::Rendered; return true; }
  return false;
}

const char* GameModeName(i32 mode) {
  switch (mode) {
    case 0: return "survival";
    case 1: return "creative";
    case 2: return "adventure";
    case 3: return "spectator";
  }
  return "unknown";
}

const char* DimensionName(i32 dimension) {
  switch (dimension) {
    case 0: return "overworld";
    case 1: return "nether";
    case 2: return "end";
  }
  return "unknown";
}

Aabb EntityHitbox(const EntitySnapshot& snap) {
  // Per-type half-extents (x, z) and full height, in blocks. These are the
  // canonical Minecraft Bedrock sizes; the box sits on the entity's feet, so
  // its vertical centre is half the height above the snapshot position.
  f32 halfW = 0.3f, height = 1.8f;
  switch (snap.type) {
    case EntityType::Player: halfW = 0.3f; height = 1.8f; break;
    case EntityType::Mob: halfW = 0.3f; height = 1.4f; break;
    case EntityType::Projectile: halfW = 0.125f; height = 0.25f; break;
    case EntityType::Item: halfW = 0.125f; height = 0.25f; break;
    case EntityType::Vehicle: halfW = 0.7f; height = 0.7f; break;
    case EntityType::Unknown: halfW = 0.3f; height = 1.8f; break;
  }
  const Vec3 feet = snap.position;
  return Aabb(Vec3(feet.x - halfW, feet.y, feet.z - halfW),
              Vec3(feet.x + halfW, feet.y + height, feet.z + halfW));
}

bool RayHitsAabb(const Vec3& origin, const Vec3& dir, const Aabb& box,
                 f32 maxDist, f32* outDist) {
  // Slab method. Division by a zero component yields ±inf, which the min/max
  // comparisons handle correctly for an axis-aligned box.
  f32 tmin = 0.0f, tmax = maxDist;
  const f32 o[3] = {origin.x, origin.y, origin.z};
  const f32 d[3] = {dir.x, dir.y, dir.z};
  const f32 lo[3] = {box.min.x, box.min.y, box.min.z};
  const f32 hi[3] = {box.max.x, box.max.y, box.max.z};
  for (int i = 0; i < 3; ++i) {
    if (std::fabs(d[i]) < 1e-8f) {
      // Parallel to this slab: a miss when the origin is outside it.
      if (o[i] < lo[i] || o[i] > hi[i]) return false;
      continue;
    }
    f32 t1 = (lo[i] - o[i]) / d[i];
    f32 t2 = (hi[i] - o[i]) / d[i];
    if (t1 > t2) std::swap(t1, t2);
    if (t1 > tmin) tmin = t1;
    if (t2 < tmax) tmax = t2;
    if (tmin > tmax) return false;
  }
  if (outDist) *outDist = tmin;
  return true;
}

//----------------------------------------------------------------------------
// ActorController impls.
//
// The Berry::World module logic (LevelSeam wiring, entity cache, KV store)
// lives in services.cpp — this file only provides the ActorController helper
// that operates on a LevelSeam.
//----------------------------------------------------------------------------

bool ActorController::IsValid() const {
  EntitySnapshot snap;
  return seam_.GetEntity(id_, &snap);
}

EntitySnapshot ActorController::Snapshot() const {
  EntitySnapshot snap;
  seam_.GetEntity(id_, &snap);
  return snap;
}

Vec3 ActorController::Position() const {
  EntitySnapshot snap;
  return seam_.GetEntity(id_, &snap) ? snap.position : Vec3();
}

Vec3 ActorController::Velocity() const {
  EntitySnapshot snap;
  return seam_.GetEntity(id_, &snap) ? snap.velocity : Vec3();
}

f32 ActorController::Health() const {
  EntitySnapshot snap;
  return seam_.GetEntity(id_, &snap) ? snap.health : -1;
}

}  // namespace Berry