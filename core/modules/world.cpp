#include "berry/game.h"
#include "berry/logger.h"

namespace Berry {

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