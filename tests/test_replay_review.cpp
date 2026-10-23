// Host tests for the fight-review overlay (berry/replay_review.h).
//
// The overlay turns a reconstructed replay tick into draw calls through the one
// render path. These tests build a replay, play it, and assert on the exact
// primitives the Renderer issued (via the capturing seam), so the overlay is
// proven to draw what it says without a GPU.
#include "berry/replay_capture.h"
#include "berry/replay_format.h"
#include "berry/replay_review.h"
#include "berry/replay_world.h"
#include "berry/render.h"

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

ReplayFile BuildReplay() {
  ReplayCapture cap;
  CaptureConfig cfg;
  cfg.keyframeTicks = 10;
  cap.SetConfig(cfg);
  cap.Start();
  ShimLevel seam;
  EntityId player = Id(1);
  seam.AddEntity(player, MakeSnap(1, EntityType::Player, 0, 64, 0, "Me"));
  for (u64 tick = 0; tick <= 40; ++tick) {
    seam.AddEntity(Id(2), MakeSnap(2, EntityType::Mob, 3.0f + (f32)tick * 0.1f, 64, 5, "Zombie"));
    if (tick % 10 == 0) cap.CaptureEvent("combat", "t" + std::to_string(tick), tick);
    cap.CaptureTick(tick, seam, {}, player);
  }
  cap.Stop();
  return cap.Snapshot();
}

/// A camera looking down -Z from above, so world points project to screen.
CameraState MakeCamera(f32 w, f32 h) {
  CameraState cam;
  cam.view = Mat4::LookAt(Vec3(0, 64, -10), Vec3(0, 64, 0), Vec3(0, 1, 0));
  cam.projection = Mat4::Perspective(70.0f, w / h, 0.1f, 1000.0f);
  cam.width = w;
  cam.height = h;
  cam.valid = true;
  return cam;
}

//--------------------------------------------------------------------------

void TestPlaybackAdvances() {
  ReplayFile f = BuildReplay();
  ReplayWorld world(&f);
  ReplayReview review;
  review.SetActive(true);
  CHECK(!review.playing());
  CHECK(review.Play(&world));
  CHECK(review.playing());
  CHECK(review.tick() == world.firstTick());

  // A quarter-second step at 20 tps advances 5 ticks. (A single call is
  // clamped to 0.25s, so a stall cannot fast-forward the replay.)
  const u64 t = review.Advance(0.25f);
  CHECK(t == 5);
  CHECK(review.tick() == 5);
  CHECK(review.Advance(0.25f) == 10);
  CHECK(review.tick() == 10);

  // A bad delta is ignored, not applied.
  CHECK(review.Advance(-1.0f) == 10);
  CHECK(review.Advance(0.0f) == 10);

  // Playing to the end holds the final frame and stops.
  for (int i = 0; i < 20; ++i) review.Advance(0.25f);
  CHECK(review.tick() == world.lastTick());
  CHECK(!review.playing());

  review.Pause();
  CHECK(!review.playing());
  review.Stop();
  CHECK(!review.activeWorld());
}

void TestSpeed() {
  ReplayFile f = BuildReplay();
  ReplayWorld world(&f);
  ReplayReview review;
  review.Play(&world);
  CHECK(review.speed() == 1.0f);
  CHECK(review.SetSpeed(2.0f));
  review.Advance(0.25f);   // 5 ticks at 1x, 10 at 2x
  CHECK(review.tick() == 10);
  // A bad speed is refused and leaves the value.
  CHECK(!review.SetSpeed(0.0f));
  CHECK(!review.SetSpeed(-1.0f));
  CHECK(!review.SetSpeed(1e9f));
  CHECK(review.speed() == 2.0f);
}

void TestPresentDrawsEntities() {
  ReplayFile f = BuildReplay();
  ReplayWorld world(&f);
  ReplayReview review;
  review.SetActive(true);
  review.SetLocalPlayer(Id(1));
  review.Play(&world);
  review.Seek(20);

  Renderer r;
  Berry::Test::CaptureRenderSeam seam;
  r.BindSeam(&seam);
  r.BeginFrame(Vec2(1280, 720));

  const CameraState cam = MakeCamera(1280, 720);
  const size_t drawn = review.Present(cam, r, Vec2(1280, 720));
  r.EndFrame();

  // Both entities are in front of the camera and should project.
  CHECK(drawn == 2);
  const auto items = review.lastItems();
  CHECK(items.size() == 2);
  // Boxes: 2 rects. Names: 2 texts. Local->other line: 1. Markers: 5 notches.
  CHECK(seam.rects == 2);
  CHECK(seam.texts == 2);
  CHECK(seam.lines == 1 + 5);
  bool sawZombie = false, sawMe = false;
  for (const auto& tx : seam.textContents) {
    if (tx == "Zombie") sawZombie = true;
    if (tx == "Me") sawMe = true;
  }
  CHECK(sawZombie);
  CHECK(sawMe);
}

void TestInactiveDrawsNothing() {
  ReplayFile f = BuildReplay();
  ReplayWorld world(&f);
  ReplayReview review;
  review.Play(&world);   // playing, but not active
  review.Seek(10);

  Renderer r;
  Berry::Test::CaptureRenderSeam seam;
  r.BindSeam(&seam);
  r.BeginFrame(Vec2(800, 600));
  CHECK(review.Present(MakeCamera(800, 600), r, Vec2(800, 600)) == 0);
  r.EndFrame();
  CHECK(seam.rects == 0);
  CHECK(seam.texts == 0);
  CHECK(seam.lines == 0);
}

void TestStyleToggles() {
  ReplayFile f = BuildReplay();
  ReplayWorld world(&f);
  ReplayReview review;
  review.SetActive(true);
  review.SetLocalPlayer(Id(1));
  review.Play(&world);
  review.Seek(20);

  ReplayReviewStyle st;
  st.names = false;
  st.markers = false;
  st.lineToLocal = false;
  review.SetStyle(st);

  Renderer r;
  Berry::Test::CaptureRenderSeam seam;
  r.BindSeam(&seam);
  r.BeginFrame(Vec2(800, 600));
  CHECK(review.Present(MakeCamera(800, 600), r, Vec2(800, 600)) == 2);
  r.EndFrame();
  CHECK(seam.rects == 2);   // boxes only
  CHECK(seam.texts == 0);
  CHECK(seam.lines == 0);
}

void TestBehindCameraSkipped() {
  ReplayFile f = BuildReplay();
  ReplayWorld world(&f);
  ReplayReview review;
  review.SetActive(true);
  review.Play(&world);
  review.Seek(20);

  // A camera looking away from every entity draws nothing.
  CameraState cam = MakeCamera(800, 600);
  cam.view = Mat4::LookAt(Vec3(0, 64, -10), Vec3(0, 64, -100), Vec3(0, 1, 0));

  Renderer r;
  Berry::Test::CaptureRenderSeam seam;
  r.BindSeam(&seam);
  r.BeginFrame(Vec2(800, 600));
  CHECK(review.Present(cam, r, Vec2(800, 600)) == 0);
  r.EndFrame();
  CHECK(seam.rects == 0);
}

void TestScrub() {
  ReplayFile f = BuildReplay();
  ReplayWorld world(&f);
  ReplayReview review;
  review.Play(&world);
  review.Pause();
  CHECK(review.Seek(35));
  CHECK(review.tick() == 35);
  CHECK(review.Seek(0));
  CHECK(review.tick() == 0);
}

void TestNoWorldPresent() {
  ReplayReview review;
  review.SetActive(true);
  Renderer r;
  Berry::Test::CaptureRenderSeam seam;
  r.BindSeam(&seam);
  r.BeginFrame(Vec2(100, 100));
  CHECK(review.Present(MakeCamera(100, 100), r, Vec2(100, 100)) == 0);
  r.EndFrame();
  CHECK(!review.activeWorld());
  CHECK(!review.Seek(5));
}

}  // namespace

int main() {
  TestPlaybackAdvances();
  TestSpeed();
  TestPresentDrawsEntities();
  TestInactiveDrawsNothing();
  TestStyleToggles();
  TestBehindCameraSkipped();
  TestScrub();
  TestNoWorldPresent();

  if (g_fail == 0) {
    printf("test_replay_review: ALL OK\n");
    return 0;
  }
  printf("test_replay_review: %d FAILURES\n", g_fail);
  return 1;
}
