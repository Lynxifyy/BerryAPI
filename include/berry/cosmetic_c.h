#pragma once
// Cosmetic C ABI: lets a host that is not linked against BerryAPI's C++ ABI — a
// launcher's native preloader, which `dlopen`s `libBerryAPI.so` and talks to it
// through the small C surface — drive the local player's cosmetics and install
// the player-model hook that makes bones resolve.
//
// Until now the cosmetics system (`berry/cosmetic.h`, `berry/cosmetic_device.h`)
// was C++ only, so a host could not reach it without linking the whole C++
// surface. This header closes that gap the same way `berry/seam.h` closed it for
// the world seam and `berry/replay_c.h` for the replay: a plain C struct of
// function pointers, obtained by calling `BERRY_GetCosmeticApi()`.
//
// Two halves:
//   * the LOADOUT half — reload packs, list the catalog, equip/unequip, presets
//     and stats, all over the local player;
//   * the BONE half — `set_player_model_hook` installs a detour on the game's
//     player-render function and binds a host-supplied bone reader, which is
//     what makes a cosmetic resolve to a real rig. Without it the render path
//     fails closed and draws nothing (the correct default for a build whose
//     layout is unknown).
//
// Fail-closed throughout:
//   * `BERRY_GetCosmeticApi` returns null before attach, so a host cannot drive
//     cosmetics on an engine that is not running.
//   * The struct carries `size`/`abi_version`; the host checks both against
//     `BERRY_CosmeticAbiVersion()`/`BERRY_CosmeticStructSize()` before it calls
//     a single pointer, so a layout drift is caught by a number.
//   * Every call that can fail returns a non-zero `BerryCosmeticStatus` and
//     writes nothing on failure, so a refused equip leaves the loadout intact.

#include "berry/export.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The cosmetic C ABI version. Independent of the global `BERRY_ABI_VERSION`:
/// the surface may gain entry points without moving the engine config ABI.
/// Bump whenever a struct here changes layout or a pointer's signature changes.
#define BERRY_COSMETIC_ABI_VERSION 1

/// The longest asset id a record carries, including the NUL. A longer id is
/// truncated in `list` and refused by `equip`, so a hostile pack cannot inflate
/// a record.
#define BERRY_COSMETIC_MAX_ID 128
/// The longest custom-animation clip name, matching `CosmeticAsset::kMaxAnimationNameLen`.
#define BERRY_COSMETIC_MAX_ANIMATION 64

/// Status codes. 0 is success; a caller tests `!= 0` for failure and may map the
/// value to a message with `BERRY_CosmeticStatusName`.
typedef enum BerryCosmeticStatus {
  BERRY_COSMETIC_OK = 0,
  /// BerryAPI is not attached, so there is no cosmetic service to drive.
  BERRY_COSMETIC_NOT_ATTACHED = 1,
  /// A required argument was null, empty or out of range.
  BERRY_COSMETIC_BAD_ARGUMENT = 2,
  /// The asset id is not in the registry.
  BERRY_COSMETIC_UNKNOWN_ASSET = 3,
  /// The local player does not own the asset (remote loadouts only).
  BERRY_COSMETIC_NOT_OWNED = 4,
  /// The slot value is not a known `BerryCosmeticSlotC`.
  BERRY_COSMETIC_BAD_SLOT = 5,
  /// A canonical loadout / preset text was malformed; nothing was changed.
  BERRY_COSMETIC_BAD_FORMAT = 6,
  /// The named preset does not exist.
  BERRY_COSMETIC_UNKNOWN_PRESET = 7,
  /// A buffer the caller supplied was too small for the value.
  BERRY_COSMETIC_BUFFER_TOO_SMALL = 8,
  /// No usable hook backend, so the player-model hook cannot be installed.
  BERRY_COSMETIC_NO_HOOK_BACKEND = 9,
  /// The hook backend refused the detour (bad arch, or the target is unhookable).
  BERRY_COSMETIC_HOOK_REFUSED = 10,
  /// A player-model hook is already installed; clear it first.
  BERRY_COSMETIC_ALREADY_HOOKED = 11,
  /// The bone reader struct was null, too small, or ABI-mismatched.
  BERRY_COSMETIC_BAD_READER = 12,
} BerryCosmeticStatus;

/// Mirrors `Berry::CosmeticSlot`. `BERRY_COSMETIC_SLOT_NONE` is 0; the values are
/// the wire/ABI contract and must not be reordered.
typedef enum BerryCosmeticSlotC {
  BERRY_COSMETIC_SLOT_NONE = 0,
  BERRY_COSMETIC_SLOT_HEAD = 1,
  BERRY_COSMETIC_SLOT_BACK = 2,
  BERRY_COSMETIC_SLOT_SHOULDERS = 3,
  BERRY_COSMETIC_SLOT_BODY = 4,
  BERRY_COSMETIC_SLOT_TRAIL = 5,
  BERRY_COSMETIC_SLOT_EFFECT = 6,
  BERRY_COSMETIC_SLOT_EMOTE = 7,
  BERRY_COSMETIC_SLOT_COUNT = 8,
} BerryCosmeticSlotC;

/// One registry asset, flattened so no C++ type crosses the boundary. String
/// fields are fixed-size and NUL-terminated; a value too long is truncated.
typedef struct BerryCosmeticAssetC {
  char id[BERRY_COSMETIC_MAX_ID];
  int32_t slot;       // BerryCosmeticSlotC
  int32_t bone;       // Berry::Bone value
  uint64_t geometry;  // loaded mesh handle
  uint64_t texture;   // loaded texture handle
  float offset_x, offset_y, offset_z;
  float rotation_pitch, rotation_yaw, rotation_roll;
  float scale;
  float tint_r, tint_g, tint_b;
  float trail;        // 0 rigid .. 1 fully slack
  char animation[BERRY_COSMETIC_MAX_ANIMATION];
  float animation_speed;
  char pack[BERRY_COSMETIC_MAX_ID];
  char author[BERRY_COSMETIC_MAX_ID];
} BerryCosmeticAssetC;

/// The cosmetic service's counters, flattened.
typedef struct BerryCosmeticStatsC {
  uint64_t frames;
  uint64_t submitted;       // geometry submissions
  uint64_t skipped_no_bone;
  uint64_t skipped_not_owned;
  uint64_t cache_hits;
  uint64_t cache_misses;
  uint32_t registry_count;  // assets in the catalog
  uint32_t preset_count;    // wardrobe presets
} BerryCosmeticStatsC;

/// A bone's world transform, as the host's bone reader reports it. Mirrors
/// `Berry::CosmeticBoneTransform`.
typedef struct BerryCosmeticBoneTransformC {
  float px, py, pz;         // world position of the bone origin
  float pitch, yaw, roll;   // degrees
  float vx, vy, vz;         // owning entity velocity (blocks/s), for trailing sway
  float scale;
  int32_t valid;            // 1 when the reading is usable
} BerryCosmeticBoneTransformC;

/// The per-build bone accessors a host fills. Both callbacks are optional: a
/// null `bone_transform` makes the source resolve nothing (fail closed), and a
/// null `pose_revision` disables the resolve cache (correctness over speed).
///
/// `raw_id`/`runtime_id` identify the entity the game is rendering; `bone` is a
/// `Berry::Bone` value. The callback returns non-zero and fills `out` on a hit.
typedef struct BerryPlayerModelBoneReaderC {
  /// Must equal `sizeof(BerryPlayerModelBoneReaderC)`.
  uint32_t size;
  /// Must equal `BERRY_COSMETIC_ABI_VERSION`.
  uint32_t abi_version;
  /// Host context, passed back to every callback unchanged. May be null.
  void* user;
  int32_t (*bone_transform)(uint64_t raw_id, uint16_t runtime_id, int32_t bone,
                            BerryCosmeticBoneTransformC* out, void* user);
  int32_t (*pose_revision)(uint64_t raw_id, uint16_t runtime_id, uint64_t* out,
                           void* user);
} BerryPlayerModelBoneReaderC;

/// The cosmetic C surface. Every pointer is optional: a null one means this
/// build does not provide that call, and the host must not invoke it. The host
/// checks `size` and `abi_version` before using any pointer.
typedef struct BerryCosmeticApi {
  /// Must equal `sizeof(BerryCosmeticApi)`.
  uint32_t size;
  /// Must equal `BERRY_COSMETIC_ABI_VERSION`.
  uint32_t abi_version;

  //--- catalog + packs ------------------------------------------------------

  /// Reloads model packs from `pack_dir` (`*.pack.json`) and asset data from
  /// `asset_dir` (`*.mesh.json` / `*.tex.json`), replacing what is loaded. A
  /// null/empty directory leaves that half untouched. Returns the number of
  /// assets registered, or 0 on a refusal. Safe to call repeatedly.
  uint32_t (*reload_packs)(const char* pack_dir, const char* asset_dir);

  /// Writes up to `cap` catalog records into `out`, writing the count to
  /// `*count`. Returns `BERRY_COSMETIC_OK` even when the catalog is empty.
  int32_t (*list)(BerryCosmeticAssetC* out, size_t cap, size_t* count);

  /// The number of registered assets.
  uint32_t (*registry_count)(void);

  //--- local loadout --------------------------------------------------------

  /// Equips the asset with `asset_id` on the local player, in the slot the
  /// registry records for it. A second equip in the same slot replaces the
  /// first. Refuses an unknown asset.
  int32_t (*equip)(const char* asset_id);
  /// Removes whatever is in `slot`. Refuses an unknown slot.
  int32_t (*unequip)(int32_t slot);
  /// Writes the asset id equipped in `slot` into `out` (a NUL-terminated string,
  /// "" when the slot is empty). Refuses an unknown slot or a short buffer.
  int32_t (*equipped)(int32_t slot, char* out, size_t cap);
  /// Empties the local loadout.
  int32_t (*clear_loadout)(void);

  /// Writes the local loadout's canonical text into `out` (NUL-terminated) and
  /// its length into `*out_len` (excluding the NUL). Refuses a short buffer.
  int32_t (*loadout_canonical)(char* out, size_t cap, size_t* out_len);
  /// Parses `text` back into the local loadout. Fail closed: a malformed text
  /// leaves the loadout untouched.
  int32_t (*set_loadout_canonical)(const char* text);

  //--- wardrobe (presets) ---------------------------------------------------

  /// Saves the current local loadout as preset `name` (replacing one of the
  /// same name).
  int32_t (*save_preset)(const char* name);
  /// Replaces the local loadout with preset `name`.
  int32_t (*apply_preset)(const char* name);
  /// 1 when preset `name` exists.
  int32_t (*has_preset)(const char* name);
  /// Writes up to `cap` preset names into `names` (each a NUL-terminated string
  /// of at most `name_cap` bytes) and the count into `*count`.
  int32_t (*preset_names)(char* names, size_t name_cap, size_t cap, size_t* count);

  //--- remote render gate ---------------------------------------------------

  /// When 1 (the default), only the local player renders. When 0, verified
  /// remote loadouts (fed by the sync channel) render too.
  int32_t (*set_local_only)(int32_t local_only);
  /// The current local-only flag (1 or 0).
  int32_t (*local_only)(void);

  //--- stats ----------------------------------------------------------------

  /// Writes the service counters into `out`. Never fails.
  void (*stats)(BerryCosmeticStatsC* out);

  //--- player-model bone plumbing ------------------------------------------

  /// Installs the player-model hook on `target` (the game's player-render
  /// function, resolved by the host's own signature scan) and binds `reader` as
  /// the per-build bone source. This is what makes cosmetics resolve to a real
  /// rig. Refuses a null target, a bad reader, an unattached engine, no usable
  /// hook backend, a refused detour, or a hook already installed.
  int32_t (*set_player_model_hook)(void* target,
                                   const BerryPlayerModelBoneReaderC* reader);
  /// Removes the hook and unbinds the reader, so the source resolves nothing.
  /// Idempotent.
  void (*clear_player_model_hook)(void);
  /// 1 when a player-model hook is installed.
  int32_t (*has_player_model_hook)(void);
} BerryCosmeticApi;

/// Maps a status to a short message for a diagnostics line. Never null.
BERRY_API const char* BERRY_CosmeticStatusName(int32_t status);

/// The `BERRY_COSMETIC_ABI_VERSION` this build was compiled with. The host
/// compares its own expectation against this before calling any pointer.
BERRY_API uint32_t BERRY_CosmeticAbiVersion(void);

/// The exact `sizeof(BerryCosmeticApi)` this build expects, so a host that
/// mirrors the struct reads it before binding and refuses on a mismatch.
BERRY_API uint32_t BERRY_CosmeticStructSize(void);

/// The exact `sizeof(BerryPlayerModelBoneReaderC)` this build expects, for the
/// same mirror check on the reader struct.
BERRY_API uint32_t BERRY_CosmeticBoneReaderStructSize(void);

/// Returns the cosmetic C surface, or null when BerryAPI is not attached. The
/// returned pointer is stable for the process's lifetime; the host must check
/// `size`/`abi_version` before using it.
BERRY_API const BerryCosmeticApi* BERRY_GetCosmeticApi(void);

#ifdef __cplusplus
}
#endif
