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

void ActorController::SetPosition(const Vec3& p) {
  // The seam's SetPosition is reached through a virtual method registration.
  // For now, entity position mutation is provided by the shim/on-device
  // implementation; the module layer relies on the seam having a hook.
  (void)p;
}

Vec3 ActorController::Velocity() const {
  EntitySnapshot snap;
  return seam_.GetEntity(id_, &snap) ? snap.velocity : Vec3();
}

void ActorController::SetVelocity(const Vec3& v) { (void)v; }
void ActorController::AddVelocity(const Vec3& dv) { (void)dv; }
f32 ActorController::Health() const {
  EntitySnapshot snap;
  return seam_.GetEntity(id_, &snap) ? snap.health : -1;
}
void ActorController::SetHealth(f32 h) { (void)h; }

}  // namespace Berry