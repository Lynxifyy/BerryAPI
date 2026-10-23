// Host tests for the client-side animator (Phase 6a).
#include "berry/animator.h"

#include "berry/cosmetic.h"
#include "shim_level.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static bool Near(f32 a, f32 b, f32 eps = 1e-4f) {
  const f32 d = a - b;
  return (d < 0 ? -d : d) <= eps;
}

static void TestAuthoringBounds() {
  Animator a;
  CHECK(a.clipCount() == 0);

  // A clip needs a name and a positive, finite duration.
  CHECK(!a.AddClip("", 1.0f, true));
  CHECK(!a.AddClip("bad", 0.0f, true));
  CHECK(!a.AddClip("bad", -1.0f, true));
  CHECK(!a.AddClip("bad", std::nanf(""), true));
  CHECK(a.AddClip("idle", 2.0f, true));
  CHECK(a.clipCount() == 1);
  // A duplicate name is refused.
  CHECK(!a.AddClip("idle", 2.0f, true));

  // A name at the cap is refused.
  CHECK(!a.AddClip(std::string(Animator::kMaxNameLen + 1, 'x'), 1.0f, true));

  // Keys need a real bone, an in-range time, and a positive scale.
  CHECK(!a.AddKey("idle", Bone::None, 0.0f, Vec3::Zero(), Vec3::Zero(), 1.0f));
  CHECK(!a.AddKey("idle", Bone::Head, -1.0f, Vec3::Zero(), Vec3::Zero(), 1.0f));
  CHECK(!a.AddKey("idle", Bone::Head, 3.0f, Vec3::Zero(), Vec3::Zero(), 1.0f));
  CHECK(!a.AddKey("idle", Bone::Head, 0.0f, Vec3::Zero(), Vec3::Zero(), 0.0f));
  CHECK(!a.AddKey("missing", Bone::Head, 0.0f, Vec3::Zero(), Vec3::Zero(), 1.0f));
  CHECK(a.AddKey("idle", Bone::Head, 0.0f, Vec3::Zero(), Vec3::Zero(), 1.0f));
  CHECK(a.AddKey("idle", Bone::Head, 2.0f, Vec3(0, 1, 0), Vec3::Zero(), 2.0f));
  CHECK(a.keyCount("idle") == 2);

  // The clip cap holds.
  for (size_t i = 0; i < Animator::kMaxClips + 5; ++i) {
    a.AddClip("c" + std::to_string(i), 1.0f, true);
  }
  CHECK(a.clipCount() <= Animator::kMaxClips);
}

static void TestSamplingInterpolates() {
  Animator a;
  CHECK(a.AddClip("swing", 1.0f, false));
  CHECK(a.AddKey("swing", Bone::RightArm, 0.0f, Vec3(0, 0, 0), Vec3(0, 0, 0), 1.0f));
  CHECK(a.AddKey("swing", Bone::RightArm, 1.0f, Vec3(0, 2, 0), Vec3(0, 90, 0), 3.0f));
  CHECK(a.Play("swing"));   // Sample works against the selected clip

  // Before the first key: hold it.
  Animator::BonePose p = a.Sample(Bone::RightArm, -5.0f);
  CHECK(p.valid);
  CHECK(Near(p.position.y, 0.0f));

  // Halfway: the lerp.
  p = a.Sample(Bone::RightArm, 0.5f);
  CHECK(p.valid);
  CHECK(Near(p.position.y, 1.0f));
  CHECK(Near(p.rotation.y, 45.0f));
  CHECK(Near(p.scale, 2.0f));

  // Past the last key: hold it.
  p = a.Sample(Bone::RightArm, 5.0f);
  CHECK(p.valid);
  CHECK(Near(p.position.y, 2.0f));

  // A bone with no keys samples invalid, not a fabricated pose.
  CHECK(!a.Sample(Bone::LeftLeg, 0.5f).valid);
}

static void TestPlaybackLoopingAndOneShot() {
  Animator a;
  CHECK(a.AddClip("loop", 1.0f, true));
  CHECK(a.AddClip("once", 1.0f, false));
  CHECK(a.AddKey("loop", Bone::Head, 0.0f, Vec3::Zero(), Vec3::Zero(), 1.0f));
  CHECK(a.AddKey("loop", Bone::Head, 1.0f, Vec3(1, 0, 0), Vec3::Zero(), 1.0f));
  CHECK(a.AddKey("once", Bone::Head, 0.0f, Vec3::Zero(), Vec3::Zero(), 1.0f));
  CHECK(a.AddKey("once", Bone::Head, 1.0f, Vec3(1, 0, 0), Vec3::Zero(), 1.0f));

  // Playing an unknown clip fails closed.
  CHECK(!a.Play("nope"));

  CHECK(a.Play("loop"));
  CHECK(a.playing());
  CHECK(a.currentClip() == "loop");
  CHECK(Near(a.time(), 0.0f));

  // Advancing a looping clip wraps.
  a.Advance(1.25f);
  CHECK(a.playing());
  CHECK(Near(a.time(), 0.25f));
  // A big delta stays bounded.
  a.Advance(1000.0f);
  CHECK(a.time() >= 0.0f && a.time() < 1.0f);

  // A one-shot stops at its end.
  CHECK(a.Play("once"));
  a.Advance(2.0f);
  CHECK(!a.playing());
  CHECK(Near(a.time(), 1.0f));

  // A negative or non-finite delta is ignored.
  CHECK(a.Play("loop"));
  a.Advance(-1.0f);
  CHECK(Near(a.time(), 0.0f));
  a.Advance(std::nanf(""));
  CHECK(Near(a.time(), 0.0f));

  // Seek wraps a looping clip and clamps a one-shot.
  CHECK(a.Seek(2.5f));
  CHECK(Near(a.time(), 0.5f));
  CHECK(a.Play("once"));
  CHECK(a.Seek(9.0f));
  CHECK(Near(a.time(), 1.0f));

  // Stop leaves the time; Reset rewinds.
  a.Stop();
  CHECK(!a.playing());
  a.Reset();
  CHECK(Near(a.time(), 0.0f));
}

static void TestSamplePose() {
  Animator a;
  CHECK(a.AddClip("pose", 1.0f, true));
  CHECK(a.AddKey("pose", Bone::Head, 0.0f, Vec3(0, 1, 0), Vec3::Zero(), 1.0f));
  CHECK(a.AddKey("pose", Bone::Head, 1.0f, Vec3(0, 2, 0), Vec3::Zero(), 1.0f));
  CHECK(a.AddKey("pose", Bone::LeftArm, 0.0f, Vec3(1, 0, 0), Vec3::Zero(), 1.0f));
  CHECK(a.Play("pose"));

  const auto pose = a.SamplePose();
  CHECK(pose.size() == 2);   // one entry per distinct bone, not per key

  // SampleNow tracks the play time.
  a.Seek(0.5f);
  Animator::BonePose head = a.SampleNow(Bone::Head);
  CHECK(head.valid);
  CHECK(Near(head.position.y, 1.5f));

  // With no clip selected, the pose is empty.
  Animator b;
  CHECK(b.SamplePose().empty());
}

static void TestSaveLoadRoundTrip() {
  Animator a;
  CHECK(a.AddClip("walk", 1.5f, true));
  CHECK(a.AddKey("walk", Bone::LeftLeg, 0.0f, Vec3(0, 0, 0), Vec3(0, 0, 0), 1.0f));
  CHECK(a.AddKey("walk", Bone::LeftLeg, 1.5f, Vec3(0, 0, 0.5f), Vec3(10, 0, 0), 1.0f));
  CHECK(a.AddKey("walk", Bone::RightLeg, 0.0f, Vec3(0, 0, 0), Vec3(0, 0, 0), 1.0f));

  const std::string json = a.Save();

  Animator b;
  CHECK(b.Load(json));
  CHECK(b.clipCount() == 1);
  CHECK(b.hasClip("walk"));
  CHECK(b.keyCount("walk") == 3);

  // The round trip preserves the sampled pose.
  CHECK(b.Play("walk"));
  CHECK(b.Seek(0.75f));
  Animator::BonePose leg = b.SampleNow(Bone::LeftLeg);
  CHECK(leg.valid);
  CHECK(Near(leg.position.z, 0.25f));
  CHECK(Near(leg.rotation.x, 5.0f));

  // Determinism: saving the reloaded animator yields the same bytes.
  CHECK(b.Save() == json);
}

static void TestLoadRejectsBadDocuments() {
  Animator a;
  CHECK(a.AddClip("keep", 1.0f, true));
  CHECK(a.AddKey("keep", Bone::Head, 0.0f, Vec3::Zero(), Vec3::Zero(), 1.0f));

  // Every one of these must be refused, and the live clips left untouched.
  CHECK(!a.Load("not json"));
  CHECK(!a.Load("{}"));
  CHECK(!a.Load(R"({"format":"berry-animator","version":1})"));
  CHECK(!a.Load(R"({"format":"other","clips":[]})"));
  CHECK(!a.Load(R"({"format":"berry-animator","version":1,"clips":"x"})"));
  // A clip with a bad duration.
  CHECK(!a.Load(R"({"format":"berry-animator","version":1,"clips":[{"name":"c","duration":0,
    "looping":true,"keys":[]}]})"));
  // A key naming an unknown bone.
  CHECK(!a.Load(R"({"format":"berry-animator","version":1,"clips":[{"name":"c","duration":1,
    "looping":true,"keys":[{"bone":"elbow","time":0,"position":[0,0,0],
    "rotation":[0,0,0],"scale":1}]}]})"));
  // A key time past the duration.
  CHECK(!a.Load(R"({"format":"berry-animator","version":1,"clips":[{"name":"c","duration":1,
    "looping":true,"keys":[{"bone":"head","time":2,"position":[0,0,0],
    "rotation":[0,0,0],"scale":1}]}]})"));
  // A missing version, and a version this build does not write.
  CHECK(!a.Load(R"({"format":"berry-animator","clips":[]})"));
  CHECK(!a.Load(R"({"format":"berry-animator","version":2,"clips":[]})"));
  // A duplicate clip name: FindLocked would silently pick the first, so the
  // second would be unreachable.
  CHECK(!a.Load(R"({"format":"berry-animator","version":1,"clips":[
    {"name":"c","duration":1,"looping":true,"keys":[]},
    {"name":"c","duration":1,"looping":true,"keys":[]}]})"));

  // The live clips survived every rejection.
  CHECK(a.clipCount() == 1);
  CHECK(a.hasClip("keep"));
  CHECK(a.keyCount("keep") == 1);
}

static void TestLoadRejectsNonIntegerVersion() {
  Animator a;
  // A float version is not the integer this build writes, so it is refused
  // rather than truncated.
  CHECK(!a.Load(R"({"format":"berry-animator","version":1.5,"clips":[]})"));
  CHECK(a.clipCount() == 0);
}

static void TestRemoveClearAndCaps() {
  Animator a;
  CHECK(a.AddClip("a", 1.0f, true));
  CHECK(a.AddClip("b", 1.0f, true));
  CHECK(a.AddKey("a", Bone::Head, 0.0f, Vec3::Zero(), Vec3::Zero(), 1.0f));

  CHECK(a.ClearClip("a"));
  CHECK(a.keyCount("a") == 0);
  CHECK(a.hasClip("a"));          // the clip stays, only the keys go
  CHECK(!a.ClearClip("missing"));

  // Removing the playing clip stops playback.
  CHECK(a.Play("a"));
  CHECK(a.RemoveClip("a"));
  CHECK(!a.playing());
  CHECK(a.currentClip().empty());
  CHECK(!a.RemoveClip("a"));

  a.Clear();
  CHECK(a.clipCount() == 0);
  CHECK(!a.playing());
}

static void TestAnimatedSkeletonSourceDrivesCosmetics() {
  // A base rig at a known spot, and a clip that lifts the head bone.
  Test::ShimSkeleton base;
  Animator a;
  CHECK(a.AddClip("nod", 1.0f, false));   // one-shot so Seek(1.0) clamps to the end
  CHECK(a.AddKey("nod", Bone::Head, 0.0f, Vec3(0, 0, 0), Vec3::Zero(), 1.0f));
  CHECK(a.AddKey("nod", Bone::Head, 1.0f, Vec3(0, 0.5f, 0), Vec3::Zero(), 1.0f));

  AnimatedSkeletonSource seam(&base, &a);
  seam.SetTarget(EntityId{7, 1});

  // The clip's offset is added on top of the base bone transform.
  a.Play("nod");
  a.Seek(0.5f);
  CosmeticBoneTransform t;
  CHECK(seam.BoneTransform(EntityId{7, 1}, Bone::Head, &t));
  CHECK(t.valid);
  CHECK(Near(t.position.y, 64.0f + 0.25f));
  // A bone the clip does not key passes through unchanged.
  CHECK(seam.BoneTransform(EntityId{7, 1}, Bone::LeftLeg, &t));
  CHECK(Near(t.position.y, 64.0f));

  // A non-target entity is untouched.
  CHECK(seam.BoneTransform(EntityId{9, 1}, Bone::Head, &t));
  CHECK(Near(t.position.y, 64.0f));

  // The cosmetic service resolves through this seam, so the clip moves the
  // rendered cosmetic: a head-mounted asset sits higher mid-clip.
  CosmeticRegistry::Instance().Clear();
  CosmeticRegistry::Instance().RegisterBuiltins();
  CosmeticService svc;
  svc.SetSkeletonSource(&seam);
  CHECK(svc.Equip("builtin.hat.top"));

  const auto at = [&](f32 time) {
    a.Seek(time);
    svc.ClearResolveCache();
    const auto items = svc.Resolve(EntityId{7, 1}, svc.localLoadout());
    return items.empty() ? Vec3::Zero() : items.front().position;
  };
  const Vec3 low = at(0.0f);
  const Vec3 high = at(1.0f);
  CHECK(Near(high.y - low.y, 0.5f));

  // While playing, the seam reports no revision, so the resolve cache is off.
  u64 rev = 0;
  CHECK(!seam.PoseRevision(EntityId{7, 1}, &rev));

  // Stopped, the revision falls through to the base seam.
  a.Stop();
  CHECK(seam.PoseRevision(EntityId{7, 1}, &rev));
  CHECK(rev == base.revision);
}

int main() {
  TestAuthoringBounds();
  TestSamplingInterpolates();
  TestPlaybackLoopingAndOneShot();
  TestSamplePose();
  TestSaveLoadRoundTrip();
  TestLoadRejectsBadDocuments();
  TestLoadRejectsNonIntegerVersion();
  TestRemoveClearAndCaps();
  TestAnimatedSkeletonSourceDrivesCosmetics();
  if (g_fail == 0) printf("test_animator: ALL OK\n");
  return g_fail;
}
