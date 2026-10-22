#include "berry/game.h"
#include "berry/logger.h"

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