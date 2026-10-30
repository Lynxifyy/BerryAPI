// Host tests for cosmetics: registry, loadouts, the local render path and the
// signed sync channel.
//
// Covers:
//   * the registry's fail-closed rules (empty/duplicate id, unknown slot,
//     non-finite transform, non-positive scale) and the built-in catalog,
//   * one-asset-per-slot exclusivity and the canonical loadout round trip,
//   * wardrobe presets,
//   * attach composition against hand-computed bone math,
//   * the local present path, the no-seam and no-bone cases,
//   * remote loadouts gated on ownership and on local-only mode,
//   * the sync channel: a valid signed loadout applies, and a tampered,
//     forged, mis-keyed, mis-algorithm or malformed message is dropped,
//   * the cosmetic.* script bindings: installed, capability-gated, arity-safe.
#include "berry/api_bindings.h"
#include "berry/cosmetic.h"
#include "berry/cosmetic_sync.h"
#include "berry/lan_discovery.h"
#include "berry/generated_bindings.h"
#include "berry/manifest.h"
#include "berry/script.h"
#include "shim_level.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static bool Near(f32 a, f32 b, f32 eps = 1e-3f) {
  return std::fabs(a - b) <= eps;
}

namespace {

/// A registry with only the built-ins, so each test starts from a known state.
void FreshRegistry() {
  CosmeticRegistry::Instance().Clear();
  CosmeticRegistry::Instance().RegisterBuiltins();
}

std::string SeedKey(std::string* pub, std::string* sec, char fill = 'S') {
  const std::string seed(32, fill);
  crypto::Ed25519KeypairFromSeed(seed, pub, sec);
  return seed;
}

CapabilitySet Grant(const std::vector<std::string>& caps) {
  CapabilitySet s;
  for (const auto& c : caps) s.Add(c);
  return s;
}

ScriptStatus CallBinding(ScriptBindings& bindings, const std::string& name,
                         const std::vector<ScriptValue>& args,
                         const std::vector<std::string>& grants,
                         ScriptValue* out) {
  ScriptHost host("cosmetic-test", Grant(grants), nullptr);
  host.SetBindings(&bindings);
  return host.CallBinding(name, args, out);
}

}  // namespace

//----------------------------------------------------------------------------
// Registry
//----------------------------------------------------------------------------

static void TestRegistryFailClosed() {
  CosmeticRegistry::Instance().Clear();
  CosmeticError err = CosmeticError::Ok;

  CosmeticAsset ok;
  ok.id = "t.hat";
  ok.slot = CosmeticSlot::Head;
  CHECK(CosmeticRegistry::Instance().Register(ok, &err));
  CHECK(err == CosmeticError::Ok);

  // Duplicate id.
  CHECK(!CosmeticRegistry::Instance().Register(ok, &err));
  CHECK(err == CosmeticError::DuplicateId);

  // Empty id.
  CosmeticAsset noId;
  noId.slot = CosmeticSlot::Back;
  CHECK(!CosmeticRegistry::Instance().Register(noId, &err));
  CHECK(err == CosmeticError::EmptyId);

  // Unknown slot.
  CosmeticAsset noSlot;
  noSlot.id = "t.noslot";
  CHECK(!CosmeticRegistry::Instance().Register(noSlot, &err));
  CHECK(err == CosmeticError::UnknownSlot);

  // Non-finite transform.
  CosmeticAsset nan;
  nan.id = "t.nan";
  nan.slot = CosmeticSlot::Back;
  nan.offset = Vec3(std::nanf(""), 0, 0);
  CHECK(!CosmeticRegistry::Instance().Register(nan, &err));
  CHECK(err == CosmeticError::BadTransform);

  // Non-positive scale.
  CosmeticAsset zeroScale;
  zeroScale.id = "t.zero";
  zeroScale.slot = CosmeticSlot::Back;
  zeroScale.scale = 0.0f;
  CHECK(!CosmeticRegistry::Instance().Register(zeroScale, &err));
  CHECK(err == CosmeticError::BadScale);

  // Only the one good asset registered.
  CHECK(CosmeticRegistry::Instance().Count() == 1);
}

static void TestBuiltins() {
  CosmeticRegistry::Instance().Clear();
  const size_t added = CosmeticRegistry::Instance().RegisterBuiltins();
  CHECK(added == 4);
  CHECK(CosmeticRegistry::Instance().Count() == 4);
  // Idempotent.
  CHECK(CosmeticRegistry::Instance().RegisterBuiltins() == 0);
  CHECK(CosmeticRegistry::Instance().Count() == 4);

  // Each builtin resolves and lands in the right slot.
  CosmeticAsset cape;
  CHECK(CosmeticRegistry::Instance().Find("builtin.cape.red", &cape));
  CHECK(cape.slot == CosmeticSlot::Back);
  CHECK(cape.ResolvedBone() == Bone::Back);
  CHECK(CosmeticRegistry::Instance().ForSlot(CosmeticSlot::Back).size() == 1);
  CHECK(CosmeticRegistry::Instance().ForSlot(CosmeticSlot::Head).size() == 1);
}

//----------------------------------------------------------------------------
// Loadout / wardrobe
//----------------------------------------------------------------------------

static void TestLoadoutExclusivity() {
  FreshRegistry();
  CosmeticLoadout lo;
  CosmeticError err = CosmeticError::Ok;

  CHECK(lo.Equip("builtin.cape.red", &err));
  CHECK(lo.Equipped(CosmeticSlot::Back) == "builtin.cape.red");
  CHECK(lo.IsEquipped("builtin.cape.red"));

  // Unknown asset refused.
  CHECK(!lo.Equip("does.not.exist", &err));
  CHECK(err == CosmeticError::UnknownAsset);

  // A second asset in the same slot replaces the first.
  CosmeticAsset second;
  second.id = "t.cape.blue";
  second.slot = CosmeticSlot::Back;
  CHECK(CosmeticRegistry::Instance().Register(second, &err));
  CHECK(lo.Equip("t.cape.blue", &err));
  CHECK(lo.Equipped(CosmeticSlot::Back) == "t.cape.blue");
  CHECK(!lo.IsEquipped("builtin.cape.red"));
  CHECK(lo.Count() == 1);

  // Unequip.
  CHECK(lo.Unequip(CosmeticSlot::Back));
  CHECK(lo.Equipped(CosmeticSlot::Back).empty());
  CHECK(!lo.Unequip(CosmeticSlot::Back));   // already empty

  // Resolve skips nothing here.
  lo.Equip("builtin.hat.top");
  CHECK(lo.Resolved().size() == 1);
}

static void TestLoadoutCanonicalRoundTrip() {
  FreshRegistry();
  CosmeticLoadout lo;
  lo.Equip("builtin.cape.red");
  lo.Equip("builtin.hat.top");
  lo.Equip("builtin.wings.angel");

  const std::string canon = lo.Canonical();
  // Slot order is deterministic (kind order), regardless of equip order.
  CosmeticLoadout reordered;
  reordered.Equip("builtin.wings.angel");
  reordered.Equip("builtin.hat.top");
  reordered.Equip("builtin.cape.red");
  CHECK(reordered.Canonical() == canon);

  CosmeticLoadout back;
  CHECK(back.FromCanonical(canon));
  CHECK(back.Canonical() == canon);
  CHECK(back.Count() == 3);

  // Malformed canonical forms fail closed and leave the loadout untouched.
  CosmeticLoadout keep;
  keep.Equip("builtin.cape.red");
  const std::string before = keep.Canonical();
  CHECK(!keep.FromCanonical("no-separator-here\n"));
  CHECK(!keep.FromCanonical("bogus_slot=builtin.cape.red\n"));
  CHECK(!keep.FromCanonical("back=\n"));
  CHECK(keep.Canonical() == before);
}

static void TestWardrobe() {
  FreshRegistry();
  CosmeticWardrobe w;
  CosmeticLoadout pvp;
  pvp.Equip("builtin.cape.red");
  CHECK(w.Save("pvp", pvp));
  CHECK(w.Has("pvp"));
  CHECK(w.Names().size() == 1);

  CosmeticLoadout got;
  CHECK(w.Load("pvp", &got));
  CHECK(got.Equipped(CosmeticSlot::Back) == "builtin.cape.red");

  // Overwrite.
  CosmeticLoadout chill;
  chill.Equip("builtin.hat.top");
  CHECK(w.Save("pvp", chill));
  CHECK(w.Load("pvp", &got));
  CHECK(got.Equipped(CosmeticSlot::Head) == "builtin.hat.top");

  CHECK(w.Remove("pvp"));
  CHECK(!w.Load("pvp", &got));
  CHECK(!w.Remove("pvp"));
  // Empty name refused.
  CHECK(!w.Save("", chill));
}

//----------------------------------------------------------------------------
// Attach math
//----------------------------------------------------------------------------

static void TestComposeAttach() {
  // A bone at (10,64,5) yawed 90 degrees; a cape offset (0,0,-0.1).
  CosmeticAsset cape;
  cape.id = "t.cape";
  cape.slot = CosmeticSlot::Back;
  cape.offset = Vec3(0, 0, -0.1f);
  cape.rotation = Vec3(0, 0, 0);

  CosmeticBoneTransform bone;
  bone.position = Vec3(10, 64, 5);
  bone.rotation = Vec3(0, 90, 0);
  bone.scale = 1.0f;
  bone.valid = true;

  const CosmeticDrawItem item = ComposeAttach(cape, bone, EntityId{7, 1});
  CHECK(item.assetId == "t.cape");
  CHECK(item.bone == Bone::Back);
  CHECK(item.entity.raw == 7);
  // yaw 90 rotates (0,0,-0.1) to (-0.1, 0, 0).
  CHECK(Near(item.position.x, 9.9f));
  CHECK(Near(item.position.y, 64.0f));
  CHECK(Near(item.position.z, 5.0f));
  CHECK(Near(item.rotation.y, 90.0f));

  // Scale multiplies the offset and the resulting scale.
  CosmeticAsset big = cape;
  big.scale = 2.0f;
  CosmeticBoneTransform smallBone = bone;
  smallBone.scale = 0.5f;
  const CosmeticDrawItem s = ComposeAttach(big, smallBone, EntityId{});
  CHECK(Near(s.scale, 1.0f));
  // offset scaled by bone scale (0.5) then rotated: (-0.05, 0, 0).
  CHECK(Near(s.position.x, 9.95f));
}

// Trailing sway: a slack asset leans back and lags opposite its motion, in
// proportion to speed and to its trail factor. A rigid asset, a still player
// and an untrustworthy (non-finite) velocity all leave the pose untouched.
static void TestTrailingPose() {
  const f32 full = CosmeticTrailFullSpeed();
  const f32 lean = CosmeticTrailMaxLeanDeg();
  const f32 lag = CosmeticTrailMaxLag();

  // Rigid asset: never moves, whatever the speed.
  {
    CosmeticTrailPose p = CosmeticTrailingPose(0.0f, Vec3(10, 0, 0), Vec3());
    CHECK(Near(p.offset.x, 0.0f) && Near(p.offset.z, 0.0f));
    CHECK(Near(p.rotation.x, 0.0f));
  }
  // Stationary player: no sway even for a fully slack asset.
  {
    CosmeticTrailPose p = CosmeticTrailingPose(1.0f, Vec3(0, 0, 0), Vec3());
    CHECK(Near(p.offset.x, 0.0f) && Near(p.offset.z, 0.0f));
    CHECK(Near(p.rotation.x, 0.0f));
  }
  // Non-finite velocity is refused (fail closed, no sway).
  {
    const f32 nan = std::nanf("");
    CosmeticTrailPose p = CosmeticTrailingPose(1.0f, Vec3(nan, 0, 0), Vec3());
    CHECK(Near(p.offset.x, 0.0f));
    CHECK(Near(p.rotation.x, 0.0f));
  }
  // Half trail at full speed: half the maximum lean, lag opposite +x motion.
  {
    CosmeticTrailPose p = CosmeticTrailingPose(0.5f, Vec3(full, 0, 0), Vec3());
    CHECK(Near(p.rotation.x, lean * 0.5f));
    CHECK(Near(p.offset.x, -lag * 0.5f));   // opposite the motion
    CHECK(Near(p.offset.z, 0.0f));
    CHECK(Near(p.velocity.x, full));
  }
  // Full trail, exactly at full speed: the maxima, and the lag is a unit
  // vector in -x scaled by the max lag.
  {
    CosmeticTrailPose p = CosmeticTrailingPose(1.0f, Vec3(full, 0, 0), Vec3());
    CHECK(Near(p.rotation.x, lean));
    CHECK(Near(p.offset.x, -lag));
  }
  // Above full speed the lean saturates (Clamp) rather than growing.
  {
    CosmeticTrailPose p =
        CosmeticTrailingPose(1.0f, Vec3(full * 5.0f, 0, 0), Vec3());
    CHECK(Near(p.rotation.x, lean));
    CHECK(Near(p.offset.x, -lag));
  }
  // Vertical-only motion leans but does not drag sideways.
  {
    CosmeticTrailPose p = CosmeticTrailingPose(1.0f, Vec3(0, -full, 0), Vec3());
    CHECK(Near(p.offset.x, 0.0f) && Near(p.offset.z, 0.0f));
    CHECK(Near(p.rotation.x, lean));
  }
  // Diagonal motion: lag is the unit direction, so |offset| == lag.
  {
    CosmeticTrailPose p = CosmeticTrailingPose(1.0f, Vec3(full, 0, full), Vec3());
    const f32 mag = std::sqrt(p.offset.x * p.offset.x + p.offset.z * p.offset.z);
    CHECK(Near(mag, lag));
  }
}

// ComposeAttach folds the trailing pose in: position shifts by the lag and the
// pitch gains the lean, on top of the existing bone/offset math.
static void TestComposeAttachTrailing() {
  CosmeticAsset cape;
  cape.id = "t.cape";
  cape.slot = CosmeticSlot::Back;
  cape.offset = Vec3(0, 0, -0.1f);
  cape.trail = 1.0f;

  CosmeticBoneTransform bone;
  bone.position = Vec3(10, 64, 5);
  bone.rotation = Vec3(0, 90, 0);
  bone.velocity = Vec3(CosmeticTrailFullSpeed(), 0, 0);   // sprinting +x
  bone.scale = 1.0f;
  bone.valid = true;

  const CosmeticDrawItem item = ComposeAttach(cape, bone, EntityId{7, 1});
  // Base offset rotates to (-0.1, 0, 0); the lag adds -maxLag on x.
  CHECK(Near(item.position.x, 9.9f - CosmeticTrailMaxLag()));
  CHECK(Near(item.position.y, 64.0f));
  CHECK(Near(item.position.z, 5.0f));
  // Pitch gains the full lean; yaw is unchanged.
  CHECK(Near(item.rotation.x, CosmeticTrailMaxLeanDeg()));
  CHECK(Near(item.rotation.y, 90.0f));
  CHECK(Near(item.velocity.x, CosmeticTrailFullSpeed()));

  // A rigid asset ignores the same motion entirely.
  CosmeticAsset hat = cape;
  hat.trail = 0.0f;
  const CosmeticDrawItem rigid = ComposeAttach(hat, bone, EntityId{7, 1});
  CHECK(Near(rigid.position.x, 9.9f));
  CHECK(Near(rigid.rotation.x, 0.0f));
}

// The service injects the local velocity source into bones that do not report
// their own motion, so a skeleton-only seam still gets trailing sway.
static void TestServiceVelocityInjection() {
  FreshRegistry();
  CosmeticService svc;
  Berry::Test::ShimSkeleton skel;
  Berry::Test::CaptureCosmeticSeam seam;
  Berry::Test::ShimVelocity vel;
  svc.SetSkeletonSource(&skel);
  svc.SetRenderSeam(&seam);
  svc.SetVelocitySource(&vel);

  // The cape built-in carries a trail factor.
  CHECK(svc.Equip("builtin.cape.red"));
  vel.value = Vec3(CosmeticTrailFullSpeed(), 0, 0);

  CHECK(svc.PresentLocal(EntityId{1, 1}) == 1);
  CHECK(seam.items.size() == 1);
  CHECK(seam.items[0].velocity.x > 0.0f);
  CHECK(seam.items[0].rotation.x > 0.0f);   // leaning

  // Unknown velocity reads as stationary: the cape sits flush.
  vel.known = false;
  seam.Clear();
  CHECK(svc.PresentLocal(EntityId{1, 1}) == 1);
  CHECK(Near(seam.items[0].velocity.x, 0.0f));
  CHECK(Near(seam.items[0].rotation.x, 0.0f));

  // No velocity source at all: same as unknown.
  svc.SetVelocitySource(nullptr);
  seam.Clear();
  CHECK(svc.PresentLocal(EntityId{1, 1}) == 1);
  CHECK(Near(seam.items[0].rotation.x, 0.0f));
}

// The offset cache reuses a resolve only while the skeleton's pose revision,
// the loadout and the injected velocity are all unchanged. A seam that cannot
// report a revision never hits, so the cache is never a source of stale poses.
static void TestOffsetCache() {
  FreshRegistry();
  Berry::Test::ShimSkeleton skel;
  Berry::Test::CaptureCosmeticSeam seam;
  CosmeticService svc;
  svc.SetSkeletonSource(&skel);
  svc.SetRenderSeam(&seam);
  svc.Equip("builtin.cape.red");

  const EntityId e{200, 7};
  CHECK(svc.PresentLocal(e) == 1);
  const Vec3 first = seam.items[0].position;
  CHECK(svc.stats().cacheMisses == 1);
  CHECK(svc.stats().cacheHits == 0);

  // Same revision, same loadout, same velocity: the resolve is reused and the
  // submitted item is byte-for-byte the one we cached.
  seam.Clear();
  CHECK(svc.PresentLocal(e) == 1);
  CHECK(svc.stats().cacheHits == 1);
  CHECK(seam.items[0].position == first);

  // The skeleton moved: a new revision invalidates the entry.
  skel.revision = 2;
  seam.Clear();
  CHECK(svc.PresentLocal(e) == 1);
  CHECK(svc.stats().cacheMisses == 2);
  CHECK(seam.items[0].position != first);   // recomposed at the new pose

  // Equipping something else changes the loadout key.
  svc.Equip("builtin.hat.top");
  seam.Clear();
  CHECK(svc.PresentLocal(e) == 2);
  CHECK(svc.stats().cacheMisses == 3);

  // The injected velocity is part of the identity too.
  Berry::Test::ShimVelocity vel;
  vel.value = Vec3(0, 0, 0);
  svc.SetVelocitySource(&vel);
  CHECK(svc.resolveCacheSize() == 0);   // a seam swap clears the cache
  CHECK(svc.PresentLocal(e) == 2);
  CHECK(svc.stats().cacheMisses == 4);
  vel.value = Vec3(3, 0, 0);
  seam.Clear();
  CHECK(svc.PresentLocal(e) == 2);
  CHECK(svc.stats().cacheMisses == 5);
  // Holding the velocity steady now hits.
  seam.Clear();
  CHECK(svc.PresentLocal(e) == 2);
  CHECK(svc.stats().cacheHits == 2);

  // A seam with no revision always misses, even when nothing changed: it can
  // never prove the pose is unchanged, so it must never reuse an entry. The
  // already-stored entries stay put but are not consulted.
  const size_t sizeBefore = svc.resolveCacheSize();
  CHECK(sizeBefore == 1);
  skel.noRevision = true;
  seam.Clear();
  CHECK(svc.PresentLocal(e) == 2);
  seam.Clear();
  CHECK(svc.PresentLocal(e) == 2);
  CHECK(svc.resolveCacheSize() == sizeBefore);   // no new entries stored
  CHECK(svc.stats().cacheHits == 2);             // unchanged by the two misses

  // The cache is bounded: a crowd larger than the cap cannot grow it without
  // limit, and the oldest entries are evicted.
  skel.noRevision = false;
  const CosmeticLoadout lo = svc.localLoadout();
  for (u64 i = 0; i < CosmeticService::kMaxResolveCacheEntries + 10; ++i) {
    svc.Resolve(EntityId{i, 1}, lo);
  }
  CHECK(svc.resolveCacheSize() <= CosmeticService::kMaxResolveCacheEntries);

  // Recency, not insertion order, drives eviction. Fill exactly to the cap,
  // touch the oldest entry (entity 0) to promote it, then insert one more: the
  // victim must be the next-oldest (entity 1), and entity 0 must survive.
  svc.ClearResolveCache();
  for (u64 i = 0; i < CosmeticService::kMaxResolveCacheEntries; ++i) {
    svc.Resolve(EntityId{i, 1}, lo);
  }
  CHECK(svc.resolveCacheSize() == CosmeticService::kMaxResolveCacheEntries);
  seam.Clear();
  svc.Resolve(EntityId{0, 1}, lo);                     // hit: promotes entity 0
  svc.Resolve(EntityId{999999, 1}, lo);                // overflow: evicts LRU
  CHECK(svc.resolveCacheSize() == CosmeticService::kMaxResolveCacheEntries);

  const u64 hitsBefore = svc.stats().cacheHits;
  const u64 missesBefore = svc.stats().cacheMisses;
  seam.Clear();
  svc.Resolve(EntityId{0, 1}, lo);                     // promoted: still cached
  CHECK(svc.stats().cacheHits == hitsBefore + 1);
  seam.Clear();
  svc.Resolve(EntityId{1, 1}, lo);                     // evicted: must miss
  CHECK(svc.stats().cacheMisses == missesBefore + 1);

  svc.ClearResolveCache();
  CHECK(svc.resolveCacheSize() == 0);
}

//----------------------------------------------------------------------------
// Service: local present + remote gating
//----------------------------------------------------------------------------

static void TestServiceLocalPresent() {
  FreshRegistry();
  Berry::Test::ShimSkeleton skel;
  Berry::Test::CaptureCosmeticSeam seam;
  CosmeticService svc;
  svc.SetSkeletonSource(&skel);
  svc.SetRenderSeam(&seam);

  svc.Equip("builtin.cape.red");
  svc.Equip("builtin.hat.top");
  CHECK(svc.PresentLocal(EntityId{100, 1}) == 2);
  CHECK(seam.items.size() == 2);
  CHECK(svc.stats().frames == 1);
  CHECK(svc.stats().submitted == 2);

  // Snapshot accessor reflects the state.
  CHECK(svc.localLoadout().Equipped(CosmeticSlot::Back) == "builtin.cape.red");
  CHECK(svc.Equipped(CosmeticSlot::Head) == "builtin.hat.top");
}

static void TestServiceNoSeamOrBone() {
  FreshRegistry();
  Berry::Test::CaptureCosmeticSeam seam;

  // No seam: nothing submits, no crash.
  CosmeticService bare;
  bare.Equip("builtin.cape.red");
  CHECK(bare.PresentLocal(EntityId{1, 1}) == 0);

  // A seam but no skeleton: still nothing.
  CosmeticService noSkel;
  noSkel.SetRenderSeam(&seam);
  noSkel.Equip("builtin.cape.red");
  CHECK(noSkel.PresentLocal(EntityId{1, 1}) == 0);

  // Skeleton that cannot resolve the bone: the asset is skipped and counted.
  Berry::Test::ShimSkeleton bad;
  bad.failAll = true;
  CosmeticService svc;
  svc.SetSkeletonSource(&bad);
  svc.SetRenderSeam(&seam);
  svc.Equip("builtin.cape.red");
  CHECK(svc.PresentLocal(EntityId{1, 1}) == 0);
  CHECK(svc.stats().skippedNoBone == 1);
  CHECK(seam.items.empty());
}

static void TestPresets() {
  FreshRegistry();
  Berry::Test::ShimSkeleton skel;
  Berry::Test::CaptureCosmeticSeam seam;
  CosmeticService svc;
  svc.SetSkeletonSource(&skel);
  svc.SetRenderSeam(&seam);

  svc.Equip("builtin.cape.red");
  CHECK(svc.SavePreset("pvp"));
  CHECK(svc.HasPreset("pvp"));
  CHECK(svc.PresetNames().size() == 1);

  CHECK(svc.Unequip(CosmeticSlot::Back));
  CHECK(svc.Equipped(CosmeticSlot::Back).empty());
  CHECK(svc.ApplyPreset("pvp"));
  CHECK(svc.Equipped(CosmeticSlot::Back) == "builtin.cape.red");
  CHECK(!svc.ApplyPreset("ghost"));
}

static void TestRemoteOwnershipGate() {
  FreshRegistry();
  auto& ownership = CosmeticOwnership::Instance();
  ownership.Clear();

  Berry::Test::ShimSkeleton skel;
  Berry::Test::CaptureCosmeticSeam seam;
  CosmeticService svc;
  svc.SetSkeletonSource(&skel);
  svc.SetRenderSeam(&seam);

  CosmeticLoadout remote;
  remote.Equip("builtin.cape.red");
  remote.Equip("builtin.hat.top");

  // Nothing owned yet: the loadout installs but is empty.
  CHECK(svc.SetRemoteLoadout("bob", remote) == 0);
  CHECK(svc.RemotePlayers().size() == 1);

  // Own the cape only: exactly one slot survives.
  CHECK(ownership.Grant("bob", "builtin.cape.red"));
  CHECK(svc.SetRemoteLoadout("bob", remote) == 1);
  CHECK(svc.stats().skippedNotOwned >= 1);

  // Local-only (default): a remote present draws nothing.
  CHECK(svc.PresentRemote("bob", EntityId{200, 2}) == 0);
  CHECK(seam.items.empty());

  // With sync enabled, the owned cape draws.
  svc.SetLocalOnly(false);
  CHECK(svc.PresentRemote("bob", EntityId{200, 2}) == 1);
  CHECK(seam.items.size() == 1);
  CHECK(seam.items[0].assetId == "builtin.cape.red");

  CHECK(svc.ClearRemoteLoadout("bob"));
  CHECK(svc.RemotePlayers().empty());
  CHECK(!svc.ClearRemoteLoadout("bob"));

  // Empty player id is refused outright.
  CHECK(svc.SetRemoteLoadout("", remote) == 0);
}

//----------------------------------------------------------------------------
// Sync channel
//----------------------------------------------------------------------------

namespace {

struct CaptureTransport final : CosmeticSyncTransport {
  std::vector<std::string> sent;
  bool fail = false;
  bool Send(const std::string& m) override {
    if (fail) return false;
    sent.push_back(m);
    return true;
  }
};

}  // namespace

static void TestSyncEd25519HappyPath() {
  FreshRegistry();
  auto& ownership = CosmeticOwnership::Instance();
  ownership.Clear();

  std::string pub, sec;
  SeedKey(&pub, &sec, 'S');

  CaptureTransport transport;
  CosmeticSyncChannel ch;
  ch.SetTransport(&transport);
  ch.SetVerifyKey(CosmeticSyncKey::Ed25519PublicKey(crypto::ToHexLower(pub)));
  CHECK(ch.configured());

  CosmeticLoadout lo;
  lo.Equip("builtin.cape.red");
  lo.Equip("builtin.hat.top");
  CHECK(ch.Publish("alice", lo, sec, false));
  CHECK(transport.sent.size() == 1);

  CosmeticSyncMessage msg;
  CHECK(ch.Verify(transport.sent[0], &msg) == CosmeticSyncResult::Ok);
  CHECK(msg.playerId == "alice");
  CHECK(msg.algo == signature_algo::Ed25519());
  CHECK(msg.loadout.Count() == 2);

  // Receive applies through the ownership gate: alice owns only the cape.
  ownership.Grant("alice", "builtin.cape.red");
  Berry::Test::ShimSkeleton skel;
  Berry::Test::CaptureCosmeticSeam seam;
  CosmeticService svc;
  svc.SetSkeletonSource(&skel);
  svc.SetRenderSeam(&seam);
  size_t applied = 0;
  CHECK(ch.Receive(transport.sent[0], &svc, &applied) == CosmeticSyncResult::Ok);
  CHECK(applied == 1);
  CHECK(ch.accepted() == 1);
}

static void TestSyncRejections() {
  FreshRegistry();
  std::string pub, sec;
  SeedKey(&pub, &sec, 'S');

  CaptureTransport transport;
  CosmeticSyncChannel ch;
  ch.SetTransport(&transport);
  ch.SetVerifyKey(CosmeticSyncKey::Ed25519PublicKey(crypto::ToHexLower(pub)));

  CosmeticLoadout lo;
  lo.Equip("builtin.cape.red");
  CHECK(ch.Publish("alice", lo, sec, false));
  const std::string good = transport.sent[0];

  CosmeticSyncMessage msg;
  // Empty and garbage.
  CHECK(ch.Verify("", &msg) == CosmeticSyncResult::EmptyMessage);
  CHECK(ch.Verify("garbage", &msg) == CosmeticSyncResult::BadEnvelope);
  // Oversized.
  CHECK(ch.Verify(std::string(CosmeticSyncMaxMessageBytes() + 1, 'x'), &msg) ==
        CosmeticSyncResult::TooLarge);
  // Tampered body: signature no longer verifies.
  std::string tampered = good;
  const size_t p = tampered.find("builtin.cape.red");
  CHECK(p != std::string::npos);
  tampered[p] = 'X';
  CHECK(ch.Verify(tampered, &msg) == CosmeticSyncResult::BadSignature);
  // Wrong key.
  std::string pub2, sec2;
  SeedKey(&pub2, &sec2, 'T');
  CosmeticSyncChannel other;
  other.SetVerifyKey(CosmeticSyncKey::Ed25519PublicKey(crypto::ToHexLower(pub2)));
  CHECK(other.Verify(good, &msg) == CosmeticSyncResult::BadSignature);
  // No key configured.
  CosmeticSyncChannel bare;
  CHECK(bare.Verify(good, &msg) == CosmeticSyncResult::NotConfigured);

  // A rejection changes no state and is counted.
  CosmeticService svc;
  size_t applied = 123;
  CHECK(ch.Receive(tampered, &svc, &applied) == CosmeticSyncResult::BadSignature);
  CHECK(applied == 123);   // untouched
  CHECK(ch.rejected() == 1);
  CHECK(ch.lastResult() == CosmeticSyncResult::BadSignature);
}

static void TestSyncHmacAndAlgorithmConfusion() {
  FreshRegistry();
  CosmeticSyncChannel hmac;
  hmac.SetVerifyKey(CosmeticSyncKey::HmacSecret("s3cret"));

  CosmeticLoadout lo;
  lo.Equip("builtin.cape.red");
  std::string message;
  CHECK(CosmeticSyncSignHmac("bob", lo, "s3cret", &message));

  CosmeticSyncMessage msg;
  CHECK(hmac.Verify(message, &msg) == CosmeticSyncResult::Ok);
  CHECK(msg.playerId == "bob");

  // Tampered HMAC frame is rejected.
  std::string tampered = message;
  tampered[tampered.size() - 1] =
      (tampered[tampered.size() - 1] == 'a') ? 'b' : 'a';
  CHECK(hmac.Verify(tampered, &msg) == CosmeticSyncResult::BadSignature);

  // An HMAC frame presented to an Ed25519 verifier must not pass: the
  // algorithm is checked against the key's mode.
  std::string pub, sec;
  SeedKey(&pub, &sec, 'S');
  CosmeticSyncChannel ed;
  ed.SetVerifyKey(CosmeticSyncKey::Ed25519PublicKey(crypto::ToHexLower(pub)));
  CHECK(ed.Verify(message, &msg) == CosmeticSyncResult::BadSignature);

  // An unknown algorithm token is reported as such, not silently accepted.
  const std::string unknown =
      std::string(CosmeticSyncMagic()) + "\nalgo: rot13\nsig: 00\npayload: 0\n";
  CHECK(ed.Verify(unknown, &msg) == CosmeticSyncResult::UnknownAlgorithm);

  // Publishing with no transport fails closed.
  CosmeticSyncChannel noTransport;
  CHECK(!noTransport.Publish("x", lo, sec, false));
}

//----------------------------------------------------------------------------
// Sync channel: replay, rate limiting, revocation
//----------------------------------------------------------------------------

namespace {

/// A controllable clock so the replay TTL and the rate limiter are driven by
/// test time, not wall time.
struct FakeClock {
  u64 ms = 1000;
  u64 operator()() const { return ms; }
  void Advance(u64 d) { ms += d; }
};

}  // namespace

static void TestSyncReplayRefused() {
  FreshRegistry();
  auto& ownership = CosmeticOwnership::Instance();
  ownership.Clear();
  ownership.Grant("alice", "builtin.cape.red");

  std::string pub, sec;
  SeedKey(&pub, &sec, 'S');

  FakeClock clock;
  CaptureTransport transport;
  CosmeticSyncChannel ch;
  ch.SetClock([&clock] { return clock(); });
  ch.SetTransport(&transport);
  ch.SetVerifyKey(CosmeticSyncKey::Ed25519PublicKey(crypto::ToHexLower(pub)));

  CosmeticLoadout lo;
  lo.Equip("builtin.cape.red");
  CHECK(ch.Publish("alice", lo, sec, false));
  const std::string good = transport.sent[0];

  Berry::Test::ShimSkeleton skel;
  Berry::Test::CaptureCosmeticSeam seam;
  CosmeticService svc;
  svc.SetSkeletonSource(&skel);
  svc.SetRenderSeam(&seam);

  // First delivery applies.
  size_t applied = 0;
  CHECK(ch.Receive(good, &svc, &applied) == CosmeticSyncResult::Ok);
  CHECK(applied == 1);
  CHECK(ch.replayWindowSize() == 1);

  // The identical message again is refused as a replay, and changes nothing.
  applied = 99;
  CHECK(ch.Receive(good, &svc, &applied) == CosmeticSyncResult::Replayed);
  CHECK(applied == 99);
  CHECK(ch.lastResult() == CosmeticSyncResult::Replayed);

  // Verify() is a pure signature check and stays Ok for the same bytes: the
  // replay defence lives in Receive.
  CosmeticSyncMessage msg;
  CHECK(ch.Verify(good, &msg) == CosmeticSyncResult::Ok);

  // A different, freshly signed message is a different digest and is accepted.
  CosmeticLoadout lo2;
  lo2.Equip("builtin.hat.top");
  ownership.Grant("alice", "builtin.hat.top");
  CHECK(ch.Publish("alice", lo2, sec, false));
  CHECK(ch.Receive(transport.sent[1], &svc, &applied) == CosmeticSyncResult::Ok);
}

static void TestSyncReplayWindowExpires() {
  FreshRegistry();
  auto& ownership = CosmeticOwnership::Instance();
  ownership.Clear();
  ownership.Grant("alice", "builtin.cape.red");

  std::string pub, sec;
  SeedKey(&pub, &sec, 'S');

  FakeClock clock;
  CaptureTransport transport;
  CosmeticSyncChannel ch;
  ch.SetClock([&clock] { return clock(); });
  ch.SetTransport(&transport);
  ch.SetVerifyKey(CosmeticSyncKey::Ed25519PublicKey(crypto::ToHexLower(pub)));
  CosmeticSyncConfig cfg;
  cfg.replayTtlMs = 500;
  // A big burst so the rate limiter does not interfere with this test.
  cfg.rateBurst = 1000;
  ch.SetConfig(cfg);

  CosmeticLoadout lo;
  lo.Equip("builtin.cape.red");
  CHECK(ch.Publish("alice", lo, sec, false));
  const std::string good = transport.sent[0];

  CHECK(ch.Receive(good, nullptr) == CosmeticSyncResult::Ok);
  CHECK(ch.Receive(good, nullptr) == CosmeticSyncResult::Replayed);

  // Past the TTL the digest is dropped, so the same message is admitted again.
  clock.Advance(501);
  CHECK(ch.Receive(good, nullptr) == CosmeticSyncResult::Ok);
}

static void TestSyncRateLimited() {
  FreshRegistry();
  auto& ownership = CosmeticOwnership::Instance();
  ownership.Clear();
  ownership.Grant("alice", "builtin.cape.red");

  std::string pub, sec;
  SeedKey(&pub, &sec, 'S');

  FakeClock clock;
  CaptureTransport transport;
  CosmeticSyncChannel ch;
  ch.SetClock([&clock] { return clock(); });
  ch.SetTransport(&transport);
  ch.SetVerifyKey(CosmeticSyncKey::Ed25519PublicKey(crypto::ToHexLower(pub)));
  CosmeticSyncConfig cfg;
  cfg.rateBurst = 3.0;
  cfg.ratePerSecond = 1.0;   // 1 token/sec
  ch.SetConfig(cfg);

  // Distinct signed messages from one peer, so the limiter (not a replay) is
  // what refuses them. The four builtin assets sit in four slots, so the 16
  // subsets are 16 distinct loadouts.
  const char* kAssets[4] = {"builtin.aura.spark", "builtin.cape.red",
                            "builtin.hat.top", "builtin.wings.angel"};
  std::vector<std::string> msgs;
  for (int mask = 0; mask < 10; ++mask) {
    CosmeticLoadout lo;
    for (int b = 0; b < 4; ++b) {
      if (mask & (1 << b)) lo.Equip(kAssets[b]);
    }
    std::string m;
    CHECK(CosmeticSyncSignEd25519("alice", lo, sec, &m));
    msgs.push_back(m);
  }

  // The first three spend the burst; the rest are rate limited.
  int ok = 0, limited = 0;
  for (const auto& m : msgs) {
    const CosmeticSyncResult r = ch.Receive(m, nullptr);
    if (r == CosmeticSyncResult::Ok) ++ok;
    else if (r == CosmeticSyncResult::RateLimited) ++limited;
  }
  CHECK(ok == 3);
  CHECK(limited == 7);
  CHECK(ch.lastResult() == CosmeticSyncResult::RateLimited);

  // After enough time passes, a token refills and one more is admitted.
  clock.Advance(1000);
  CHECK(ch.Receive(msgs[3], nullptr) == CosmeticSyncResult::Ok);

  // The rate limit is per peer: a different peer has its own full bucket.
  std::string m;
  CosmeticLoadout lo;
  lo.Equip("builtin.cape.red");
  CHECK(CosmeticSyncSignEd25519("bob", lo, sec, &m));
  CHECK(ch.Receive(m, nullptr) == CosmeticSyncResult::Ok);
}

static void TestSyncPeerTableBounded() {
  FreshRegistry();
  std::string pub, sec;
  SeedKey(&pub, &sec, 'S');

  FakeClock clock;
  CaptureTransport transport;
  CosmeticSyncChannel ch;
  ch.SetClock([&clock] { return clock(); });
  ch.SetTransport(&transport);
  ch.SetVerifyKey(CosmeticSyncKey::Ed25519PublicKey(crypto::ToHexLower(pub)));
  CosmeticSyncConfig cfg;
  cfg.maxPeers = 4;
  cfg.rateBurst = 1000;   // don't let the limiter mask the eviction test
  ch.SetConfig(cfg);

  CosmeticLoadout lo;
  lo.Equip("builtin.cape.red");
  for (int i = 0; i < 50; ++i) {
    std::string m;
    CHECK(CosmeticSyncSignEd25519("peer" + std::to_string(i), lo, sec, &m));
    CHECK(ch.Receive(m, nullptr) == CosmeticSyncResult::Ok);
  }
  // 50 distinct peers were seen, but the table never grew past the cap.
  CHECK(ch.trackedPeers() == 4);
}

static void TestSyncRevocation() {
  FreshRegistry();
  auto& ownership = CosmeticOwnership::Instance();
  ownership.Clear();
  ownership.Grant("mallory", "builtin.cape.red");

  std::string pub, sec;
  SeedKey(&pub, &sec, 'S');

  FakeClock clock;
  CaptureTransport transport;
  CosmeticSyncChannel ch;
  ch.SetClock([&clock] { return clock(); });
  ch.SetTransport(&transport);
  ch.SetVerifyKey(CosmeticSyncKey::Ed25519PublicKey(crypto::ToHexLower(pub)));

  CosmeticLoadout lo;
  lo.Equip("builtin.cape.red");
  CHECK(ch.Publish("mallory", lo, sec, false));
  const std::string good = transport.sent[0];

  // Before revocation the (valid, signed) message is accepted.
  CHECK(ch.Receive(good, nullptr) == CosmeticSyncResult::Ok);

  // After revoking the peer, even a validly signed, unseen message is refused.
  ch.RevokePeer("mallory");
  CHECK(ch.IsPeerRevoked("mallory"));
  CHECK(!ch.IsPeerRevoked("alice"));

  CosmeticLoadout lo2;
  lo2.Equip("builtin.hat.top");
  CHECK(ch.Publish("mallory", lo2, sec, false));
  CHECK(ch.Receive(transport.sent[1], nullptr) == CosmeticSyncResult::Revoked);
  CHECK(ch.lastResult() == CosmeticSyncResult::Revoked);

  // Clearing the list re-admits the peer.
  ch.ClearRevocations();
  CHECK(!ch.IsPeerRevoked("mallory"));
  CHECK(ch.Receive(transport.sent[1], nullptr) == CosmeticSyncResult::Ok);
}

//----------------------------------------------------------------------------
// Script bindings
//----------------------------------------------------------------------------

static void TestCosmeticBindings() {
  FreshRegistry();
  CosmeticService svc;
  ScriptApi::Services services;
  services.cosmetics = &svc;
  ScriptApi api(services);
  ScriptBindings bindings;
  const size_t n = api.InstallBindings(&bindings);
  CHECK(n == api::BindingSpecCount());
  CHECK(bindings.Has("cosmetic.equip"));
  CHECK(bindings.Has("cosmetic.equipped"));

  ScriptValue out;
  // Without the cosmetic grant the binding is denied.
  CHECK(CallBinding(bindings, "cosmetic.equip",
                    {ScriptValue::Str("builtin.cape.red")}, {"render"}, &out) ==
        ScriptStatus::CapabilityDenied);
  // With it, the call reaches the service.
  CHECK(CallBinding(bindings, "cosmetic.equip",
                    {ScriptValue::Str("builtin.cape.red")}, {"cosmetic"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(CallBinding(bindings, "cosmetic.equipped",
                    {ScriptValue::Str("back")}, {"cosmetic"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.ToDisplay() == "builtin.cape.red");

  // Unknown asset and unknown slot are errors, not silent successes.
  CHECK(CallBinding(bindings, "cosmetic.equip", {ScriptValue::Str("nope")},
                    {"cosmetic"}, &out) == ScriptStatus::Error);
  CHECK(CallBinding(bindings, "cosmetic.equipped", {ScriptValue::Str("hat")},
                    {"cosmetic"}, &out) == ScriptStatus::Error);
  // Wrong arity fails closed before the body.
  CHECK(CallBinding(bindings, "cosmetic.equip", {}, {"cosmetic"}, &out) ==
        ScriptStatus::Error);

  // Presets round-trip through the binding, and the list is newline-joined.
  CHECK(CallBinding(bindings, "cosmetic.save_preset", {ScriptValue::Str("pvp")},
                    {"cosmetic"}, &out) == ScriptStatus::Ok);
  CHECK(CallBinding(bindings, "cosmetic.presets", {}, {"cosmetic"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.ToDisplay() == "pvp");
  CHECK(CallBinding(bindings, "cosmetic.apply_preset",
                    {ScriptValue::Str("ghost")}, {"cosmetic"}, &out) ==
        ScriptStatus::Error);

  // set_local_only flips the flag and nothing else.
  CHECK(CallBinding(bindings, "cosmetic.set_local_only",
                    {ScriptValue::Bool(false)}, {"cosmetic"}, &out) ==
        ScriptStatus::Ok);
  CHECK(!svc.localOnly());

  // A missing service makes every cosmetic binding fail closed.
  ScriptApi::Services none;
  ScriptApi bare(none);
  ScriptBindings bareBindings;
  bare.InstallBindings(&bareBindings);
  CHECK(CallBinding(bareBindings, "cosmetic.equip",
                    {ScriptValue::Str("builtin.cape.red")}, {"cosmetic"}, &out) ==
        ScriptStatus::Error);
}

// The LAN discovery + sync surface: `cosmetic.peers`/`cosmetic.peer` read the
// client's own mDNS cache, and `cosmetic.sync_stats` reads the channel counters.
// Both fail closed with no service bound, so a script never reads a fabricated
// peer or a stale success.
static void TestLanSyncBindings() {
  LanDiscoveryConfig dcfg;
  dcfg.iface = "127.0.0.1";
  LanDiscovery discovery;
  CHECK(discovery.Open(dcfg));
  CHECK(discovery.Publish("alice", 47823, "sync=1"));

  CosmeticSyncChannel ch;
  ch.SetVerifyKey(CosmeticSyncKey::HmacSecret("shared"));

  ScriptApi::Services services;
  services.discovery = &discovery;
  services.sync = &ch;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  const std::vector<std::string> caps = {"cosmetic"};

  // The channel is configured but nothing has been accepted or refused yet.
  CHECK(CallBinding(bindings, "cosmetic.sync_stats", {}, caps, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsTable()->at("configured").Truthy());
  CHECK(out.AsTable()->at("accepted").AsNumber() == 0);
  CHECK(out.AsTable()->at("rejected").AsNumber() == 0);

  // Discovery names this client's own published service once it is polled (a
  // local responder answers its own query on loopback).
  CHECK(CallBinding(bindings, "cosmetic.peers", {}, caps, &out) ==
        ScriptStatus::Ok);
  const f64 before = out.AsNumber();
  for (int i = 0; i < 40; ++i) {
    discovery.Query();
    discovery.Poll(5);
    CHECK(CallBinding(bindings, "cosmetic.peers", {}, caps, &out) ==
          ScriptStatus::Ok);
    if (out.AsNumber() > 0) break;
  }
  CHECK(out.AsNumber() >= before);

  // `cosmetic.peer` returns a table for a live index and an empty one (no
  // fabricated fields) out of range.
  CHECK(CallBinding(bindings, "cosmetic.peer", {ScriptValue::Num(0)}, caps, &out) ==
        ScriptStatus::Ok);
  if (out.AsTable()->find("instance") != out.AsTable()->end()) {
    CHECK(out.AsTable()->at("port").AsNumber() == 47823);
  }
  CHECK(CallBinding(bindings, "cosmetic.peer", {ScriptValue::Num(9999)}, caps, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsTable()->find("instance") == out.AsTable()->end());

  // Without the cosmetic grant the calls are denied.
  CHECK(CallBinding(bindings, "cosmetic.peers", {}, {"render"}, &out) ==
        ScriptStatus::CapabilityDenied);

  // A missing service fails closed: 0 peers, empty peer, unconfigured stats.
  ScriptApi::Services none;
  ScriptApi bare(none);
  ScriptBindings bareBindings;
  bare.InstallBindings(&bareBindings);
  CHECK(CallBinding(bareBindings, "cosmetic.peers", {}, caps, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 0);
  CHECK(CallBinding(bareBindings, "cosmetic.peer", {ScriptValue::Num(0)}, caps, &out) ==
        ScriptStatus::Error);
  CHECK(CallBinding(bareBindings, "cosmetic.sync_stats", {}, caps, &out) ==
        ScriptStatus::Ok);
  CHECK(!out.AsTable()->at("configured").Truthy());
  CHECK(out.AsTable()->at("accepted").AsNumber() == 0);

  discovery.Close();
}

// input.velocity returns the local player's core motion as a Vec3 table. It is
// capability-gated like the rest of the module, and it fails closed: a missing
// source reports a zero (stationary) vector, never a stale or fabricated one.
static void TestVelocityBinding() {
  Berry::Test::ShimVelocity vel;
  vel.value = Vec3(1.5f, -0.25f, -3.0f);
  ScriptApi::Services services;
  services.velocity = &vel;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  // Denied without the input grant.
  CHECK(CallBinding(bindings, "input.velocity", {}, {"render"}, &out) ==
        ScriptStatus::CapabilityDenied);

  CHECK(CallBinding(bindings, "input.velocity", {}, {"input"}, &out) ==
        ScriptStatus::Ok);
  const ScriptTable* t = out.AsTable();
  CHECK(t != nullptr);
  CHECK(t && Near((f32)t->at("x").AsNumber(), 1.5f));
  CHECK(t && Near((f32)t->at("y").AsNumber(), -0.25f));
  CHECK(t && Near((f32)t->at("z").AsNumber(), -3.0f));

  // No source: zero vector (stationary), still a well-formed Vec3.
  ScriptApi::Services none;
  ScriptApi bare(none);
  ScriptBindings bareBindings;
  bare.InstallBindings(&bareBindings);
  CHECK(CallBinding(bareBindings, "input.velocity", {}, {"input"}, &out) ==
        ScriptStatus::Ok);
  const ScriptTable* z = out.AsTable();
  CHECK(z != nullptr);
  CHECK(z && Near((f32)z->at("x").AsNumber(), 0.0f));
  CHECK(z && Near((f32)z->at("z").AsNumber(), 0.0f));
}

//----------------------------------------------------------------------------
// Drawing through Berry::Render
//
// A cosmetic must present through the one RenderSeam, like every other overlay
// consumer: the service resolves its loadout into draw items and queues them as
// world-space billboards the Renderer projects with its camera.
//----------------------------------------------------------------------------

static void TestDrawThroughRenderer() {
  FreshRegistry();
  Berry::Test::ShimSkeleton skel;
  CosmeticService svc;
  svc.SetSkeletonSource(&skel);
  svc.Equip("builtin.cape.red");

  Renderer r;
  Berry::Test::CaptureRenderSeam seam;
  r.BindSeam(&seam);

  const EntityId e{1, 1};

  // No camera yet: a billboard cannot be projected, so nothing is drawn and
  // DrawLocal reports zero rather than drawing at a bogus spot.
  r.BeginFrame(Vec2(1280, 720));
  CHECK(svc.DrawLocal(r, e) == 0);
  r.EndFrame();
  CHECK(seam.rects == 0);

  // A camera aimed at the rig's bone makes the cape project as one quad.
  CameraState cam;
  cam.width = 1280;
  cam.height = 720;
  cam.view = Mat4::LookAt(Vec3(10.0f, 64.0f, 15.0f), Vec3(10.0f, 64.0f, 5.0f),
                          Vec3(0, 1, 0));
  cam.projection = Mat4::Perspective(70, 1280.0f / 720.0f, 0.1f, 200.0f);
  cam.valid = true;
  r.SetCamera(cam);

  r.BeginFrame(Vec2(1280, 720));
  CHECK(svc.DrawLocal(r, e) == 1);
  r.EndFrame();
  CHECK(seam.rects == 1);
  CHECK(seam.lines == 0 && seam.texts == 0 && seam.circles == 0);

  // With no skeleton source there is nothing to resolve; still zero, no crash.
  svc.SetSkeletonSource(nullptr);
  r.BeginFrame(Vec2(1280, 720));
  CHECK(svc.DrawLocal(r, e) == 0);
  r.EndFrame();

  // A non-positive scale is refused by the item composer, not drawn inverted.
  svc.SetSkeletonSource(&skel);
  CosmeticDrawItem bad;
  bad.position = Vec3(10, 64, 5);
  bad.scale = 0.0f;
  r.BeginFrame(Vec2(1280, 720));
  CHECK(!CosmeticService::DrawItem(r, bad));
  r.EndFrame();
  CHECK(seam.rects == 0);
}

static void TestDrawRemoteGated() {
  FreshRegistry();
  Berry::Test::ShimSkeleton skel;
  CosmeticService svc;
  svc.SetSkeletonSource(&skel);

  Renderer r;
  Berry::Test::CaptureRenderSeam seam;
  r.BindSeam(&seam);
  CameraState cam;
  cam.width = 1280;
  cam.height = 720;
  cam.view = Mat4::LookAt(Vec3(10.0f, 64.0f, 15.0f), Vec3(10.0f, 64.0f, 5.0f),
                          Vec3(0, 1, 0));
  cam.projection = Mat4::Perspective(70, 1280.0f / 720.0f, 0.1f, 200.0f);
  cam.valid = true;
  r.SetCamera(cam);

  const std::string peer = "alex";
  CosmeticLoadout loadout;
  CHECK(loadout.Equip(CosmeticRegistry::Instance().All()[0].id));
  CosmeticOwnership::Instance().Grant(peer, CosmeticRegistry::Instance().All()[0].id);
  CHECK(svc.SetRemoteLoadout(peer, loadout) == 1);

  // Local-only (the default) draws nothing for a remote player.
  r.BeginFrame(Vec2(1280, 720));
  CHECK(svc.DrawRemote(r, peer, EntityId{2, 2}) == 0);
  r.EndFrame();
  CHECK(seam.rects == 0);

  // Turning local-only off renders the verified remote loadout.
  svc.SetLocalOnly(false);
  r.BeginFrame(Vec2(1280, 720));
  CHECK(svc.DrawRemote(r, peer, EntityId{2, 2}) == 1);
  r.EndFrame();
  CHECK(seam.rects == 1);

  // An unknown peer still draws nothing.
  r.BeginFrame(Vec2(1280, 720));
  CHECK(svc.DrawRemote(r, "nobody", EntityId{3, 3}) == 0);
  r.EndFrame();

  CosmeticOwnership::Instance().Clear();
}

// A per-asset custom animation: an asset that names a clip is moved by the
// animation source's pose, composed on top of its static offset. The service
// samples the clip at the animation clock scaled by the asset's speed, and an
// unknown clip (or no source) fails closed to a rigid placement.
static void TestAssetCustomAnimation() {
  FreshRegistry();
  Berry::Test::ShimSkeleton skel;
  Berry::Test::CaptureCosmeticSeam seam;
  Berry::Test::ShimAnimation anim;
  CosmeticService svc;
  svc.SetSkeletonSource(&skel);
  svc.SetRenderSeam(&seam);

  // Register an asset that names a clip and a speed.
  CosmeticAsset cape;
  cape.id = "t.anim.cape";
  cape.slot = CosmeticSlot::Back;
  cape.offset = Vec3(0, 0, -0.1f);
  cape.trail = 0.0f;                 // isolate the animation from the sway
  cape.animation = "cape.flap";
  cape.animationSpeed = 2.0f;
  CosmeticError err = CosmeticError::Ok;
  CHECK(CosmeticRegistry::Instance().Register(cape, &err));
  svc.SetAnimationSource(&anim);
  CHECK(svc.Equip("t.anim.cape"));

  // The animation source reports a bone-local flap: +0.3 on y, +30 degrees roll.
  anim.pose.offset = Vec3(0, 0.3f, 0);
  anim.pose.rotation = Vec3(0, 0, 30.0f);
  anim.pose.scale = 1.0f;
  anim.pose.valid = true;

  const EntityId e{300, 1};
  // Advance the clock to 0.5 s. The clip is sampled at 0.5 * speed(2) = 1.0 s.
  svc.AdvanceAnimations(0.5f);
  CHECK(Near(svc.animationTime(), 0.5f));
  seam.Clear();
  CHECK(svc.PresentLocal(e) == 1);
  CHECK(anim.lastClip == "cape.flap");
  CHECK(Near(anim.lastTime, 1.0f));
  // The animation's y offset is added before the bone's 90-degree yaw rotates
  // it, so a +y offset stays +y. Base offset (0,0,-0.1) yaws to (-0.1,0,0).
  // The shim rig sits at x=10 plus a 0.01*revision jitter (revision 1 here).
  CHECK(Near(seam.items[0].position.x, 9.91f));
  CHECK(Near(seam.items[0].position.y, 64.3f));
  // The animation's roll adds to the bone yaw (90) on y.
  CHECK(Near(seam.items[0].rotation.z, 30.0f));
  CHECK(Near(seam.items[0].rotation.y, 90.0f));
  CHECK(seam.items[0].animation == "cape.flap");
  CHECK(Near(seam.items[0].animationTime, 1.0f));

  // A source that cannot sample (unknown clip) places the asset rigidly, and
  // the draw item carries no animation name.
  anim.enabled = false;
  seam.Clear();
  CHECK(svc.PresentLocal(e) == 1);
  CHECK(Near(seam.items[0].position.y, 64.0f));
  CHECK(Near(seam.items[0].rotation.z, 0.0f));
  CHECK(seam.items[0].animation.empty());
  CHECK(Near(seam.items[0].animationTime, 0.0f));

  // With no animation source at all, the asset is rigid too (fail closed).
  anim.enabled = true;
  svc.SetAnimationSource(nullptr);
  seam.Clear();
  CHECK(svc.PresentLocal(e) == 1);
  CHECK(Near(seam.items[0].position.y, 64.0f));
  CHECK(seam.items[0].animation.empty());
}

// An animated loadout is never cached: the pose changes with the clock while
// the skeleton revision is unchanged, so a cached entry would freeze it. An
// all-rigid loadout is cached as before.
static void TestAnimatedResolveNotCached() {
  FreshRegistry();
  Berry::Test::ShimSkeleton skel;
  Berry::Test::CaptureCosmeticSeam seam;
  Berry::Test::ShimAnimation anim;
  CosmeticService svc;
  svc.SetSkeletonSource(&skel);
  svc.SetRenderSeam(&seam);
  svc.SetAnimationSource(&anim);

  CosmeticAsset cape;
  cape.id = "t.anim.spin";
  cape.slot = CosmeticSlot::Back;
  cape.animation = "aura.spin";
  CosmeticError err = CosmeticError::Ok;
  CHECK(CosmeticRegistry::Instance().Register(cape, &err));
  CHECK(svc.Equip("t.anim.spin"));

  anim.pose.valid = true;
  anim.pose.offset = Vec3(0, 0.1f, 0);

  const EntityId e{301, 2};
  // The skeleton reports a stable revision, so a rigid asset would hit. An
  // animated one must not: each resolve recomposes and stores no entry.
  svc.AdvanceAnimations(0.1f);
  CHECK(svc.PresentLocal(e) == 1);
  CHECK(svc.resolveCacheSize() == 0);
  CHECK(svc.stats().cacheMisses == 1);
  svc.AdvanceAnimations(0.1f);
  CHECK(svc.PresentLocal(e) == 1);
  CHECK(svc.resolveCacheSize() == 0);
  CHECK(svc.stats().cacheHits == 0);

  // Unequip the animated asset and equip a rigid one (the hat names no clip):
  // the resolve is cacheable again and a second present hits.
  svc.Unequip(CosmeticSlot::Back);
  CHECK(svc.Equip("builtin.hat.top"));
  seam.Clear();
  CHECK(svc.PresentLocal(e) == 1);
  CHECK(svc.resolveCacheSize() == 1);
  seam.Clear();
  CHECK(svc.PresentLocal(e) == 1);
  CHECK(svc.stats().cacheHits == 1);
}

// The AnimatorAnimationSource adapter samples a named clip from the animator,
// independent of what is playing, so a per-asset clip and a script's playback
// never fight. An unknown clip fails closed.
static void TestAnimatorAnimationSource() {
  Animator anim;
  CHECK(anim.AddClip("flap", 1.0f, true));
  CHECK(anim.AddKey("flap", Bone::Back, 0.0f, Vec3(0, 0, 0), Vec3(0, 0, 0), 1.0f));
  CHECK(anim.AddKey("flap", Bone::Back, 1.0f, Vec3(0, 0.2f, 0), Vec3(0, 0, 45.0f),
                    1.5f));
  AnimatorAnimationSource src(&anim);

  CosmeticAnimPose pose;
  // Halfway through a looping clip: the lerp midpoint.
  CHECK(src.Sample("flap", Bone::Back, 0.5f, &pose));
  CHECK(pose.valid);
  CHECK(Near(pose.offset.y, 0.1f));
  CHECK(Near(pose.rotation.z, 22.5f));
  CHECK(Near(pose.scale, 1.25f));

  // A looping clip wraps: t=1.5 samples the same as t=0.5.
  CosmeticAnimPose wrapped;
  CHECK(src.Sample("flap", Bone::Back, 1.5f, &wrapped));
  CHECK(Near(wrapped.offset.y, 0.1f));

  // Unknown clip, unknown bone and an empty name all fail closed.
  CosmeticAnimPose bad;
  CHECK(!src.Sample("nope", Bone::Back, 0.5f, &bad));
  CHECK(!bad.valid);
  CHECK(!src.Sample("flap", Bone::Head, 0.5f, &bad));
  CHECK(!src.Sample("", Bone::Back, 0.5f, &bad));

  // Sampling a clip never disturbs the animator's own playback state.
  CHECK(anim.Play("flap"));
  anim.Advance(0.25f);
  CHECK(src.Sample("flap", Bone::Back, 0.9f, &pose));
  CHECK(Near(anim.time(), 0.25f));
}

// A pack may name a clip and a speed; a bad name (over-long) or a bad speed is
// refused wholesale, and a valid one round-trips through the registry.
static void TestPackAnimationFields() {
  CosmeticRegistry::Instance().Clear();
  const std::string good = R"({
    "format": 1, "name": "anim-pack", "author": "t",
    "assets": [
      {"id": "p.cape", "slot": "back", "animation": "flap",
       "animation_speed": 2.5}
    ]
  })";
  std::vector<CosmeticAsset> assets;
  std::string name;
  std::string detail;
  CHECK(CosmeticPackLoader::ParsePack(good, &assets, &name, &detail) ==
        PackError::Ok);
  CHECK(assets.size() == 1);
  CHECK(assets[0].animation == "flap");
  CHECK(Near(assets[0].animationSpeed, 2.5f));

  // A non-positive speed is refused.
  const std::string badSpeed = R"({
    "format": 1, "name": "p", "author": "t",
    "assets": [{"id": "p.cape", "slot": "back", "animation_speed": 0}]
  })";
  CHECK(CosmeticPackLoader::ParsePack(badSpeed, &assets) ==
        PackError::BadAsset);

  // An over-long animation name is refused.
  std::string longName(65, 'a');
  const std::string badName = R"({
    "format": 1, "name": "p", "author": "t",
    "assets": [{"id": "p.cape", "slot": "back", "animation": ")" +
                              longName + R"("}]
  })";
  CHECK(CosmeticPackLoader::ParsePack(badName, &assets) == PackError::BadAsset);

  // The registry refuses a non-finite speed directly.
  CosmeticAsset a;
  a.id = "r.anim";
  a.slot = CosmeticSlot::Back;
  a.animationSpeed = std::nanf("");
  CosmeticError err = CosmeticError::Ok;
  CHECK(!CosmeticRegistry::Instance().Register(a, &err));
  CHECK(err == CosmeticError::BadAnimation);
}

int main() {
  TestRegistryFailClosed();
  TestBuiltins();
  TestLoadoutExclusivity();
  TestLoadoutCanonicalRoundTrip();
  TestWardrobe();
  TestComposeAttach();
  TestTrailingPose();
  TestComposeAttachTrailing();
  TestServiceVelocityInjection();
  TestOffsetCache();
  TestServiceLocalPresent();
  TestServiceNoSeamOrBone();
  TestPresets();
  TestRemoteOwnershipGate();
  TestSyncEd25519HappyPath();
  TestSyncRejections();
  TestSyncHmacAndAlgorithmConfusion();
  TestSyncReplayRefused();
  TestSyncReplayWindowExpires();
  TestSyncRateLimited();
  TestSyncPeerTableBounded();
  TestSyncRevocation();
  TestCosmeticBindings();
  TestLanSyncBindings();
  TestVelocityBinding();
  TestDrawThroughRenderer();
  TestDrawRemoteGated();
  TestAssetCustomAnimation();
  TestAnimatedResolveNotCached();
  TestAnimatorAnimationSource();
  TestPackAnimationFields();
  if (g_fail == 0) printf("test_cosmetic: ALL OK\n");
  return g_fail;
}
