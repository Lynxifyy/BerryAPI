#pragma once
#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <array>

namespace Berry {

//----------------------------------------------------------------------------
// Game model — the native-world-facing view of entities, blocks and level
// state that BerryAPI exposes to its modules.
//
// These are intentionally thin and version-agnostic: every accessor goes
// through a "seam" that the shim (host) or a per-version game struct offset
// table (on-device) answers. Modules never poke raw engine memory directly;
// they talk to these objects, which keeps modules portable across engine
// versions and makes all of BerryAPI unit-testable without a game binary.
//----------------------------------------------------------------------------

enum class EntityType : u32 { Player, Mob, Projectile, Item, Vehicle, Unknown };

/// The lowercase name of an entity type ("player", "mob", ...). The inverse of
/// `ParseEntityType`, so a name round-trips through a script call.
BERRY_API const char* EntityTypeName(EntityType t);
/// Parses a type name. Returns false for an unknown name rather than guessing
/// `Unknown`, so a filter typo is an error and not a silent match-everything.
BERRY_API bool ParseEntityType(const std::string& name, EntityType* out);

enum class DamageCause : u32 {
  None = 0,
  Contact, EntityAttack, EntitySweepAttack, Projectile, Suffocation, Fall,
  Fire, FireTick, Lava, Drowning, BlockExplosion, EntityExplosion, Void,
  Magic, Wither, Anvil, FallingBlock, Thorns, Fatal, Starve, OutOfWorld,
  Lightning, Suiciding, Stalagmite
};

struct BERRY_API EntityId {
  u64 raw = 0;   // raw network/engine id
  u16 runtime = 0; // per-level runtime id (MCPE uses runtime entity ids)
  bool operator==(const EntityId& o) const { return raw == o.raw && runtime == o.runtime; }
};

struct BERRY_API EntitySnapshot {
  EntityId id;
  EntityType type = EntityType::Unknown;
  Vec3 position;
  Vec3 velocity;
  Vec3 rotation;   // pitch,yaw,roll
  f32 health = -1;
  f32 maxHealth = -1;
  bool alive = false;
  bool onGround = true;
  std::string name;      // display name (players) or type name
  u64 lastSeenTick = 0;
};

struct BERRY_API LevelTime {
  i64 absolute = 0;   // total day/night ticks
  f32 daylight = 0;   // 0..1 (0 = night, 1 = midday)
  bool day = true;
  f32 rainLevel = 0;
  f32 thunderLevel = 0;
};

// Callback used when a game event occurs (damage, tick, block change…)
using GameCallback = std::function<void(const void* evt)>;

//----------------------------------------------------------------------------
// LevelSeam — ABSTRACT ENGINE INTERFACE.
//
// On device the World module fills this from resolved game pointers and hooks.
// On the host, the shim provides a simulated Level so every BerryAPI module is
// fully exercised in tests.
//----------------------------------------------------------------------------

class BERRY_API LevelSeam {
 public:
  virtual ~LevelSeam() = default;

  virtual const char* Name() const = 0;

  /// Snapshot of an entity by runtime id (0 = not present).
  virtual bool GetEntity(EntityId id, EntitySnapshot* out) const = 0;
  virtual std::vector<EntityId> AllEntities() const = 0;
  /// Entities within an AABB (axis-aligned, world space).
  virtual std::vector<EntityId> EntitiesInAabb(const Aabb& box,
                                               EntityType filter = EntityType::Unknown) const = 0;
  /// Entities within a radius of a point.
  virtual std::vector<EntityId> EntitiesInRadius(const Vec3& center, f32 radius) const = 0;
  /// Raycast: returns hit entity id + distance.
  virtual bool RaycastEntity(const Vec3& origin, const Vec3& dir, f32 maxDist,
                             EntityId* outId, f32* outDist) const = 0;
  /// Raycast blocks: returns hit position + block id.
  virtual bool RaycastBlock(const Vec3& origin, const Vec3& dir, f32 maxDist,
                            Vec3* outPos, u32* outBlockType) const = 0;

  /// Block id at integer coordinates (0 = air). Read-only.
  virtual u32 GetBlock(const Vec3& worldPos) const = 0;

  /// Read-only level time.
  virtual LevelTime GetTime() const = 0;
};

//----------------------------------------------------------------------------
// ActorController — read-only observation of one entity (usually the local
// player). World access through BerryAPI is read-only: none of these mutate
// the game. Mutating wrappers were removed in v2 (see 0.2).
//----------------------------------------------------------------------------

class BERRY_API ActorController {
 public:
  ActorController(LevelSeam& seam, EntityId id) : seam_(seam), id_(id) {}

  EntityId Id() const { return id_; }
  bool IsValid() const;
  EntitySnapshot Snapshot() const;

  Vec3 Position() const;
  Vec3 Velocity() const;

  f32 Health() const;

 private:
  LevelSeam& seam_;
  EntityId id_;
};

}  // namespace Berry