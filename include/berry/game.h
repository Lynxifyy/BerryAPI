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

/// A block coordinate, as `world.get_biome` / `world.get_light` take it.
struct BERRY_API BlockPos {
  i32 x = 0, y = 0, z = 0;
  BlockPos() = default;
  BlockPos(i32 x_, i32 y_, i32 z_) : x(x_), y(y_), z(z_) {}
  bool operator==(const BlockPos& o) const { return x == o.x && y == o.y && z == o.z; }
  bool operator!=(const BlockPos& o) const { return !(*this == o); }
  bool operator<(const BlockPos& o) const {
    if (x != o.x) return x < o.x;
    if (y != o.y) return y < o.y;
    return z < o.z;
  }
};

/// The chunk load state a `world.get_chunk_state` read reports.
enum class ChunkState : u32 {
  Unloaded = 0,   // no chunk record for the coordinate
  Loaded,         // present in memory, not necessarily drawn
  Rendered,       // present and currently in the draw set
};

BERRY_API const char* ChunkStateName(ChunkState s);
/// Parses a chunk-state name. Returns false for an unknown name rather than
/// guessing, so a typo fails closed.
BERRY_API bool ParseChunkState(const std::string& name, ChunkState* out);

/// One active status effect on the local player.
struct BERRY_API PlayerEffect {
  std::string name;      // "speed", "strength", …
  i32 amplifier = 0;
  f32 secondsLeft = 0;
  bool ambient = false;
};

/// The local player's own state, beyond what an EntitySnapshot carries. All of
/// this is state the client already holds for its own player, so exposing it
/// reveals nothing the client was not already sent.
struct BERRY_API LocalPlayerState {
  bool valid = false;         // false until the local player is bound
  i32 hunger = 20;            // 0..20 food level
  f32 saturation = 0;         // hidden saturation value
  i32 armorPoints = 0;        // total armour points (0..20)
  i32 gamemode = 0;           // 0 survival, 1 creative, 2 adventure, 3 spectator
  i32 dimension = 0;          // 0 overworld, 1 nether, 2 end
  i32 xpLevel = 0;
  f32 xpProgress = 0;         // 0..1 toward the next level
  f32 air = 300;              // remaining breath, in ticks
  bool onFire = false;
  bool sneaking = false;
  bool sprinting = false;
  std::vector<PlayerEffect> effects;
};

BERRY_API const char* GameModeName(i32 mode);
BERRY_API const char* DimensionName(i32 dimension);

/// The axis-aligned bounding box of an entity, derived from its snapshot
/// position and type. The core never reads engine hitbox memory: the size is a
/// fixed, per-type constant (players/mobs ~0.6x1.8, projectiles/items small),
/// and the box sits on the entity's feet. This is what `entity.get_hitbox` and
/// `entity.trace_line` report, so both agree on one definition.
BERRY_API Aabb EntityHitbox(const EntitySnapshot& snap);

/// Ray/box intersection (slab method). `dir` need not be normalized; `outDist`
/// is the parametric distance along `dir` to the entry point, so a caller that
/// normalized `dir` reads it as a world distance. Returns false when the ray
/// misses or the box is behind the origin.
BERRY_API bool RayHitsAabb(const Vec3& origin, const Vec3& dir, const Aabb& box,
                           f32 maxDist, f32* outDist);

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

  /// The biome name at a block coordinate, or "" when unknown. Read-only; the
  /// default answers "" so an adapter that does not implement it fails closed
  /// rather than fabricating a biome.
  virtual std::string GetBiome(const BlockPos& pos) const {
    (void)pos;
    return std::string();
  }

  /// The light level (0..15) at a block coordinate, or -1 when unknown. The
  /// default answers -1, so a missing implementation reads as "no data".
  virtual i32 GetLight(const BlockPos& pos) const {
    (void)pos;
    return -1;
  }

  /// The load state of the chunk containing a block coordinate. The default
  /// reports Unloaded, so an adapter that cannot answer never claims a chunk
  /// is present.
  virtual ChunkState GetChunkState(const BlockPos& pos) const {
    (void)pos;
    return ChunkState::Unloaded;
  }

  /// The local player's own state. `valid` is false by default, so an adapter
  /// that does not report it fails closed instead of inventing stats.
  virtual LocalPlayerState GetLocalPlayerState() const {
    return LocalPlayerState{};
  }
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