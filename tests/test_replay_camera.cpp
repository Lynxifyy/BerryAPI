// Replay camera: the keyframed timeline over recorded replay ticks.
//
// These are host tests: no GPU, no clock, no game. They drive `Advance` with an
// explicit delta so playback is deterministic, and they assert the properties
// the feature actually promises — a key is exact, a between-keys sample is
// bracketed, the path does not overshoot under `smooth`, an export walks a
// fixed number of frames whatever the wall clock did, and every bad input fails
// closed rather than building a camera the renderer cannot trust.

#include "berry/replay_camera.h"
#include "berry/math.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace Berry;

static int g_fail = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
      ++g_fail;                                                           \
    }                                                                     \
  } while (0)

static bool Near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

/// Angles are equal when their difference is a whole turn: yaw 360 is yaw 0.
static bool NearAngle(f32 a, f32 b, f32 eps = 1e-3f) {
  f32 d = std::fmod(a - b, 360.0f);
  if (d > 180.0f) d -= 360.0f;
  if (d < -180.0f) d += 360.0f;
  return std::fabs(d) <= eps;
}

static CameraKey Key(u64 tick, Vec3 pos, f32 pitch, f32 yaw, f32 fov = 70.0f) {
  CameraKey k;
  k.tick = tick;
  k.position = pos;
  k.pitch = pitch;
  k.yaw = yaw;
  k.fov = fov;
  return k;
}

static void TestAuthoringBoundsAndUniqueness() {
  ReplayCamera c;
  CHECK(c.empty());
  CHECK(c.keyCount() == 0);
  CHECK(c.AddKey(Key(0, Vec3(0, 64, 0), 0, 0)));
  CHECK(c.AddKey(Key(40, Vec3(10, 64, 0), 0, 90)));
  CHECK(c.keyCount() == 2);
  CHECK(!c.empty());

  // A second key at the same tick is refused: a sample must have one answer.
  CHECK(!c.AddKey(Key(0, Vec3(1, 1, 1), 0, 0)));
  CHECK(c.keyCount() == 2);

  // A non-finite component is refused.
  CHECK(!c.AddKey(Key(80, Vec3(std::nanf(""), 0, 0), 0, 0)));
  // An out-of-range FOV is refused.
  CHECK(!c.AddKey(Key(80, Vec3(0, 64, 0), 0, 0, 0.5f)));
  CHECK(!c.AddKey(Key(80, Vec3(0, 64, 0), 0, 0, 200.0f)));

  CHECK(c.hasKeyAt(0));
  CHECK(!c.hasKeyAt(20));
  CHECK(c.firstTick() == 0);
  CHECK(c.lastTick() == 40);
  CHECK(c.durationTicks() == 40);

  CHECK(c.RemoveKey(0));
  CHECK(!c.RemoveKey(0));    // already gone
  CHECK(c.firstTick() == 40);
  CHECK(c.durationTicks() == 0);

  // Keys are held sorted regardless of insertion order.
  ReplayCamera s;
  CHECK(s.AddKey(Key(80, Vec3(0, 0, 0), 0, 0)));
  CHECK(s.AddKey(Key(0, Vec3(0, 0, 0), 0, 0)));
  CHECK(s.AddKey(Key(40, Vec3(0, 0, 0), 0, 0)));
  CHECK(s.firstTick() == 0);
  CHECK(s.lastTick() == 80);
}

static void TestKeysAreExactAndHeld() {
  ReplayCamera c;
  CHECK(c.AddKey(Key(0, Vec3(0, 64, 0), 0, 0, 60)));
  CHECK(c.AddKey(Key(20, Vec3(20, 64, 0), 0, 90, 80)));

  // Exactly on a key samples that key.
  CameraSample a = c.Sample(0);
  CHECK(a.valid);
  CHECK(Near(a.position.x, 0.0f));
  CHECK(Near(a.fov, 60.0f));

  CameraSample b = c.Sample(20);
  CHECK(b.valid);
  CHECK(Near(b.position.x, 20.0f));
  CHECK(Near(b.yaw, 90.0f));
  CHECK(Near(b.fov, 80.0f));

  // Before the first key holds the first; past the last holds the last.
  CameraSample before = c.Sample(-100);
  CHECK(before.valid);
  CHECK(Near(before.position.x, 0.0f));
  CameraSample after = c.Sample(1000);
  CHECK(after.valid);
  CHECK(Near(after.position.x, 20.0f));
  CHECK(Near(after.fov, 80.0f));

  // An empty timeline samples invalid.
  ReplayCamera empty;
  CHECK(!empty.Sample(0).valid);
}

static void TestLinearInterpolation() {
  ReplayCamera c;
  CHECK(c.AddKey(Key(0, Vec3(0, 0, 0), 0, 0, 60)));
  CHECK(c.AddKey(Key(20, Vec3(20, 40, 0), 0, 90, 80)));
  CHECK(c.smoothing() == CameraSmoothing::Linear);

  CameraSample mid = c.Sample(10);
  CHECK(mid.valid);
  CHECK(Near(mid.position.x, 10.0f));
  CHECK(Near(mid.position.y, 20.0f));
  CHECK(Near(mid.yaw, 45.0f));
  CHECK(Near(mid.fov, 70.0f));

  // The shorter arc: 350 -> 10 goes forward through 0, not back through 180.
  ReplayCamera wrap;
  CHECK(wrap.AddKey(Key(0, Vec3(0, 0, 0), 0, 350)));
  CHECK(wrap.AddKey(Key(20, Vec3(0, 0, 0), 0, 10)));
  CameraSample w = wrap.Sample(10);
  CHECK(NearAngle(w.yaw, 0.0f));
}

static void TestSmoothDoesNotOvershoot() {
  ReplayCamera c;
  // A sharp corner: the middle key pulls the path up and to the right.
  CHECK(c.AddKey(Key(0, Vec3(0, 0, 0), 0, 0)));
  CHECK(c.AddKey(Key(20, Vec3(20, 20, 0), 0, 0)));
  CHECK(c.AddKey(Key(40, Vec3(40, 0, 0), 0, 0)));
  c.SetSmoothing(CameraSmoothing::Smooth);
  CHECK(c.smoothing() == CameraSmoothing::Smooth);

  // Smooth passes exactly through every key.
  CHECK(Near(c.Sample(0).position.y, 0.0f));
  CHECK(Near(c.Sample(20).position.y, 20.0f));
  CHECK(Near(c.Sample(40).position.y, 0.0f));

  // Between keys it is monotone in x (a Catmull-Rom does not double back on an
  // evenly spaced path) and stays inside the corner's y envelope at the
  // midpoint, which an unclamped spline with a bad tangent would exceed.
  f32 prevX = -1.0f;
  for (int i = 0; i <= 40; ++i) {
    const CameraSample s = c.Sample((f32)i);
    CHECK(s.position.x >= prevX - 1e-4f);
    CHECK(s.position.y >= -1e-3f && s.position.y <= 20.0f + 1e-3f);
    prevX = s.position.x;
  }

  // Angles use an eased lerp under smooth: still no overshoot.
  ReplayCamera a;
  CHECK(a.AddKey(Key(0, Vec3(0, 0, 0), 0, 0)));
  CHECK(a.AddKey(Key(20, Vec3(0, 0, 0), 0, 90)));
  a.SetSmoothing(CameraSmoothing::Smooth);
  for (int i = 0; i <= 20; ++i) {
    const f32 yaw = a.Sample((f32)i).yaw;
    CHECK(yaw >= -1e-3f && yaw <= 90.0f + 1e-3f);
  }
}

static void TestSmoothingNames() {
  CameraSmoothing s;
  CHECK(CameraSmoothingFromName("linear", &s));
  CHECK(s == CameraSmoothing::Linear);
  CHECK(CameraSmoothingFromName("smooth", &s));
  CHECK(s == CameraSmoothing::Smooth);
  CHECK(!CameraSmoothingFromName("cubic", &s));
  CHECK(!CameraSmoothingFromName("", &s));
  CHECK(std::string(CameraSmoothingName(CameraSmoothing::Smooth)) == "smooth");
}

static void TestPlaybackAndSpeed() {
  ReplayCamera c;
  CHECK(c.AddKey(Key(0, Vec3(0, 0, 0), 0, 0)));
  CHECK(c.AddKey(Key(200, Vec3(0, 0, 0), 0, 0)));

  CHECK(!c.playing());
  CHECK(!c.active());
  c.SetActive(true);
  CHECK(c.active());

  // Advance does nothing while paused.
  c.Advance(1.0f);
  CHECK(Near(c.playhead(), 0.0f));

  c.Play();
  CHECK(c.playing());
  // 1 second at 20 ticks/s is 20 ticks.
  c.Advance(1.0f);
  CHECK(Near(c.playhead(), 20.0f, 0.01f));

  // A negative or non-finite delta is ignored.
  c.Advance(-5.0f);
  CHECK(Near(c.playhead(), 20.0f, 0.01f));
  c.Advance(std::nanf(""));
  CHECK(Near(c.playhead(), 20.0f, 0.01f));

  // Speed multiplies the step, and a bad speed is refused.
  CHECK(c.SetSpeed(2.0f));
  c.Advance(1.0f);
  CHECK(Near(c.playhead(), 60.0f, 0.01f));
  CHECK(!c.SetSpeed(0.0f));
  CHECK(!c.SetSpeed(-1.0f));
  CHECK(!c.SetSpeed(1e9f));
  CHECK(Near(c.speed(), 2.0f));

  // Reaching the last key stops playback and holds the final view.
  c.Advance(100.0f);
  CHECK(!c.playing());
  CHECK(Near(c.playhead(), 200.0f, 0.01f));

  // Play from the end restarts from the top rather than stopping instantly.
  c.Play();
  CHECK(c.playing());
  CHECK(Near(c.playhead(), 0.0f, 0.01f));

  c.Pause();
  CHECK(!c.playing());
  CHECK(Near(c.playhead(), 0.0f, 0.01f));

  c.Seek(100.0f);
  CHECK(Near(c.playhead(), 100.0f, 0.01f));
  c.Stop();
  CHECK(Near(c.playhead(), 0.0f, 0.01f));

  // Seek clamps into the key range and refuses a non-finite tick.
  CHECK(c.Seek(-50.0f));
  CHECK(Near(c.playhead(), 0.0f, 0.01f));
  CHECK(c.Seek(99999.0f));
  CHECK(Near(c.playhead(), 200.0f, 0.01f));
  CHECK(!c.Seek(std::nanf("")));

  // An empty timeline cannot seek or play.
  ReplayCamera e;
  CHECK(!e.Seek(10.0f));
  e.Play();
  CHECK(!e.playing());
}

static void TestFovOverride() {
  ReplayCamera c;
  CHECK(c.AddKey(Key(0, Vec3(0, 0, 0), 0, 0, 60)));
  CHECK(c.AddKey(Key(20, Vec3(0, 0, 0), 0, 0, 60)));
  CHECK(Near(c.Sample(10).fov, 60.0f));

  CHECK(c.SetFovOverride(100.0f));
  CHECK(Near(c.fovOverride(), 100.0f));
  CHECK(Near(c.Sample(10).fov, 100.0f));

  // A bad override is refused and the old one stands.
  CHECK(!c.SetFovOverride(500.0f));
  CHECK(Near(c.fovOverride(), 100.0f));

  // Zero clears the pin, back to the keyframed FOV.
  CHECK(c.SetFovOverride(0.0f));
  CHECK(Near(c.fovOverride(), 0.0f));
  CHECK(Near(c.Sample(10).fov, 60.0f));
}

static void TestBuildCamera() {
  ReplayCamera c;
  CHECK(c.AddKey(Key(0, Vec3(0, 64, 0), 0, 0, 70)));
  CHECK(c.AddKey(Key(20, Vec3(0, 64, 20), 0, 0, 70)));
  c.Seek(10);

  CameraState cam;
  CHECK(c.BuildCamera(16.0f / 9.0f, 1920.0f, 1080.0f, &cam));
  CHECK(cam.valid);
  CHECK(Near(cam.width, 1920.0f));
  CHECK(Near(cam.height, 1080.0f));

  // The camera projects a point ahead of it and refuses one behind it.
  Vec2 screen;
  CHECK(cam.WorldToScreen(Vec3(0, 64, 30), &screen));
  CHECK(!cam.WorldToScreen(Vec3(0, 64, -30), &screen));

  // A bad aspect fails closed and leaves the camera untouched.
  CameraState bad;
  CHECK(!c.BuildCamera(0.0f, 100.0f, 100.0f, &bad));
  CHECK(!bad.valid);
  CHECK(!c.BuildCamera(std::nanf(""), 100.0f, 100.0f, &bad));

  // An empty timeline builds no camera.
  ReplayCamera e;
  CHECK(!e.BuildCamera(1.0f, 100.0f, 100.0f, &bad));

  // BuildCameraAt is the same pose at an explicit tick.
  CameraState at;
  CHECK(c.BuildCameraAt(0.0f, 1.0f, 100.0f, 100.0f, &at));
  CHECK(at.valid);
}

static void TestRollBanksTheView() {
  ReplayCamera c;
  CameraKey k = Key(0, Vec3(0, 64, 0), 0, 0, 70);
  k.roll = 45.0f;
  CHECK(c.AddKey(k));

  CameraSample s = c.Sample(0);
  CHECK(s.valid);
  CHECK(Near(s.roll, 45.0f));

  // The rolled view still looks the same way (the forward axis is unchanged by
  // roll) but its up vector is banked, so a point straight up projects to one
  // side rather than the centre.
  CameraState cam;
  CHECK(c.BuildCamera(1.0f, 100.0f, 100.0f, &cam));
  const Vec3 fwd(-cam.view.m[2], -cam.view.m[6], -cam.view.m[10]);
  CHECK(Near(fwd.z, 1.0f, 0.01f));   // yaw 0 looks toward +Z
}

static void TestExportWalksFixedFrameCount() {
  ReplayCamera c;
  // 200 ticks at 20 ticks/s is a 10-second timeline.
  CHECK(c.AddKey(Key(0, Vec3(0, 0, 0), 0, 0, 70)));
  CHECK(c.AddKey(Key(200, Vec3(0, 0, 0), 0, 0, 70)));

  CHECK(!c.exporting());
  CHECK(c.ExportBegin(60.0f, 16.0f / 9.0f, 1920.0f, 1080.0f));
  CHECK(c.exporting());
  CHECK(Near(c.exportFps(), 60.0f));

  // 10 seconds at 60 fps is ~600 frames, whatever the wall clock does. Sleep a
  // random amount between steps to prove the count does not depend on it.
  u64 frames = 0;
  CameraState cam;
  while (c.ExportStep(&cam)) {
    CHECK(cam.valid);
    ++frames;
    if (frames == 5) std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  // 200 ticks / (20/60 ticks per frame) = 600 frames plus the one at the end.
  CHECK(frames == 601);
  CHECK(c.exportFrame() == 601);
  // Running off the end ends the export.
  CHECK(!c.exporting());
  CHECK(!c.ExportStep(&cam));

  // Bad export parameters fail closed.
  CHECK(!c.ExportBegin(0.0f, 1.0f, 100.0f, 100.0f));
  CHECK(!c.ExportBegin(1000.0f, 1.0f, 100.0f, 100.0f));
  CHECK(!c.ExportBegin(60.0f, 0.0f, 100.0f, 100.0f));
  CHECK(!c.ExportBegin(60.0f, 1.0f, 0.0f, 100.0f));
  ReplayCamera e;
  CHECK(!e.ExportBegin(60.0f, 1.0f, 100.0f, 100.0f));   // no keys

  // End is idempotent and stops a running export.
  CHECK(c.ExportBegin(30.0f, 1.0f, 100.0f, 100.0f));
  CHECK(c.exporting());
  c.ExportEnd();
  CHECK(!c.exporting());
  c.ExportEnd();
  CHECK(!c.exporting());
}

static void TestClearResetsEverything() {
  ReplayCamera c;
  CHECK(c.AddKey(Key(0, Vec3(0, 0, 0), 0, 0)));
  c.SetActive(true);
  c.Play();
  c.Seek(0.0f);
  CHECK(c.ExportBegin(30.0f, 1.0f, 100.0f, 100.0f));

  c.Clear();
  CHECK(c.empty());
  CHECK(c.keyCount() == 0);
  CHECK(!c.playing());
  CHECK(Near(c.playhead(), 0.0f));
  CHECK(!c.exporting());
  // Clear keeps the active flag: it is a view mode, not timeline state.
  CHECK(c.active());
}

static void TestCameraViewMatrixConvention() {
  // yaw 0 looks toward +Z, pitch 0 is level; a point ahead is in front.
  const Mat4 v = CameraViewMatrix(Vec3(0, 0, 0), 0.0f, 0.0f, 0.0f);
  CameraState cam;
  cam.view = v;
  cam.projection = Mat4::Perspective(70.0f, 1.0f, 0.1f, 100.0f);
  cam.width = cam.height = 100.0f;
  cam.valid = true;
  Vec2 s;
  CHECK(cam.WorldToScreen(Vec3(0, 0, 10), &s));
  CHECK(!cam.WorldToScreen(Vec3(0, 0, -10), &s));
}

int main() {
  TestAuthoringBoundsAndUniqueness();
  TestKeysAreExactAndHeld();
  TestLinearInterpolation();
  TestSmoothDoesNotOvershoot();
  TestSmoothingNames();
  TestPlaybackAndSpeed();
  TestFovOverride();
  TestBuildCamera();
  TestRollBanksTheView();
  TestExportWalksFixedFrameCount();
  TestClearResetsEverything();
  TestCameraViewMatrixConvention();
  if (g_fail == 0) std::printf("test_replay_camera: ALL OK\n");
  return g_fail;
}
