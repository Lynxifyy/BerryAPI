// Host tests for the world-seam C ABI (include/berry/seam.h, core/modules/seam.cpp).
//
// Covers:
//   * Refusal: a null struct, a short `size`, a wrong `abi_version` and a call
//     before the engine is attached are all refused, and nothing is bound.
//   * Binding: a valid struct binds, and every read routes through the host's
//     callback with the host's own `user` context passed back unchanged.
//   * Fail-closed: a null callback answers the LevelSeam default (no entity,
//     empty list, air, default time, "" biome, -1 light, Unloaded chunk, an
//     invalid local player) rather than fabricating a value.
//   * Unbind: BERRY_ClearWorldSeam removes the seam and the reads fall back to
//     their defaults; BERRY_HasWorldSeam tracks the state.

#include "berry/seam.h"
#include "berry/engine.h"
#include "berry/game.h"
#include "berry/services.h"

#include <cstdio>
#include <cstring>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

/// The host context, so a test can prove the adapter passes `user` through.
struct HostCtx {
  int getEntityCalls = 0;
  int allCalls = 0;
  int radiusCalls = 0;
  int blockCalls = 0;
  int timeCalls = 0;
  int biomeCalls = 0;
  int lightCalls = 0;
  int chunkCalls = 0;
  int localCalls = 0;
  int seenUserMatches = 0;
  HostCtx* expectedSelf = this;
};

int32_t GetEntity(uint64_t runtimeId, BerryEntitySnapshotC* out, void* user) {
  auto* ctx = static_cast<HostCtx*>(user);
  if (ctx == nullptr || out == nullptr) return 0;
  ctx->getEntityCalls++;
  if (user == ctx->expectedSelf) ctx->seenUserMatches++;
  if (runtimeId != 42) return 0;
  std::memset(out, 0, sizeof(*out));
  out->raw_id = 7;
  out->runtime_id = 42;
  out->type = static_cast<uint32_t>(EntityType::Player);
  out->px = 1.0f; out->py = 2.0f; out->pz = 3.0f;
  out->vx = 0.5f; out->vy = 0.0f; out->vz = -0.5f;
  out->pitch = 10.0f; out->yaw = 20.0f; out->roll = 0.0f;
  out->health = 18.0f; out->max_health = 20.0f;
  out->alive = 1; out->on_ground = 1;
  out->name = "Tester";
  out->last_seen_tick = 99;
  return 1;
}

int32_t AllEntities(uint64_t* ids, size_t cap, size_t* count, void* user) {
  auto* ctx = static_cast<HostCtx*>(user);
  if (ctx == nullptr || ids == nullptr || count == nullptr) return 0;
  ctx->allCalls++;
  size_t n = cap < 2 ? cap : 2;
  for (size_t i = 0; i < n; i++) ids[i] = 100 + i;
  *count = n;
  return 1;
}

int32_t EntitiesInRadius(float x, float y, float z, float radius, uint64_t* ids,
                         size_t cap, size_t* count, void* user) {
  auto* ctx = static_cast<HostCtx*>(user);
  if (ctx == nullptr || ids == nullptr || count == nullptr) return 0;
  ctx->radiusCalls++;
  (void)x; (void)y; (void)z; (void)radius;
  if (cap == 0) { *count = 0; return 1; }
  ids[0] = 555;
  *count = 1;
  return 1;
}

uint32_t GetBlock(int32_t x, int32_t y, int32_t z, void* user) {
  auto* ctx = static_cast<HostCtx*>(user);
  if (ctx != nullptr) ctx->blockCalls++;
  // A known block at (1,2,3) and air everywhere else.
  return (x == 1 && y == 2 && z == 3) ? 17u : 0u;
}

int32_t GetTime(BerryLevelTimeC* out, void* user) {
  auto* ctx = static_cast<HostCtx*>(user);
  if (ctx == nullptr || out == nullptr) return 0;
  ctx->timeCalls++;
  out->absolute = 12000;
  out->daylight = 0.25f;
  out->day = 0;
  out->rain_level = 0.5f;
  out->thunder_level = 0.1f;
  return 1;
}

int32_t GetBiome(int32_t x, int32_t y, int32_t z, char* out, size_t cap, void* user) {
  auto* ctx = static_cast<HostCtx*>(user);
  if (ctx == nullptr || out == nullptr || cap == 0) return 0;
  ctx->biomeCalls++;
  (void)x; (void)y; (void)z;
  const char* name = "plains";
  size_t n = std::strlen(name);
  if (n >= cap) n = cap - 1;
  std::memcpy(out, name, n);
  out[n] = '\0';
  return 1;
}

int32_t GetLight(int32_t x, int32_t y, int32_t z, void* user) {
  auto* ctx = static_cast<HostCtx*>(user);
  if (ctx != nullptr) ctx->lightCalls++;
  (void)x; (void)y; (void)z;
  return 12;
}

int32_t GetChunkState(int32_t x, int32_t y, int32_t z, void* user) {
  auto* ctx = static_cast<HostCtx*>(user);
  if (ctx != nullptr) ctx->chunkCalls++;
  (void)x; (void)y; (void)z;
  return 2;  // Rendered
}

int32_t GetLocalPlayer(BerryLocalPlayerC* out, void* user) {
  auto* ctx = static_cast<HostCtx*>(user);
  if (ctx == nullptr || out == nullptr) return 0;
  ctx->localCalls++;
  std::memset(out, 0, sizeof(*out));
  out->valid = 1;
  out->hunger = 15;
  out->saturation = 2.5f;
  out->armor_points = 8;
  out->gamemode = 1;
  out->dimension = 2;
  out->xp_level = 30;
  out->xp_progress = 0.5f;
  out->air = 250.0f;
  out->on_fire = 1;
  out->sneaking = 0;
  out->sprinting = 1;
  out->px = 4.0f; out->py = 65.0f; out->pz = -7.0f;
  out->pitch = -12.0f; out->yaw = 90.0f; out->roll = 0.0f;
  return 1;
}

BerrySeamC MakeSeam(HostCtx* ctx) {
  BerrySeamC seam{};
  seam.size = sizeof(BerrySeamC);
  seam.abi_version = BERRY_SEAM_ABI_VERSION;
  seam.user = ctx;
  seam.get_entity = GetEntity;
  seam.all_entities = AllEntities;
  seam.entities_in_radius = EntitiesInRadius;
  seam.get_block = GetBlock;
  seam.get_time = GetTime;
  seam.get_biome = GetBiome;
  seam.get_light = GetLight;
  seam.get_chunk_state = GetChunkState;
  seam.get_local_player = GetLocalPlayer;
  return seam;
}

/// The bound seam the World module is reading, or null.
LevelSeam* BoundSeam() {
  WorldHandle* world = WorldFrom(Engine::Instance().Modules());
  if (world == nullptr) return nullptr;
  return world->Seam().get();
}

}  // namespace

int main() {
  Engine& engine = Engine::Instance();

  // --- Refusal before attach: there is no World module to own a seam. ---
  HostCtx ctx;
  BerrySeamC seam = MakeSeam(&ctx);
  CHECK(BERRY_SetWorldSeam(&seam) == 0);
  CHECK(BERRY_HasWorldSeam() == 0);

  // --- Malformed structs are refused even once attached. ---
  CHECK(engine.Bootstrap());
  CHECK(engine.Attach());
  CHECK(BERRY_SetWorldSeam(nullptr) == 0);
  BerrySeamC shortSeam = MakeSeam(&ctx);
  shortSeam.size = sizeof(BerrySeamC) - 1;
  CHECK(BERRY_SetWorldSeam(&shortSeam) == 0);
  BerrySeamC badAbi = MakeSeam(&ctx);
  badAbi.abi_version = BERRY_SEAM_ABI_VERSION + 1;
  CHECK(BERRY_SetWorldSeam(&badAbi) == 0);
  CHECK(BERRY_HasWorldSeam() == 0);

  // --- A valid seam binds. ---
  CHECK(BERRY_SetWorldSeam(&seam) == 1);
  CHECK(BERRY_HasWorldSeam() == 1);
  LevelSeam* bound = BoundSeam();
  CHECK(bound != nullptr);

  // --- Entity read routes through the host, with `user` passed back. ---
  EntityId id;
  id.raw = 7;
  id.runtime = 42;
  EntitySnapshot snap;
  CHECK(bound->GetEntity(id, &snap));
  CHECK(ctx.getEntityCalls == 1);
  CHECK(ctx.seenUserMatches == 1);
  CHECK(snap.id.raw == 7);
  CHECK(snap.type == EntityType::Player);
  CHECK(snap.position.x == 1.0f && snap.position.y == 2.0f && snap.position.z == 3.0f);
  CHECK(snap.health == 18.0f && snap.maxHealth == 20.0f);
  CHECK(snap.alive && snap.onGround);
  CHECK(snap.name == "Tester");
  CHECK(snap.lastSeenTick == 99);
  // An unknown runtime id reads as "no entity".
  EntityId missing;
  missing.runtime = 999;
  CHECK(!bound->GetEntity(missing, &snap));

  // --- List reads. ---
  std::vector<EntityId> all = bound->AllEntities();
  CHECK(all.size() == 2);
  CHECK(all[0].raw == 100 && all[1].raw == 101);
  CHECK(ctx.allCalls == 1);

  std::vector<EntityId> near = bound->EntitiesInRadius(Vec3(0, 0, 0), 16.0f);
  CHECK(near.size() == 1 && near[0].raw == 555);
  CHECK(ctx.radiusCalls == 1);

  // A null list callback answers empty, not garbage.
  BerrySeamC noLists = MakeSeam(&ctx);
  noLists.all_entities = nullptr;
  noLists.entities_in_radius = nullptr;
  noLists.entities_in_aabb = nullptr;
  CHECK(BERRY_SetWorldSeam(&noLists) == 1);
  bound = BoundSeam();
  CHECK(bound->AllEntities().empty());
  CHECK(bound->EntitiesInRadius(Vec3(0, 0, 0), 8.0f).empty());

  // --- Block, time, biome, light, chunk, local player. ---
  CHECK(bound->GetBlock(Vec3(1.9f, 2.1f, 3.0f)) == 17u);   // floored into (1,2,3)
  CHECK(bound->GetBlock(Vec3(9.0f, 9.0f, 9.0f)) == 0u);
  CHECK(ctx.blockCalls == 2);

  LevelTime t = bound->GetTime();
  CHECK(t.absolute == 12000);
  CHECK(t.daylight == 0.25f);
  CHECK(!t.day);
  CHECK(t.rainLevel == 0.5f && t.thunderLevel == 0.1f);
  CHECK(ctx.timeCalls == 1);

  BlockPos bp(1, 2, 3);
  CHECK(bound->GetBiome(bp) == "plains");
  CHECK(ctx.biomeCalls == 1);
  CHECK(bound->GetLight(bp) == 12);
  CHECK(ctx.lightCalls == 1);
  CHECK(bound->GetChunkState(bp) == ChunkState::Rendered);
  CHECK(ctx.chunkCalls == 1);

  LocalPlayerState lp = bound->GetLocalPlayerState();
  CHECK(lp.valid);
  CHECK(lp.hunger == 15);
  CHECK(lp.saturation == 2.5f);
  CHECK(lp.armorPoints == 8);
  CHECK(lp.gamemode == 1);
  CHECK(lp.dimension == 2);
  CHECK(lp.xpLevel == 30);
  CHECK(lp.xpProgress == 0.5f);
  CHECK(lp.air == 250.0f);
  CHECK(lp.onFire && !lp.sneaking && lp.sprinting);
  CHECK(lp.position.x == 4.0f && lp.position.y == 65.0f && lp.position.z == -7.0f);
  CHECK(lp.rotation.x == -12.0f && lp.rotation.y == 90.0f);
  CHECK(ctx.localCalls == 1);

  // --- A host that answers only part of the surface degrades, not fabricates. ---
  HostCtx sparse;
  BerrySeamC minimal{};
  minimal.size = sizeof(BerrySeamC);
  minimal.abi_version = BERRY_SEAM_ABI_VERSION;
  minimal.user = &sparse;
  CHECK(BERRY_SetWorldSeam(&minimal) == 1);
  bound = BoundSeam();
  CHECK(!bound->GetEntity(id, &snap));
  CHECK(bound->AllEntities().empty());
  CHECK(bound->GetBlock(Vec3(1, 2, 3)) == 0u);
  CHECK(bound->GetBiome(bp).empty());
  CHECK(bound->GetLight(bp) == -1);
  CHECK(bound->GetChunkState(bp) == ChunkState::Unloaded);
  CHECK(!bound->GetLocalPlayerState().valid);
  LevelTime empty = bound->GetTime();
  CHECK(empty.absolute == 0 && empty.daylight == 0.0f);

  // --- The struct-size / ABI probes a mirroring host checks before binding. ---
  CHECK(BERRY_WorldSeamStructSize() == sizeof(BerrySeamC));
  CHECK(BERRY_WorldSeamAbiVersion() == BERRY_SEAM_ABI_VERSION);

  // --- Unbind. ---
  BERRY_ClearWorldSeam();
  CHECK(BERRY_HasWorldSeam() == 0);
  CHECK(BoundSeam() == nullptr);
  // Re-binding after a clear works.
  CHECK(BERRY_SetWorldSeam(&seam) == 1);
  CHECK(BERRY_HasWorldSeam() == 1);
  BERRY_ClearWorldSeam();

  engine.Detach();

  if (g_fail == 0) {
    printf("test_seam: ALL OK\n");
    return 0;
  }
  printf("test_seam: %d FAILURE(S)\n", g_fail);
  return 1;
}
