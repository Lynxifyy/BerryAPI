// Host tests for the cosmetic C ABI (berry/cosmetic_c.h).
//
// Covers the contract a dlopen'ing host relies on:
//   * The ABI version, struct size and reader size are reported and
//     self-consistent with the mirrored structs.
//   * `BERRY_GetCosmeticApi` refuses before attach and returns the surface after.
//   * Every call fails closed before attach (no crash, a named status).
//   * The catalog, loadout, presets and stats round-trip on the real service.
//   * The bone reader is wrapped and the player-model hook is refused without a
//     target and installed (then cleared) with one.
#include "berry/cosmetic_c.h"

#include "berry/engine.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

// A bone reader that reports a fixed pose for every entity, so the hook path can
// be exercised headlessly. `pose_revision` bumps a counter, standing in for a
// moving pose.
struct ReaderState {
  int boneCalls = 0;
  unsigned long long revision = 0;
};

int32_t BoneTransform(uint64_t raw, uint16_t runtime, int32_t bone,
                      BerryCosmeticBoneTransformC* out, void* user) {
  (void)raw;
  (void)runtime;
  (void)bone;
  ReaderState* st = static_cast<ReaderState*>(user);
  if (st != nullptr) ++st->boneCalls;
  if (out == nullptr) return 0;
  out->px = 1.0f;
  out->py = 2.0f;
  out->pz = 3.0f;
  out->pitch = 0.0f;
  out->yaw = 90.0f;
  out->roll = 0.0f;
  out->vx = 0.0f;
  out->vy = 0.0f;
  out->vz = 0.0f;
  out->scale = 1.0f;
  out->valid = 1;
  return 1;
}

int32_t PoseRevision(uint64_t raw, uint16_t runtime, uint64_t* out, void* user) {
  (void)raw;
  (void)runtime;
  ReaderState* st = static_cast<ReaderState*>(user);
  if (out == nullptr) return 0;
  *out = st != nullptr ? ++st->revision : 0;
  return 1;
}

}  // namespace

int main() {
  // --- Before attach: the surface is refused, every call fails closed ---
  CHECK(BERRY_GetCosmeticApi() == nullptr);
  CHECK(BERRY_CosmeticAbiVersion() == BERRY_COSMETIC_ABI_VERSION);
  CHECK(BERRY_CosmeticStructSize() > 0);
  CHECK(BERRY_CosmeticBoneReaderStructSize() == sizeof(BerryPlayerModelBoneReaderC));
  CHECK(std::strcmp(BERRY_CosmeticStatusName(BERRY_COSMETIC_NOT_ATTACHED), "not attached") == 0);
  CHECK(std::strcmp(BERRY_CosmeticStatusName(BERRY_COSMETIC_OK), "ok") == 0);
  CHECK(std::strcmp(BERRY_CosmeticStatusName(999), "unknown status") == 0);

  // --- Attach, then the surface is available and self-consistent ---
  Berry::Engine& eng = Berry::Engine::Instance();
  eng.Bootstrap();
  CHECK(eng.Attach());

  const BerryCosmeticApi* api = BERRY_GetCosmeticApi();
  CHECK(api != nullptr);
  if (api == nullptr) {
    printf("no cosmetic api after attach\n");
    return 1;
  }

  // The guard a host checks before calling a single pointer.
  CHECK(api->size == BERRY_CosmeticStructSize());
  CHECK(api->abi_version == BERRY_CosmeticAbiVersion());
  CHECK(api->size == sizeof(BerryCosmeticApi));
  CHECK(api->reload_packs && api->list && api->registry_count && api->equip &&
        api->unequip && api->equipped && api->clear_loadout &&
        api->loadout_canonical && api->set_loadout_canonical && api->save_preset &&
        api->apply_preset && api->has_preset && api->preset_names &&
        api->set_local_only && api->local_only && api->stats &&
        api->set_player_model_hook && api->clear_player_model_hook &&
        api->has_player_model_hook);

  // --- The built-in catalog is present, so equip has something to target ---
  const uint32_t count = api->registry_count();
  CHECK(count > 0);
  CHECK(api->list(nullptr, 0, nullptr) == BERRY_COSMETIC_BAD_ARGUMENT);

  size_t listed = 0;
  CHECK(api->list(nullptr, 0, &listed) == BERRY_COSMETIC_OK);
  CHECK(listed == 0);
  CHECK(api->list(nullptr, 4, &listed) == BERRY_COSMETIC_BAD_ARGUMENT);

  BerryCosmeticAssetC records[64];
  CHECK(api->list(records, 64, &listed) == BERRY_COSMETIC_OK);
  CHECK(listed > 0 && listed <= 64);
  const char* firstId = records[0].id;
  CHECK(firstId[0] != '\0');

  // --- Equip / equipped / unequip round-trip ---
  CHECK(api->equip(nullptr) == BERRY_COSMETIC_BAD_ARGUMENT);
  CHECK(api->equip("") == BERRY_COSMETIC_BAD_ARGUMENT);
  CHECK(api->equip("no.such.asset") == BERRY_COSMETIC_UNKNOWN_ASSET);

  CHECK(api->equip(firstId) == BERRY_COSMETIC_OK);
  char buf[BERRY_COSMETIC_MAX_ID];
  CHECK(api->equipped(records[0].slot, buf, sizeof(buf)) == BERRY_COSMETIC_OK);
  CHECK(std::strcmp(buf, firstId) == 0);

  // A short buffer is refused and reports too-small, not a truncation.
  char tiny[2];
  CHECK(api->equipped(records[0].slot, tiny, sizeof(tiny)) == BERRY_COSMETIC_BUFFER_TOO_SMALL);

  // A bad slot is refused.
  CHECK(api->equipped(0, buf, sizeof(buf)) == BERRY_COSMETIC_BAD_SLOT);
  CHECK(api->unequip(999) == BERRY_COSMETIC_BAD_SLOT);

  // Canonical round-trip: the text names what is equipped.
  char canonical[512];
  size_t clen = 0;
  CHECK(api->loadout_canonical(canonical, sizeof(canonical), &clen) == BERRY_COSMETIC_OK);
  CHECK(clen > 0);
  CHECK(std::strstr(canonical, firstId) != nullptr);

  CHECK(api->clear_loadout() == BERRY_COSMETIC_OK);
  CHECK(api->equipped(records[0].slot, buf, sizeof(buf)) == BERRY_COSMETIC_OK);
  CHECK(buf[0] == '\0');

  // A canonical loadout round-trips back to the same equip.
  CHECK(api->set_loadout_canonical(canonical) == BERRY_COSMETIC_OK);
  CHECK(api->equipped(records[0].slot, buf, sizeof(buf)) == BERRY_COSMETIC_OK);
  CHECK(std::strcmp(buf, firstId) == 0);

  // A malformed canonical text is refused, and leaves the loadout intact.
  CHECK(api->set_loadout_canonical("this is not canonical") == BERRY_COSMETIC_BAD_FORMAT);
  CHECK(api->equipped(records[0].slot, buf, sizeof(buf)) == BERRY_COSMETIC_OK);
  CHECK(std::strcmp(buf, firstId) == 0);

  // --- Presets ---
  CHECK(api->save_preset(nullptr) == BERRY_COSMETIC_BAD_ARGUMENT);
  CHECK(api->save_preset("pvp") == BERRY_COSMETIC_OK);
  CHECK(api->has_preset("pvp") == 1);
  CHECK(api->has_preset("nope") == 0);
  CHECK(api->apply_preset("nope") == BERRY_COSMETIC_UNKNOWN_PRESET);

  char names[8 * 64];
  size_t ncount = 0;
  CHECK(api->preset_names(names, 64, 8, &ncount) == BERRY_COSMETIC_OK);
  CHECK(ncount >= 1);
  CHECK(std::strcmp(names, "pvp") == 0);

  api->clear_loadout();
  CHECK(api->apply_preset("pvp") == BERRY_COSMETIC_OK);
  CHECK(api->equipped(records[0].slot, buf, sizeof(buf)) == BERRY_COSMETIC_OK);
  CHECK(std::strcmp(buf, firstId) == 0);

  // --- The local-only render gate round-trips ---
  CHECK(api->local_only() == 1);
  CHECK(api->set_local_only(0) == BERRY_COSMETIC_OK);
  CHECK(api->local_only() == 0);
  CHECK(api->set_local_only(1) == BERRY_COSMETIC_OK);
  CHECK(api->local_only() == 1);

  // --- Stats reflect the catalog and presets ---
  BerryCosmeticStatsC stats;
  api->stats(&stats);
  CHECK(stats.registry_count == count);
  CHECK(stats.preset_count >= 1);

  // --- The player-model hook: refused without a target, installed with one ---
  CHECK(api->has_player_model_hook() == 0);
  CHECK(api->set_player_model_hook(nullptr, nullptr) == BERRY_COSMETIC_BAD_ARGUMENT);

  ReaderState readerState;
  BerryPlayerModelBoneReaderC reader{};
  reader.size = sizeof(BerryPlayerModelBoneReaderC);
  reader.abi_version = BERRY_COSMETIC_ABI_VERSION;
  reader.user = &readerState;
  reader.bone_transform = &BoneTransform;
  reader.pose_revision = &PoseRevision;

  // A null reader, a short reader and an ABI-mismatched reader are refused.
  CHECK(api->set_player_model_hook(&readerState, nullptr) == BERRY_COSMETIC_BAD_READER);
  BerryPlayerModelBoneReaderC shortReader = reader;
  shortReader.size = 4;
  CHECK(api->set_player_model_hook(&readerState, &shortReader) == BERRY_COSMETIC_BAD_READER);
  BerryPlayerModelBoneReaderC badAbi = reader;
  badAbi.abi_version = 999;
  CHECK(api->set_player_model_hook(&readerState, &badAbi) == BERRY_COSMETIC_BAD_READER);
  BerryPlayerModelBoneReaderC noFn = reader;
  noFn.bone_transform = nullptr;
  CHECK(api->set_player_model_hook(&readerState, &noFn) == BERRY_COSMETIC_BAD_READER);

  // With a target, the hook installs (the host harness backend can hook) and
  // reports itself installed; a second install is refused until cleared.
  int hookTarget = 0;
  const int32_t hookStatus = api->set_player_model_hook(&hookTarget, &reader);
  if (hookStatus == BERRY_COSMETIC_OK) {
    CHECK(api->has_player_model_hook() == 1);
    CHECK(api->set_player_model_hook(&hookTarget, &reader) == BERRY_COSMETIC_ALREADY_HOOKED);
    api->clear_player_model_hook();
    CHECK(api->has_player_model_hook() == 0);
  } else {
    // No usable hook backend on this host: the refusal is named, not a crash.
    printf("note: hook backend unavailable (%s)\n",
           BERRY_CosmeticStatusName(hookStatus));
    CHECK(hookStatus == BERRY_COSMETIC_NO_HOOK_BACKEND ||
          hookStatus == BERRY_COSMETIC_HOOK_REFUSED);
  }

  // --- Detach: the surface is refused again ---
  eng.Detach();
  CHECK(BERRY_GetCosmeticApi() == nullptr);

  if (g_fail == 0) {
    printf("test_cosmetic_c: all checks passed\n");
    return 0;
  }
  printf("test_cosmetic_c: %d failure(s)\n", g_fail);
  return 1;
}
