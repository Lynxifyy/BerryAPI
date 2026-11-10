# Cosmetic C ABI (design)

Status: implemented in `include/berry/cosmetic_c.h` + `core/modules/cosmetic_c.cpp`.
ABI: `BERRY_COSMETIC_ABI_VERSION` = 1 (independent of the global `BERRY_ABI_VERSION`).

## Problem

The cosmetics system (`berry/cosmetic.h`, `berry/cosmetic_device.h`) is a C++ surface. A
host that is not linked against BerryAPI's C++ ABI — a launcher's native preloader, which
`dlopen`s `libBerryAPI.so` and talks to it through a small C surface — cannot reach it at
all: there is no `BERRY_*` entry point for a loadout, a catalog read, a pack load, or the
player-model hook. The world seam, replay and governor each closed this gap with a plain C
struct of function pointers; cosmetics is the last major service without one.

There is a second, subtler gap. Even with a C ABI, cosmetics resolve **no bones** unless the
embedder installs a player-model hook: `BerryClient::BuildCosmetics` installs it only when
`cfg.playerModelRenderTarget && cfg.playerModelBones.valid()`, and BerryAPI does not resolve
that per-build target itself (it is a signature-scan result the host owns). So the ABI must
also carry the player-model plumbing, or the surface would exist and draw nothing.

## Shape

Mirrors the replay/governor C ABIs exactly, so a host has one pattern to learn:

* `BERRY_GetCosmeticApi()` returns a pointer to a single, process-lifetime
  `BerryCosmeticApi` whose function pointers never change. Null before attach.
* The struct carries `size` and `abi_version`. The host checks both against
  `BERRY_CosmeticStructSize()` / `BERRY_CosmeticAbiVersion()` before calling a pointer, so
  a layout drift is caught by a number, never by a misread function pointer.
* Every call that can fail returns a named `BerryCosmeticStatus` and writes nothing on
  failure. `BERRY_CosmeticStatusName()` maps it to a message for a diagnostics line.

The surface (local player unless stated):

| call | purpose |
|------|---------|
| `reload_packs(pack_dir, asset_dir)` | load model packs + asset data from directories |
| `list(out, cap, count)` | catalog snapshot (fixed-size records, no allocation crosses) |
| `equip(asset_id)` / `unequip(slot)` / `equipped(slot, out, cap)` | the local loadout |
| `clear_loadout()` | empty the local loadout |
| `loadout_canonical(out, cap)` | the canonical loadout text (hashing / wire framing) |
| `set_loadout_canonical(text)` | parse a canonical loadout back, fail-closed |
| `save_preset(name)` / `apply_preset(name)` / `preset_names(out, cap, count)` | wardrobe |
| `set_local_only(v)` | local-only vs. rendering verified remote loadouts |
| `stats(out)` | frames, submissions, skips, cache hits/misses, counts |
| `set_player_model_hook(target, reader)` / `clear_player_model_hook()` / `has_player_model_hook()` | bone plumbing |

`slot` is a `BerryCosmeticSlotC` value (mirrors `CosmeticSlot`). `equip` takes an asset id
only: the registry resolves the slot, so a host cannot equip into the wrong one.

## Bone plumbing (the part that makes it draw)

```c
typedef struct BerryCosmeticBoneTransformC {
  float px, py, pz;          // world position of the bone origin
  float pitch, yaw, roll;    // degrees
  float vx, vy, vz;          // owning entity velocity (blocks/s), for trailing sway
  float scale;
  int32_t valid;
} BerryCosmeticBoneTransformC;

typedef struct BerryPlayerModelBoneReaderC {
  uint32_t size;             // must equal sizeof(BerryPlayerModelBoneReaderC)
  uint32_t abi_version;      // must equal BERRY_COSMETIC_ABI_VERSION
  void* user;                // host context, passed back unchanged
  int32_t (*bone_transform)(uint64_t raw_id, uint16_t runtime_id, int32_t bone,
                            BerryCosmeticBoneTransformC* out, void* user);
  int32_t (*pose_revision)(uint64_t raw_id, uint16_t runtime_id, uint64_t* out, void* user);
} BerryPlayerModelBoneReaderC;
```

`set_player_model_hook(target, reader)` wraps the reader in the C++ `PlayerModelBoneReader`
(a pair of `std::function`s) and installs a `PlayerModelHook` on `target` through the active
hook backend, exactly as `BerryClient::BuildCosmetics` does for the C++ path. `pose_revision`
is optional: an empty one disables the resolve cache (correctness over speed), matching the
C++ contract.

Fail-closed, in order: a null `target`, a null/short/ABI-mismatched reader, an unattached
engine, a backend that cannot hook, or a hook already installed all leave no hook behind and
return a named status. `clear_player_model_hook` uninstalls and unbinds, so the source
resolves nothing again.

## Config

`BerryConfigC` grows two borrowed-string fields, matching every other directory the struct
already carries (`script_directory`, `script_data_directory`):

```c
const char* cosmetic_pack_directory;   // *.pack.json root, or NULL
const char* cosmetic_asset_directory;  // *.mesh.json / *.tex.json root, or NULL
```

`Engine::Bootstrap` forwards both into `ClientConfig`, so packs load at attach the same way
the C++ path loads them. This is a **public config layout change**, so the global
`BERRY_ABI_VERSION` is bumped 8 → 9. The world-seam struct layout is unchanged
(`sizeof(BerrySeamC)` stays 112); only the number the seam reports moves with the global,
which is BerryAPI's existing convention (v3–v8 all bumped it for config growth).

## Threading and ownership

`CosmeticService` is already thread-safe (short map operations under a lock, submission
outside it; the script worker mutates while the render thread resolves). The C ABI adds no
new shared state: every pointer in `BerryCosmeticApi` calls straight through to a service
method. The one piece of process state is the player-model hook, guarded by the same mutex
the C++ path uses. The returned `BerryCosmeticApi` is immutable after first construction, so
a host may cache it once the size/ABI check passes.

## Why this is the right boundary

The alternative — link the C++ surface (`_ZN5Berry*` is exported) — would pin the launcher to
BerryAPI's C++ ABI, its `std::string`/`std::function` layout, and its `libc++_shared`
version. The whole point of the C ABI is that a launcher can ship a BerryAPI build it did not
compile. Keeping the boundary at plain structs of function pointers preserves that.
