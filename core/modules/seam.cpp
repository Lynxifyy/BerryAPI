// The C-ABI world-seam adapter. See include/berry/seam.h.
//
// A host that is not linked against BerryAPI's C++ ABI fills a `BerrySeamC` of
// function pointers; this wraps it in a `Berry::LevelSeam` so the World module
// reads through it unchanged. Every read that the host left null (or that the
// host reports as "no data") answers the same fail-closed default the
// `LevelSeam` interface documents, so a partial host degrades rather than
// fabricates.

#include "berry/seam.h"

#include "berry/engine.h"
#include "berry/game.h"
#include "berry/services.h"
#include "berry/world_query.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Berry {
namespace {

/// The largest id list one query may return, matching the grid's own cap so a
/// hostile or runaway host cannot make a query allocate without bound.
constexpr size_t kMaxIds = SpatialGrid::kMaxEntities;

}  // namespace

/// Wraps a host's C seam struct as a `LevelSeam`.
class SeamAdapter final : public LevelSeam {
 public:
  explicit SeamAdapter(const BerrySeamC& seam) : seam_(seam) {}

  const char* Name() const override { return "host:BerrySeamC"; }

  bool GetEntity(EntityId id, EntitySnapshot* out) const override {
    if (out == nullptr || seam_.get_entity == nullptr) return false;
    BerryEntitySnapshotC c{};
    if (seam_.get_entity(id.runtime, &c, seam_.user) == 0) return false;
    *out = FromC(c);
    return true;
  }

  std::vector<EntityId> AllEntities() const override {
    return Collect(seam_.all_entities);
  }

  std::vector<EntityId> EntitiesInRadius(const Vec3& center, f32 radius) const override {
    if (seam_.entities_in_radius == nullptr) return {};
    std::vector<uint64_t> ids(kMaxIds);
    size_t count = 0;
    if (seam_.entities_in_radius(center.x, center.y, center.z, radius, ids.data(),
                                 ids.size(), &count, seam_.user) == 0) {
      return {};
    }
    return ToIds(ids, count);
  }

  std::vector<EntityId> EntitiesInAabb(const Aabb& box,
                                       EntityType filter) const override {
    if (seam_.entities_in_aabb == nullptr) return {};
    std::vector<uint64_t> ids(kMaxIds);
    size_t count = 0;
    if (seam_.entities_in_aabb(box.min.x, box.min.y, box.min.z, box.max.x,
                               box.max.y, box.max.z, static_cast<uint32_t>(filter),
                               ids.data(), ids.size(), &count, seam_.user) == 0) {
      return {};
    }
    return ToIds(ids, count);
  }

  bool RaycastEntity(const Vec3& origin, const Vec3& dir, f32 maxDist, EntityId* outId,
                     f32* outDist) const override {
    if (seam_.raycast_entity == nullptr || outId == nullptr || outDist == nullptr) {
      return false;
    }
    uint64_t id = 0;
    f32 dist = 0;
    if (seam_.raycast_entity(origin.x, origin.y, origin.z, dir.x, dir.y, dir.z,
                             maxDist, &id, &dist, seam_.user) == 0) {
      return false;
    }
    EntityId e;
    e.raw = id;
    *outId = e;
    *outDist = dist;
    return true;
  }

  bool RaycastBlock(const Vec3& origin, const Vec3& dir, f32 maxDist, Vec3* outPos,
                    u32* outBlockType) const override {
    if (seam_.raycast_block == nullptr || outPos == nullptr || outBlockType == nullptr) {
      return false;
    }
    float x = 0, y = 0, z = 0;
    uint32_t block = 0;
    if (seam_.raycast_block(origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, maxDist,
                            &x, &y, &z, &block, seam_.user) == 0) {
      return false;
    }
    *outPos = Vec3(x, y, z);
    *outBlockType = block;
    return true;
  }

  u32 GetBlock(const Vec3& worldPos) const override {
    if (seam_.get_block == nullptr) return 0;
    // The seam reads integer block coordinates; floor so a fractional position
    // lands in the block it is inside, matching how the game addresses blocks.
    return seam_.get_block(static_cast<i32>(std::floor(worldPos.x)),
                           static_cast<i32>(std::floor(worldPos.y)),
                           static_cast<i32>(std::floor(worldPos.z)), seam_.user);
  }

  LevelTime GetTime() const override {
    LevelTime t;
    if (seam_.get_time == nullptr) return t;
    BerryLevelTimeC c{};
    if (seam_.get_time(&c, seam_.user) == 0) return t;
    t.absolute = c.absolute;
    t.daylight = c.daylight;
    t.day = c.day != 0;
    t.rainLevel = c.rain_level;
    t.thunderLevel = c.thunder_level;
    return t;
  }

  std::string GetBiome(const BlockPos& pos) const override {
    if (seam_.get_biome == nullptr) return std::string();
    char buffer[128];
    buffer[0] = '\0';
    if (seam_.get_biome(pos.x, pos.y, pos.z, buffer, sizeof(buffer), seam_.user) == 0) {
      return std::string();
    }
    buffer[sizeof(buffer) - 1] = '\0';
    return std::string(buffer);
  }

  i32 GetLight(const BlockPos& pos) const override {
    if (seam_.get_light == nullptr) return -1;
    return seam_.get_light(pos.x, pos.y, pos.z, seam_.user);
  }

  ChunkState GetChunkState(const BlockPos& pos) const override {
    if (seam_.get_chunk_state == nullptr) return ChunkState::Unloaded;
    const int32_t state = seam_.get_chunk_state(pos.x, pos.y, pos.z, seam_.user);
    if (state <= 0) return ChunkState::Unloaded;
    if (state == 1) return ChunkState::Loaded;
    return ChunkState::Rendered;
  }

  LocalPlayerState GetLocalPlayerState() const override {
    LocalPlayerState s;
    if (seam_.get_local_player == nullptr) return s;
    BerryLocalPlayerC c{};
    if (seam_.get_local_player(&c, seam_.user) == 0 || c.valid == 0) return s;
    s.valid = true;
    s.hunger = c.hunger;
    s.saturation = c.saturation;
    s.armorPoints = c.armor_points;
    s.gamemode = c.gamemode;
    s.dimension = c.dimension;
    s.xpLevel = c.xp_level;
    s.xpProgress = c.xp_progress;
    s.air = c.air;
    s.onFire = c.on_fire != 0;
    s.sneaking = c.sneaking != 0;
    s.sprinting = c.sprinting != 0;
    s.position = Vec3(c.px, c.py, c.pz);
    s.rotation = Vec3(c.pitch, c.yaw, c.roll);
    return s;
  }

 private:
  using AllFn = int32_t (*)(uint64_t*, size_t, size_t*, void*);

  std::vector<EntityId> Collect(AllFn fn) const {
    if (fn == nullptr) return {};
    std::vector<uint64_t> ids(kMaxIds);
    size_t count = 0;
    if (fn(ids.data(), ids.size(), &count, seam_.user) == 0) return {};
    return ToIds(ids, count);
  }

  static std::vector<EntityId> ToIds(const std::vector<uint64_t>& ids, size_t count) {
    if (count > ids.size()) count = ids.size();
    std::vector<EntityId> out;
    out.reserve(count);
    for (size_t i = 0; i < count; i++) {
      EntityId e;
      e.raw = ids[i];
      out.push_back(e);
    }
    return out;
  }

  static EntitySnapshot FromC(const BerryEntitySnapshotC& c) {
    EntitySnapshot s;
    s.id.raw = c.raw_id;
    s.id.runtime = c.runtime_id;
    s.type = static_cast<EntityType>(c.type);
    s.position = Vec3(c.px, c.py, c.pz);
    s.velocity = Vec3(c.vx, c.vy, c.vz);
    s.rotation = Vec3(c.pitch, c.yaw, c.roll);
    s.health = c.health;
    s.maxHealth = c.max_health;
    s.alive = c.alive != 0;
    s.onGround = c.on_ground != 0;
    if (c.name != nullptr) s.name = c.name;
    s.lastSeenTick = c.last_seen_tick;
    return s;
  }

  BerrySeamC seam_{};
};

namespace {

std::mutex g_seamMutex;
std::shared_ptr<SeamAdapter> g_seam;

}  // namespace

}  // namespace Berry

extern "C" {

BERRY_API int32_t BERRY_SetWorldSeam(const BerrySeamC* seam) {
  if (seam == nullptr) return 0;
  if (seam->size < sizeof(BerrySeamC)) return 0;
  if (seam->abi_version != BERRY_SEAM_ABI_VERSION) return 0;

  Berry::Engine& engine = Berry::Engine::Instance();
  // No seam can bind before the module set is attached: there is no World module
  // to own it, and binding one later would race the tick loop that reads it.
  if (!engine.attached()) return 0;
  Berry::WorldHandle* world = Berry::WorldFrom(engine.Modules());
  if (world == nullptr) return 0;

  std::lock_guard<std::mutex> lock(Berry::g_seamMutex);
  auto adapter = std::make_shared<Berry::SeamAdapter>(*seam);
  world->SetSeam(adapter);
  Berry::g_seam = adapter;
  return 1;
}

BERRY_API void BERRY_ClearWorldSeam(void) {
  Berry::Engine& engine = Berry::Engine::Instance();
  std::lock_guard<std::mutex> lock(Berry::g_seamMutex);
  if (Berry::WorldHandle* world = Berry::WorldFrom(engine.Modules())) {
    world->SetSeam(nullptr);
  }
  Berry::g_seam.reset();
}

BERRY_API int32_t BERRY_HasWorldSeam(void) {
  std::lock_guard<std::mutex> lock(Berry::g_seamMutex);
  return Berry::g_seam != nullptr ? 1 : 0;
}

BERRY_API uint32_t BERRY_WorldSeamStructSize(void) {
  return static_cast<uint32_t>(sizeof(BerrySeamC));
}

BERRY_API uint32_t BERRY_WorldSeamAbiVersion(void) {
  return static_cast<uint32_t>(BERRY_SEAM_ABI_VERSION);
}

}  // extern "C"
