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
  TestServiceLocalPresent();
  TestServiceNoSeamOrBone();
  TestPresets();
  TestRemoteOwnershipGate();
  TestSyncEd25519HappyPath();
  TestSyncRejections();
  TestSyncHmacAndAlgorithmConfusion();
  TestCosmeticBindings();
  TestVelocityBinding();
  if (g_fail == 0) printf("test_cosmetic: ALL OK\n");
  return g_fail;
}
