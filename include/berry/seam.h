#pragma once
// World-seam C ABI: lets a C-only host (a launcher's native preloader) bind the
// game state BerryAPI's World module reads.
//
// BerryAPI's World/Combat/Render modules read game state through a `LevelSeam`
// the embedder supplies (see berry/game.h). Until now that seam was a C++
// interface, so a host that is not linked against BerryAPI's C++ ABI — a
// launcher that `dlopen`s `libBerryAPI.so` and talks to it through the small C
// surface — could not bind one, and every world/entity read stayed fail-closed.
//
// This header closes that gap with a plain C struct of function pointers. The
// host fills it from whatever it can actually answer (the local player's own
// position and state are the usual first slice) and calls `BERRY_SetWorldSeam`.
// BerryAPI wraps the struct in an adapter that implements `Berry::LevelSeam`,
// so the modules are unchanged.
//
// Fail-closed throughout:
//   * `BERRY_SetWorldSeam` refuses a struct whose `size`/`abi_version` do not
//     match this build, so a host built against another ABI cannot bind a seam
//     with a misread layout.
//   * A null callback answers the same "no data" default its `LevelSeam`
//     method documents (no entity, empty list, air, default time), so a host
//     that can answer only part of the surface does not have to stub the rest.
//   * `BERRY_ClearWorldSeam` unbinds, and the modules fall back to their
//     fail-closed defaults (the same state as before a seam was ever bound).

#include "berry/export.h"
#include "berry/version.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// One entity, flattened so no C++ type crosses the boundary. `type` is a
/// `Berry::EntityType` value (0 Player, 1 Mob, 2 Projectile, 3 Item, 4 Vehicle,
/// 5 Unknown). `name` may be null; it is copied by the adapter, not retained.
typedef struct BerryEntitySnapshotC {
  uint64_t raw_id;
  uint16_t runtime_id;
  uint32_t type;
  float px, py, pz;
  float vx, vy, vz;
  float pitch, yaw, roll;
  float health, max_health;
  int32_t alive;
  int32_t on_ground;
  const char *name;
  uint64_t last_seen_tick;
} BerryEntitySnapshotC;

/// Read-only level time. `day` is 0/1.
typedef struct BerryLevelTimeC {
  int64_t absolute;
  float daylight;
  int32_t day;
  float rain_level;
  float thunder_level;
} BerryLevelTimeC;

/// The local player's own state. `valid` is 0 until the player is bound; when it
/// is 0 the adapter reports the same default the `LevelSeam` method does.
typedef struct BerryLocalPlayerC {
  int32_t valid;
  int32_t hunger;
  float saturation;
  int32_t armor_points;
  int32_t gamemode;
  int32_t dimension;
  int32_t xp_level;
  float xp_progress;
  float air;
  int32_t on_fire;
  int32_t sneaking;
  int32_t sprinting;
  /// The local player's world position and view rotation (pitch, yaw, roll). A host that can
  /// read only the local player (a launcher's preloader) fills these; `valid` gates them, so a
  /// zero reading is "no pose", not the origin.
  float px, py, pz;
  float pitch, yaw, roll;
} BerryLocalPlayerC;

/// The seam ABI version. Independent of the global `BERRY_ABI_VERSION`: the
/// seam struct may gain a slot without moving the engine config ABI, and the
/// engine config may grow without moving the seam. Bump whenever `BerrySeamC`
/// changes layout or a slot's signature changes.
#define BERRY_SEAM_ABI_VERSION 8

/// The seam a host fills and binds.
///
/// Every function pointer is optional: a null one makes the matching read fail
/// closed. The list functions fill up to `cap` ids into `ids`, write the number
/// written to `*count`, and return non-zero on success; a zero return or a null
/// `ids`/`count` reads as "no entities". `user` is the host's own context,
/// passed back unchanged to every callback.
typedef struct BerrySeamC {
  /// Must equal `sizeof(BerrySeamC)`.
  uint32_t size;
  /// Must equal `BERRY_SEAM_ABI_VERSION`.
  uint32_t abi_version;
  /// Host context, passed back to every callback. May be null.
  void *user;

  /// Snapshot an entity by runtime id. Returns non-zero and fills `out` on hit.
  int32_t (*get_entity)(uint64_t runtime_id, BerryEntitySnapshotC *out, void *user);
  /// Every entity id the seam knows. Fills up to `cap` into `ids`.
  int32_t (*all_entities)(uint64_t *ids, size_t cap, size_t *count, void *user);
  /// Entity ids within `radius` of (x, y, z).
  int32_t (*entities_in_radius)(float x, float y, float z, float radius,
                                uint64_t *ids, size_t cap, size_t *count, void *user);
  /// Entity ids within an axis-aligned box. `type_filter` is a
  /// `Berry::EntityType` value, or 5 (Unknown) to match every type.
  int32_t (*entities_in_aabb)(float min_x, float min_y, float min_z,
                              float max_x, float max_y, float max_z,
                              uint32_t type_filter, uint64_t *ids, size_t cap,
                              size_t *count, void *user);
  /// Raycast against entities. Returns non-zero and fills the hit on success.
  int32_t (*raycast_entity)(float ox, float oy, float oz, float dx, float dy,
                            float dz, float max_dist, uint64_t *out_id,
                            float *out_dist, void *user);
  /// Raycast against blocks. Returns non-zero and fills the hit on success.
  int32_t (*raycast_block)(float ox, float oy, float oz, float dx, float dy,
                           float dz, float max_dist, float *out_x, float *out_y,
                           float *out_z, uint32_t *out_block, void *user);
  /// Block id at integer coordinates (0 = air).
  uint32_t (*get_block)(int32_t x, int32_t y, int32_t z, void *user);
  /// Read-only level time. Returns non-zero and fills `out` on success.
  int32_t (*get_time)(BerryLevelTimeC *out, void *user);
  /// Biome name at a block coordinate. Writes a NUL-terminated string into
  /// `out` (up to `cap` bytes) and returns non-zero on success; a null/empty
  /// answer leaves the read as "" (unknown).
  int32_t (*get_biome)(int32_t x, int32_t y, int32_t z, char *out, size_t cap,
                       void *user);
  /// Light level 0..15 at a block coordinate, or a negative value when unknown.
  int32_t (*get_light)(int32_t x, int32_t y, int32_t z, void *user);
  /// Chunk load state at a block coordinate (0 Unloaded, 1 Loaded, 2 Rendered).
  int32_t (*get_chunk_state)(int32_t x, int32_t y, int32_t z, void *user);
  /// The local player's own state. Returns non-zero and fills `out` on success.
  int32_t (*get_local_player)(BerryLocalPlayerC *out, void *user);
} BerrySeamC;

/// Binds a world seam. Copies the struct (it does not retain the pointer), so the
/// caller may free its copy after the call.
///
/// Returns 1 when the seam was bound, 0 when it was refused: a null pointer, a
/// `size` smaller than `sizeof(BerrySeamC)`, an `abi_version` other than this
/// build's, or BerryAPI not yet attached (there is no World module to bind to).
/// On refusal nothing is changed and the previous seam, if any, stays bound.
BERRY_API int32_t BERRY_SetWorldSeam(const BerrySeamC *seam);

/// Unbinds the current seam, if any. Every world/entity read then answers its
/// fail-closed default, exactly as before a seam was ever bound.
BERRY_API void BERRY_ClearWorldSeam(void);

/// 1 when a seam is currently bound, else 0.
BERRY_API int32_t BERRY_HasWorldSeam(void);

/// The exact `sizeof(BerrySeamC)` this build expects.
///
/// A host that mirrors the struct (a launcher's preloader, which `dlopen`s
/// BerryAPI and so cannot include this header) reads this before binding and
/// refuses to bind when its own `sizeof` differs. That is what makes the mirror
/// safe: a layout drift is caught by a number, not by a misread function pointer.
BERRY_API uint32_t BERRY_WorldSeamStructSize(void);

/// The `BERRY_SEAM_ABI_VERSION` this build was compiled with, for the same check.
BERRY_API uint32_t BERRY_WorldSeamAbiVersion(void);

#ifdef __cplusplus
}  // extern "C"
#endif
