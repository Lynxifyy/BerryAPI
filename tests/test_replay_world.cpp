// Host tests for replay playback (berry/replay_world.h).
//
// A seek is "find the keyframe at or before the tick, then apply the deltas to
// it". These tests build a replay with capture, then prove a seek to any tick
// reconstructs exactly the state that tick was recorded with — including across
// a keyframe boundary, so a delta is never applied without its anchor.
#include "berry/replay_capture.h"
#include "berry/replay_format.h"
#include "berry/replay_world.h"

#include "shim_level.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;
using Berry::Test::ShimLevel;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

EntitySnapshot MakeSnap(u64 raw, EntityType type, f32 x, f32 y, f32 z,
                        const char* name = "") {
  EntitySnapshot s;
  s.id.raw = raw;
  s.id.runtime = (u16)raw;
  s.type = type;
  s.position = Vec3(x, y, z);
  s.health = 20;
  s.maxHealth = 20;
  s.alive = true;
  s.onGround = true;
  s.name = name;
  return s;
}
EntityId Id(u64 raw) {
  EntityId id;
  id.raw = raw;
  id.runtime = (u16)raw;
  return id;
}

/// Records a deterministic 60-tick replay where entity 2 moves +1 on x each
/// tick and a marker lands every 10 ticks.
ReplayFile BuildReplay() {
  ReplayCapture cap;
  CaptureConfig cfg;
  cfg.keyframeTicks = 10;
  cap.SetConfig(cfg);
  ReplayFingerprint fp;
  fp.buildId = 7;
  fp.label = "test";
  cap.SetFingerprint(fp);
  cap.Start();

  ShimLevel seam;
  EntityId player = Id(1);
  seam.AddEntity(player, MakeSnap(1, EntityType::Player, 0, 64, 0, "Me"));
  LevelTime t;
  t.absolute = 1000;
  t.daylight = 0.5f;
  seam.SetTime(t);

  for (u64 tick = 0; tick <= 60; ++tick) {
    seam.AddEntity(Id(2), MakeSnap(2, EntityType::Mob, (f32)tick, 64, 0, "Zombie"));
    if (tick % 10 == 0) cap.CaptureEvent("mark", "t" + std::to_string(tick), tick);
    cap.CaptureTick(tick, seam, {}, player);
  }
  cap.Stop();
  return cap.Snapshot();
}

//--------------------------------------------------------------------------

void TestSeekReconstructsExactState() {
  ReplayFile f = BuildReplay();
  ReplayWorld world(&f);
  CHECK(world.loaded());
  CHECK(world.firstTick() == 0);
  CHECK(world.lastTick() == 60);

  // Seek to each tick and check entity 2 is exactly where it was recorded.
  for (u64 tick = 0; tick <= 60; ++tick) {
    CHECK(world.Seek(tick));
    CHECK(world.tick() == tick);
    EntitySnapshot s;
    CHECK(world.GetEntity(Id(2), &s));
    CHECK(s.position.x == (f32)tick);
    CHECK(s.position.y == 64.0f);
    // The player is present the whole time.
    CHECK(world.GetEntity(Id(1), &s));
    CHECK(s.name == "Me");
    CHECK(world.AllEntities().size() == 2);
    CHECK(world.GetTime().absolute == 1000);
  }
}

void TestSeekAcrossKeyframeBoundary() {
  ReplayFile f = BuildReplay();
  ReplayWorld world(&f);
  // Seeking backwards across a keyframe must not leave a delta applied on the
  // wrong anchor: tick 5 then tick 25 must both be exact.
  CHECK(world.Seek(5));
  EntitySnapshot s;
  CHECK(world.GetEntity(Id(2), &s));
  CHECK(s.position.x == 5.0f);
  CHECK(world.Seek(25));
  CHECK(world.GetEntity(Id(2), &s));
  CHECK(s.position.x == 25.0f);
  CHECK(world.Seek(0));
  CHECK(world.GetEntity(Id(2), &s));
  CHECK(s.position.x == 0.0f);
}

void TestSeekClamps() {
  ReplayFile f = BuildReplay();
  ReplayWorld world(&f);
  CHECK(world.Seek(9999));
  CHECK(world.tick() == 60);
  EntitySnapshot s;
  CHECK(world.GetEntity(Id(2), &s));
  CHECK(s.position.x == 60.0f);
  // Before the first tick clamps to the first.
  CHECK(world.Seek(0));
  CHECK(world.tick() == 0);
}

void TestEntityLeaveAtSeek() {
  // An entity that appears then leaves must be absent from a seek after it
  // left and present from a seek before.
  ReplayCapture cap;
  CaptureConfig cfg;
  cfg.keyframeTicks = 10;
  cap.SetConfig(cfg);
  cap.Start();
  ShimLevel seam;
  EntityId player = Id(1);
  seam.AddEntity(player, MakeSnap(1, EntityType::Player, 0, 0, 0));
  for (u64 tick = 0; tick <= 20; ++tick) {
    if (tick == 5) seam.AddEntity(Id(9), MakeSnap(9, EntityType::Mob, 3, 0, 0));
    if (tick == 15) seam.RemoveEntity(Id(9));
    cap.CaptureTick(tick, seam, {}, player);
  }
  cap.Stop();
  ReplayFile f = cap.Snapshot();
  ReplayWorld world(&f);

  CHECK(world.Seek(4));
  EntitySnapshot s;
  CHECK(!world.GetEntity(Id(9), &s));   // not yet spawned
  CHECK(world.Seek(10));
  CHECK(world.GetEntity(Id(9), &s));    // present
  CHECK(world.Seek(20));
  CHECK(!world.GetEntity(Id(9), &s));   // gone
}

void TestMarkers() {
  ReplayFile f = BuildReplay();
  ReplayWorld world(&f);
  // Markers at ticks 0,10,20,30,40,50,60.
  CHECK(world.markers().size() == 7);
  CHECK(world.markers().front().tick == 0);
  CHECK(world.markers().front().channel == "mark");
  CHECK(world.markers().front().payload == "t0");
  CHECK(world.markers().back().tick == 60);
  CHECK(world.MarkerCount(0, 20) == 3);
  CHECK(world.MarkerCount(25, 45) == 2);
  CHECK(world.MarkerCount(100, 200) == 0);
}

void TestKeyframeTicks() {
  ReplayFile f = BuildReplay();
  ReplayWorld world(&f);
  // Keyframes every 10 ticks: 0,10,20,30,40,50,60.
  CHECK(world.keyframeTicks().size() == 7);
  CHECK(world.keyframeTicks()[0] == 0);
  CHECK(world.keyframeTicks()[1] == 10);
  CHECK(world.keyframeTicks().back() == 60);
}

void TestQueries() {
  ReplayFile f = BuildReplay();
  ReplayWorld world(&f);
  CHECK(world.Seek(30));
  // Entities in a box around the zombie at x=30.
  Aabb box;
  box.min = Vec3(29, 63, -1);
  box.max = Vec3(31, 65, 1);
  auto inBox = world.EntitiesInAabb(box);
  CHECK(inBox.size() == 1);
  // A radius query around the zombie.
  auto inRadius = world.EntitiesInRadius(Vec3(30, 64, 0), 2.0f);
  CHECK(inRadius.size() == 1);
  // A raycast toward the zombie hits it.
  EntityId hitId;
  f32 hitDist = 0;
  CHECK(world.RaycastEntity(Vec3(28, 64, 0), Vec3(1, 0, 0), 10.0f, &hitId, &hitDist));
  CHECK(hitId.raw == 2);
  // No block data: a block query fails closed.
  CHECK(world.GetBlock(Vec3(0, 64, 0)) == 0);
  Vec3 pos;
  u32 type = 0;
  CHECK(!world.RaycastBlock(Vec3(0, 64, 0), Vec3(1, 0, 0), 10, &pos, &type));
}

void TestEmptyReplay() {
  ReplayFile empty;
  ReplayWorld world(&empty);
  CHECK(!world.loaded());
  CHECK(!world.Seek(0));
  CHECK(world.AllEntities().empty());
  CHECK(world.markers().empty());
}

}  // namespace

int main() {
  TestSeekReconstructsExactState();
  TestSeekAcrossKeyframeBoundary();
  TestSeekClamps();
  TestEntityLeaveAtSeek();
  TestMarkers();
  TestKeyframeTicks();
  TestQueries();
  TestEmptyReplay();

  if (g_fail == 0) {
    printf("test_replay_world: ALL OK\n");
    return 0;
  }
  printf("test_replay_world: %d FAILURES\n", g_fail);
  return 1;
}
