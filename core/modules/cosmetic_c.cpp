// The cosmetic C ABI (berry/cosmetic_c.h). A thin adapter over CosmeticService
// and BerryClient, so a host that dlopen's libBerryAPI.so can drive the local
// player's cosmetics and install the player-model hook without linking the C++
// surface.
//
// Fail-closed: every entry point checks that the engine is attached and that
// the client (and so the cosmetic service) exists, and a failing call writes
// nothing. The returned struct is a single process-lifetime instance whose
// function pointers never change, so a host may cache it after the size/abi
// check. The one piece of process state - the installed player-model hook -
// lives on the client, guarded by the same ownership the C++ path uses.

#include "berry/cosmetic_c.h"

#include "berry/client.h"
#include "berry/engine.h"
#include "berry/hooks.h"
#include "berry/logger.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

/// The attached client, or null. Every call funnels through this, so a host can
/// never reach a service on an engine that is not running.
Berry::BerryClient* Client() {
  Berry::Engine& engine = Berry::Engine::Instance();
  if (!engine.attached()) return nullptr;
  return engine.Client();
}

Berry::CosmeticService* Cosmetics() {
  Berry::BerryClient* c = Client();
  return c != nullptr ? &c->cosmetics() : nullptr;
}

/// Copies `src` into a fixed-size buffer as a NUL-terminated string, truncating
/// (with the terminator always written) when it does not fit. Used for the
/// catalog records, where a fixed record is the contract.
template <size_t N>
void CopyFixed(char (&dst)[N], const std::string& src) {
  static_assert(N > 0, "fixed buffer must hold a terminator");
  const size_t n = src.size() < N - 1 ? src.size() : N - 1;
  std::memcpy(dst, src.data(), n);
  dst[n] = '\0';
}

/// Maps a `Berry::CosmeticError` to a status. The two enums are not identical
/// (the C status carries engine-level failures the registry cannot produce), so
/// the mapping is explicit rather than a cast.
int32_t StatusOf(Berry::CosmeticError e) {
  using E = Berry::CosmeticError;
  switch (e) {
    case E::Ok: return BERRY_COSMETIC_OK;
    case E::UnknownAsset: return BERRY_COSMETIC_UNKNOWN_ASSET;
    case E::NotOwned: return BERRY_COSMETIC_NOT_OWNED;
    case E::UnknownSlot: return BERRY_COSMETIC_BAD_SLOT;
    // The remaining registry failures are all "the value you gave me is not
    // usable", which the host reports as a bad argument.
    default: return BERRY_COSMETIC_BAD_ARGUMENT;
  }
}

//--- catalog + packs --------------------------------------------------------

uint32_t ReloadPacks(const char* pack_dir, const char* asset_dir) {
  Berry::BerryClient* c = Client();
  if (c == nullptr) return 0;
  const std::string packs = pack_dir != nullptr ? pack_dir : "";
  const std::string assets = asset_dir != nullptr ? asset_dir : "";
  return static_cast<uint32_t>(c->ReloadCosmeticPacks(packs, assets));
}

int32_t List(BerryCosmeticAssetC* out, size_t cap, size_t* count) {
  if (count == nullptr) return BERRY_COSMETIC_BAD_ARGUMENT;
  *count = 0;
  if (out == nullptr && cap != 0) return BERRY_COSMETIC_BAD_ARGUMENT;
  const std::vector<Berry::CosmeticAsset> all =
      Berry::CosmeticRegistry::Instance().All();
  const size_t n = all.size() < cap ? all.size() : cap;
  for (size_t i = 0; i < n; ++i) {
    const Berry::CosmeticAsset& a = all[i];
    BerryCosmeticAssetC& r = out[i];
    std::memset(&r, 0, sizeof(r));
    CopyFixed(r.id, a.id);
    r.slot = static_cast<int32_t>(a.slot);
    r.bone = static_cast<int32_t>(a.ResolvedBone());
    r.geometry = a.geometry;
    r.texture = a.texture;
    r.offset_x = a.offset.x;
    r.offset_y = a.offset.y;
    r.offset_z = a.offset.z;
    r.rotation_pitch = a.rotation.x;
    r.rotation_yaw = a.rotation.y;
    r.rotation_roll = a.rotation.z;
    r.scale = a.scale;
    r.tint_r = a.tint.x;
    r.tint_g = a.tint.y;
    r.tint_b = a.tint.z;
    r.trail = a.trail;
    CopyFixed(r.animation, a.animation);
    r.animation_speed = a.animationSpeed;
    CopyFixed(r.pack, a.pack);
    CopyFixed(r.author, a.author);
  }
  *count = n;
  return BERRY_COSMETIC_OK;
}

uint32_t RegistryCount(void) {
  return static_cast<uint32_t>(Berry::CosmeticRegistry::Instance().Count());
}

//--- local loadout ----------------------------------------------------------

int32_t Equip(const char* asset_id) {
  Berry::CosmeticService* svc = Cosmetics();
  if (svc == nullptr) return BERRY_COSMETIC_NOT_ATTACHED;
  if (asset_id == nullptr || asset_id[0] == '\0') return BERRY_COSMETIC_BAD_ARGUMENT;
  Berry::CosmeticError err = Berry::CosmeticError::Ok;
  if (!svc->Equip(std::string(asset_id), &err)) return StatusOf(err);
  return BERRY_COSMETIC_OK;
}

int32_t Unequip(int32_t slot) {
  Berry::CosmeticService* svc = Cosmetics();
  if (svc == nullptr) return BERRY_COSMETIC_NOT_ATTACHED;
  if (slot <= BERRY_COSMETIC_SLOT_NONE || slot >= BERRY_COSMETIC_SLOT_COUNT) {
    return BERRY_COSMETIC_BAD_SLOT;
  }
  // An empty slot is not an error: "remove what is there" is satisfied by
  // nothing being there.
  svc->Unequip(static_cast<Berry::CosmeticSlot>(slot));
  return BERRY_COSMETIC_OK;
}

int32_t Equipped(int32_t slot, char* out, size_t cap) {
  Berry::CosmeticService* svc = Cosmetics();
  if (svc == nullptr) return BERRY_COSMETIC_NOT_ATTACHED;
  if (slot <= BERRY_COSMETIC_SLOT_NONE || slot >= BERRY_COSMETIC_SLOT_COUNT) {
    return BERRY_COSMETIC_BAD_SLOT;
  }
  if (out == nullptr || cap == 0) return BERRY_COSMETIC_BAD_ARGUMENT;
  const std::string id = svc->Equipped(static_cast<Berry::CosmeticSlot>(slot));
  if (id.size() + 1 > cap) {
    out[0] = '\0';
    return BERRY_COSMETIC_BUFFER_TOO_SMALL;
  }
  std::memcpy(out, id.c_str(), id.size() + 1);
  return BERRY_COSMETIC_OK;
}

int32_t ClearLoadout(void) {
  Berry::CosmeticService* svc = Cosmetics();
  if (svc == nullptr) return BERRY_COSMETIC_NOT_ATTACHED;
  // Clearing the loadout resets the animation clock too, so a fresh loadout
  // starts its clips from zero.
  svc->ClearLocalLoadout();
  return BERRY_COSMETIC_OK;
}

int32_t LoadoutCanonical(char* out, size_t cap, size_t* out_len) {
  Berry::CosmeticService* svc = Cosmetics();
  if (svc == nullptr) return BERRY_COSMETIC_NOT_ATTACHED;
  if (out == nullptr || cap == 0) return BERRY_COSMETIC_BAD_ARGUMENT;
  const std::string text = svc->localLoadout().Canonical();
  if (text.size() + 1 > cap) {
    out[0] = '\0';
    if (out_len != nullptr) *out_len = text.size();
    return BERRY_COSMETIC_BUFFER_TOO_SMALL;
  }
  std::memcpy(out, text.c_str(), text.size() + 1);
  if (out_len != nullptr) *out_len = text.size();
  return BERRY_COSMETIC_OK;
}

int32_t SetLoadoutCanonical(const char* text) {
  Berry::CosmeticService* svc = Cosmetics();
  if (svc == nullptr) return BERRY_COSMETIC_NOT_ATTACHED;
  if (text == nullptr) return BERRY_COSMETIC_BAD_ARGUMENT;
  // Parse into a fresh loadout first: FromCanonical leaves its target untouched
  // on a malformed text, but building the loadout here keeps the service's own
  // loadout entirely out of the failure path.
  Berry::CosmeticLoadout parsed;
  if (!parsed.FromCanonical(std::string(text))) return BERRY_COSMETIC_BAD_FORMAT;
  // Apply each slot through Equip so the same registry/ownership rules a direct
  // equip obeys also gate a parsed loadout (a canonical text naming an asset
  // that is no longer registered must not smuggle it in).
  svc->Unequip(Berry::CosmeticSlot::Head);
  svc->Unequip(Berry::CosmeticSlot::Back);
  svc->Unequip(Berry::CosmeticSlot::Shoulders);
  svc->Unequip(Berry::CosmeticSlot::Body);
  svc->Unequip(Berry::CosmeticSlot::Trail);
  svc->Unequip(Berry::CosmeticSlot::Effect);
  svc->Unequip(Berry::CosmeticSlot::Emote);
  for (const auto& kv : parsed.slots()) {
    if (kv.second.empty()) continue;
    Berry::CosmeticError err = Berry::CosmeticError::Ok;
    if (!svc->Equip(kv.second, &err)) return StatusOf(err);
  }
  return BERRY_COSMETIC_OK;
}

//--- wardrobe ---------------------------------------------------------------

int32_t SavePreset(const char* name) {
  Berry::CosmeticService* svc = Cosmetics();
  if (svc == nullptr) return BERRY_COSMETIC_NOT_ATTACHED;
  if (name == nullptr || name[0] == '\0') return BERRY_COSMETIC_BAD_ARGUMENT;
  return svc->SavePreset(std::string(name)) ? BERRY_COSMETIC_OK
                                            : BERRY_COSMETIC_BAD_ARGUMENT;
}

int32_t ApplyPreset(const char* name) {
  Berry::CosmeticService* svc = Cosmetics();
  if (svc == nullptr) return BERRY_COSMETIC_NOT_ATTACHED;
  if (name == nullptr || name[0] == '\0') return BERRY_COSMETIC_BAD_ARGUMENT;
  return svc->ApplyPreset(std::string(name)) ? BERRY_COSMETIC_OK
                                             : BERRY_COSMETIC_UNKNOWN_PRESET;
}

int32_t HasPreset(const char* name) {
  Berry::CosmeticService* svc = Cosmetics();
  if (svc == nullptr) return BERRY_COSMETIC_NOT_ATTACHED;
  if (name == nullptr || name[0] == '\0') return BERRY_COSMETIC_BAD_ARGUMENT;
  return svc->HasPreset(std::string(name)) ? 1 : 0;
}

int32_t PresetNames(char* names, size_t name_cap, size_t cap, size_t* count) {
  if (count == nullptr) return BERRY_COSMETIC_BAD_ARGUMENT;
  *count = 0;
  Berry::CosmeticService* svc = Cosmetics();
  if (svc == nullptr) return BERRY_COSMETIC_NOT_ATTACHED;
  if (names == nullptr && cap != 0) return BERRY_COSMETIC_BAD_ARGUMENT;
  if (cap != 0 && name_cap == 0) return BERRY_COSMETIC_BAD_ARGUMENT;
  const std::vector<std::string> all = svc->PresetNames();
  const size_t n = all.size() < cap ? all.size() : cap;
  for (size_t i = 0; i < n; ++i) {
    char* dst = names + i * name_cap;
    if (all[i].size() + 1 > name_cap) {
      dst[0] = '\0';
      return BERRY_COSMETIC_BUFFER_TOO_SMALL;
    }
    std::memcpy(dst, all[i].c_str(), all[i].size() + 1);
  }
  *count = n;
  return BERRY_COSMETIC_OK;
}

//--- remote render gate -----------------------------------------------------

int32_t SetLocalOnly(int32_t local_only) {
  Berry::CosmeticService* svc = Cosmetics();
  if (svc == nullptr) return BERRY_COSMETIC_NOT_ATTACHED;
  svc->SetLocalOnly(local_only != 0);
  return BERRY_COSMETIC_OK;
}

int32_t LocalOnly(void) {
  Berry::CosmeticService* svc = Cosmetics();
  if (svc == nullptr) return BERRY_COSMETIC_NOT_ATTACHED;
  return svc->localOnly() ? 1 : 0;
}

//--- stats ------------------------------------------------------------------

void Stats(BerryCosmeticStatsC* out) {
  if (out == nullptr) return;
  std::memset(out, 0, sizeof(*out));
  Berry::CosmeticService* svc = Cosmetics();
  if (svc == nullptr) return;
  const Berry::CosmeticStats s = svc->stats();
  out->frames = s.frames;
  out->submitted = s.submitted;
  out->skipped_no_bone = s.skippedNoBone;
  out->skipped_not_owned = s.skippedNotOwned;
  out->cache_hits = s.cacheHits;
  out->cache_misses = s.cacheMisses;
  out->registry_count = static_cast<uint32_t>(Berry::CosmeticRegistry::Instance().Count());
  out->preset_count = static_cast<uint32_t>(svc->PresetNames().size());
}

//--- player-model bone plumbing --------------------------------------------

int32_t SetPlayerModelHook(void* target,
                           const BerryPlayerModelBoneReaderC* reader) {
  Berry::BerryClient* c = Client();
  if (c == nullptr) return BERRY_COSMETIC_NOT_ATTACHED;
  if (target == nullptr) return BERRY_COSMETIC_BAD_ARGUMENT;
  if (reader == nullptr) return BERRY_COSMETIC_BAD_READER;
  if (reader->size < sizeof(BerryPlayerModelBoneReaderC)) return BERRY_COSMETIC_BAD_READER;
  if (reader->abi_version != BERRY_COSMETIC_ABI_VERSION) return BERRY_COSMETIC_BAD_READER;
  if (reader->bone_transform == nullptr) return BERRY_COSMETIC_BAD_READER;
  if (c->hasPlayerModelHook()) return BERRY_COSMETIC_ALREADY_HOOKED;

  Berry::IHook* backend = Berry::HookManager::Instance().Backend();
  if (backend == nullptr || !backend->CanHook()) return BERRY_COSMETIC_NO_HOOK_BACKEND;

  // Wrap the C reader as the C++ reader the client binds. The user pointer and
  // the two callbacks are copied by value, so the host may free its struct
  // after this call. `pose_revision` may be null, which disables the cache.
  const BerryPlayerModelBoneReaderC r = *reader;
  Berry::PlayerModelBoneReader cpp;
  cpp.boneTransform = [r](Berry::EntityId entity,
                          Berry::Bone bone,
                          Berry::CosmeticBoneTransform* out) -> bool {
    if (out == nullptr) return false;
    BerryCosmeticBoneTransformC c{};
    if (r.bone_transform(entity.raw, entity.runtime, static_cast<int32_t>(bone), &c,
                         r.user) == 0) {
      return false;
    }
    if (c.valid == 0) return false;
    out->position = Berry::Vec3(c.px, c.py, c.pz);
    out->rotation = Berry::Vec3(c.pitch, c.yaw, c.roll);
    out->velocity = Berry::Vec3(c.vx, c.vy, c.vz);
    out->scale = c.scale;
    out->valid = true;
    return true;
  };
  if (r.pose_revision != nullptr) {
    cpp.poseRevision = [r](Berry::EntityId entity, Berry::u64* out) -> bool {
      if (out == nullptr) return false;
      return r.pose_revision(entity.raw, entity.runtime, out, r.user) != 0;
    };
  }

  if (!c->InstallPlayerModelHook(target, cpp)) {
    // The backend refused the detour (bad arch or an unhookable target), which
    // is a hook failure rather than a bad argument.
    return BERRY_COSMETIC_HOOK_REFUSED;
  }
  return BERRY_COSMETIC_OK;
}

void ClearPlayerModelHook(void) {
  if (Berry::BerryClient* c = Client()) c->ClearPlayerModelHook();
}

int32_t HasPlayerModelHook(void) {
  Berry::BerryClient* c = Client();
  return (c != nullptr && c->hasPlayerModelHook()) ? 1 : 0;
}

/// The process-lifetime surface. Its pointers are set once, so a host may cache
/// it after the size/abi check.
const BerryCosmeticApi kApi = {
    sizeof(BerryCosmeticApi),
    BERRY_COSMETIC_ABI_VERSION,
    &ReloadPacks,
    &List,
    &RegistryCount,
    &Equip,
    &Unequip,
    &Equipped,
    &ClearLoadout,
    &LoadoutCanonical,
    &SetLoadoutCanonical,
    &SavePreset,
    &ApplyPreset,
    &HasPreset,
    &PresetNames,
    &SetLocalOnly,
    &LocalOnly,
    &Stats,
    &SetPlayerModelHook,
    &ClearPlayerModelHook,
    &HasPlayerModelHook,
};

}  // namespace

extern "C" {

BERRY_API const char* BERRY_CosmeticStatusName(int32_t status) {
  switch (status) {
    case BERRY_COSMETIC_OK: return "ok";
    case BERRY_COSMETIC_NOT_ATTACHED: return "not attached";
    case BERRY_COSMETIC_BAD_ARGUMENT: return "bad argument";
    case BERRY_COSMETIC_UNKNOWN_ASSET: return "unknown asset";
    case BERRY_COSMETIC_NOT_OWNED: return "not owned";
    case BERRY_COSMETIC_BAD_SLOT: return "bad slot";
    case BERRY_COSMETIC_BAD_FORMAT: return "bad format";
    case BERRY_COSMETIC_UNKNOWN_PRESET: return "unknown preset";
    case BERRY_COSMETIC_BUFFER_TOO_SMALL: return "buffer too small";
    case BERRY_COSMETIC_NO_HOOK_BACKEND: return "no hook backend";
    case BERRY_COSMETIC_HOOK_REFUSED: return "hook refused";
    case BERRY_COSMETIC_ALREADY_HOOKED: return "already hooked";
    case BERRY_COSMETIC_BAD_READER: return "bad reader";
    default: return "unknown status";
  }
}

BERRY_API uint32_t BERRY_CosmeticAbiVersion(void) {
  return static_cast<uint32_t>(BERRY_COSMETIC_ABI_VERSION);
}

BERRY_API uint32_t BERRY_CosmeticStructSize(void) {
  return static_cast<uint32_t>(sizeof(BerryCosmeticApi));
}

BERRY_API uint32_t BERRY_CosmeticBoneReaderStructSize(void) {
  return static_cast<uint32_t>(sizeof(BerryPlayerModelBoneReaderC));
}

BERRY_API const BerryCosmeticApi* BERRY_GetCosmeticApi(void) {
  // A surface with no engine to drive is not returned: a host that got a
  // non-null pointer would otherwise call into a service that does not exist.
  if (Client() == nullptr) return nullptr;
  return &kApi;
}

}  // extern "C"
