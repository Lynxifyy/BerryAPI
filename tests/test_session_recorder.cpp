// Host tests for the session recorder (Phase J): record & replay driven from
// the tick, and the tape handed to a script.
//
// Covers:
//   * Start/Stop lifecycle: a fresh tape on Start, Stop keeps the tape, and a
//     capture outside a session is a no-op.
//   * CaptureTick: the level time and every seam entity land on the tape, and
//     the tick's world-diff events are recorded as channels the async layer
//     already understands.
//   * Save/Load: a round-trip replays identically, and a rejected document
//     leaves the current tape untouched.
//   * Replay: step/seek/rewind and the empty world before the first frame.
//   * The Engine accessor, and a real module tick driving the capture.
#include "berry/session_recorder.h"

#include "berry/api_bindings.h"
#include "berry/engine.h"
#include "berry/event.h"
#include "berry/script.h"
#include "berry/services.h"
#include "berry/world_query.h"

#include "shim_level.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

EntityId Id(u64 raw, u16 runtime = 0) {
  EntityId id;
  id.raw = raw;
  id.runtime = runtime;
  return id;
}

EntitySnapshot Snap(u64 raw, const Vec3& pos, EntityType type = EntityType::Mob) {
  EntitySnapshot s;
  s.id = Id(raw, (u16)raw);
  s.position = pos;
  s.type = type;
  return s;
}

}  // namespace

//----------------------------------------------------------------------------
// Lifecycle
//----------------------------------------------------------------------------

static void TestLifecycle() {
  SessionRecorder rec;
  CHECK(!rec.recording());
  CHECK(rec.empty());
  CHECK(rec.frames() == 0);

  Berry::Test::ShimLevel level;
  level.AddEntity(Id(1, 1), Snap(1, Vec3(0, 0, 0)));

  // A capture outside a session is a no-op, not a crash and not a frame.
  rec.CaptureTick(1, level);
  CHECK(rec.empty());

  CHECK(rec.Start());
  CHECK(rec.recording());
  // Starting twice is refused, so a second session cannot interleave frames.
  CHECK(!rec.Start());

  rec.CaptureTick(1, level);
  CHECK(rec.frames() == 1);

  CHECK(rec.Stop());
  CHECK(!rec.recording());
  CHECK(!rec.Stop());
  // Stop keeps the tape.
  CHECK(rec.frames() == 1);

  // Restarting clears it.
  CHECK(rec.Start());
  CHECK(rec.frames() == 0);
}

//----------------------------------------------------------------------------
// Capture
//----------------------------------------------------------------------------

static void TestCaptureFromSeam() {
  SessionRecorder rec;
  Berry::Test::ShimLevel level;
  level.AddEntity(Id(1, 1), Snap(1, Vec3(1, 2, 3), EntityType::Player));
  level.AddEntity(Id(2, 2), Snap(2, Vec3(5, 0, 0)));

  CHECK(rec.Start());
  rec.CaptureTick(7, level);
  CHECK(rec.frames() == 1);
  CHECK(!rec.overflowed());

  // The replay answers from the tape with the same entities and time.
  auto seam = rec.ReplaySeamHandle();
  CHECK(seam != nullptr);
  CHECK(std::string(seam->Name()) == "replay");
  CHECK(rec.ReplayStep());
  CHECK(rec.ReplayTick() == 7);
  CHECK(rec.ReplayIndex() == 0);
  CHECK(!rec.ReplayAtEnd());

  EntitySnapshot s;
  CHECK(seam->GetEntity(Id(1, 1), &s));
  CHECK(s.position.x == 1 && s.position.y == 2 && s.position.z == 3);
  CHECK(s.type == EntityType::Player);
  CHECK(seam->AllEntities().size() == 2);
}

static void TestCaptureRecordsDiffEvents() {
  SessionRecorder rec;
  Berry::Test::ShimLevel level;
  level.AddEntity(Id(1, 1), Snap(1, Vec3(0, 0, 0)));

  // A grid tick that sees entity 1 enter and entity 2 update.
  SpatialGrid grid;
  std::vector<WorldDiffEvent> diff = UpdateWorldGrid(grid, level, 1);
  CHECK(rec.Start());
  rec.CaptureTick(1, level, diff);
  CHECK(rec.frames() == 1);

  level.AddEntity(Id(2, 2), Snap(2, Vec3(4, 0, 0)));
  diff = UpdateWorldGrid(grid, level, 2);
  rec.CaptureTick(2, level, diff);

  // The events landed on the tape as channels the async layer understands.
  // Replay the tape through a fresh Recording parsed from the saved document.
  Recording parsed;
  TapeError err = TapeError::Ok;
  std::string detail;
  CHECK(Recording::FromJson(rec.Save(), &parsed, &err, &detail));
  CHECK(parsed.frameCount() == 2);
  // Frame 1: entity 1 entered.
  CHECK(parsed.frames()[0].events.size() == 1);
  CHECK(parsed.frames()[0].events[0].channel == "entityEntered");
  CHECK(parsed.frames()[0].events[0].payload == "1:mob");
  // Frame 2: entity 2 entered and entity 1 did not change.
  CHECK(parsed.frames()[1].events.size() == 1);
  CHECK(parsed.frames()[1].events[0].channel == "entityEntered");
}

//----------------------------------------------------------------------------
// Save / Load
//----------------------------------------------------------------------------

static void TestSaveLoadRoundTrip() {
  SessionRecorder rec;
  Berry::Test::ShimLevel level;
  level.AddEntity(Id(1, 1), Snap(1, Vec3(0, 0, 0)));
  CHECK(rec.Start());
  rec.CaptureTick(1, level);
  level.AddEntity(Id(2, 2), Snap(2, Vec3(9, 0, 0)));
  rec.CaptureTick(2, level);
  CHECK(rec.Stop());

  const std::string doc = rec.Save();
  CHECK(!doc.empty());

  SessionRecorder other;
  TapeError err = TapeError::BadJson;
  std::string detail = "x";
  CHECK(other.Load(doc, &err, &detail));
  CHECK(err == TapeError::Ok);
  CHECK(detail.empty());
  CHECK(other.frames() == 2);
  // Save is deterministic: the loaded tape serialises to the same bytes.
  CHECK(other.Save() == doc);

  // A rejected document leaves the current tape untouched.
  const size_t before = other.frames();
  CHECK(!other.Load("{not json", &err, &detail));
  CHECK(err != TapeError::Ok);
  CHECK(!detail.empty());
  CHECK(other.frames() == before);
}

//----------------------------------------------------------------------------
// Replay controls
//----------------------------------------------------------------------------

static void TestReplayControls() {
  SessionRecorder rec;
  Berry::Test::ShimLevel level;
  level.AddEntity(Id(1, 1), Snap(1, Vec3(0, 0, 0)));
  CHECK(rec.Start());
  rec.CaptureTick(1, level);
  level.AddEntity(Id(1, 1), Snap(1, Vec3(6, 0, 0)));
  rec.CaptureTick(2, level);
  CHECK(rec.Stop());

  auto seam = rec.ReplaySeamHandle();
  // Before the first step the world is empty.
  CHECK(rec.ReplayAtEnd());
  CHECK(rec.ReplayIndex() == -1);
  CHECK(rec.ReplayTick() == 0);
  CHECK(seam->AllEntities().empty());

  CHECK(rec.ReplaySeek(1));
  CHECK(rec.ReplayIndex() == 1);
  CHECK(rec.ReplayTick() == 2);
  EntitySnapshot s;
  CHECK(seam->GetEntity(Id(1, 1), &s));
  CHECK(s.position.x == 6);

  // Seek out of range is refused and leaves the position alone.
  CHECK(!rec.ReplaySeek(99));
  CHECK(rec.ReplayIndex() == 1);

  rec.ReplayRewind();
  CHECK(rec.ReplayIndex() == -1);
  CHECK(rec.ReplayAtEnd());

  CHECK(rec.ReplayStep());
  CHECK(rec.ReplayIndex() == 0);
  CHECK(rec.ReplayStep());
  CHECK(rec.ReplayIndex() == 1);
  // Past the last frame: at the end, and no frame is current.
  CHECK(!rec.ReplayStep());
  CHECK(rec.ReplayAtEnd());
}

//----------------------------------------------------------------------------
// Engine + module tick
//----------------------------------------------------------------------------

static void TestEngineDrivesCapture() {
  Engine& eng = Engine::Instance();
  CHECK(eng.Bootstrap());
  CHECK(eng.Attach());

  WorldHandle* world = WorldFrom(eng.Modules());
  CHECK(world != nullptr);
  auto shim = std::make_shared<Berry::Test::ShimLevel>();
  shim->AddEntity(Id(1, 1), Snap(1, Vec3(0, 0, 0), EntityType::Player));
  world->SetSeam(shim);

  // Not recording: a tick leaves the tape empty.
  eng.OnGameTick(1);
  CHECK(eng.Recorder().empty());

  // Recording: the module tick feeds the recorder without any extra call.
  CHECK(eng.Recorder().Start());
  eng.OnGameTick(2);
  eng.OnGameTick(3);
  CHECK(eng.Recorder().frames() == 2);
  CHECK(eng.Recorder().Stop());

  // The recorded stream replays through the seam.
  auto seam = eng.Recorder().ReplaySeamHandle();
  CHECK(eng.Recorder().ReplayStep());
  CHECK(eng.Recorder().ReplayTick() == 2);
  EntitySnapshot s;
  CHECK(seam->GetEntity(Id(1, 1), &s));
  CHECK(s.type == EntityType::Player);

  world->SetSeam(nullptr);
  eng.Detach();
}

//----------------------------------------------------------------------------
// Bindings
//----------------------------------------------------------------------------

namespace {

ScriptStatus CallRecordBinding(ScriptBindings& bindings, const std::string& name,
                               const std::vector<ScriptValue>& args,
                               const std::vector<std::string>& grants,
                               ScriptValue* out) {
  CapabilitySet s;
  for (const auto& c : grants) s.Add(c);
  ScriptHost host("record-test", s, nullptr);
  host.SetBindings(&bindings);
  return host.CallBinding(name, args, out);
}

}  // namespace

static void TestRecordBindings() {
  SessionRecorder rec;
  ScriptApi::Services services;
  services.recorder = &rec;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  // Granted: start, capture via the recorder directly, then read the tape.
  CHECK(CallRecordBinding(bindings, "record.start", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsBool() && out.Truthy());
  CHECK(CallRecordBinding(bindings, "record.recording", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());

  Berry::Test::ShimLevel level;
  level.AddEntity(Id(1, 1), Snap(1, Vec3(0, 0, 0)));
  rec.CaptureTick(1, level);

  CHECK(CallRecordBinding(bindings, "record.frames", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsNumber() && out.AsNumber() == 1.0);

  CHECK(CallRecordBinding(bindings, "record.save", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsString() && !out.AsString().empty());
  const std::string tape = out.AsString();

  CHECK(CallRecordBinding(bindings, "record.stop", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());

  // replay_seek refuses a negative index rather than casting it.
  CHECK(CallRecordBinding(bindings, "record.replay_seek",
                          {ScriptValue::Num(-1.0)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsBool() && !out.Truthy());
  CHECK(CallRecordBinding(bindings, "record.replay_index", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == -1.0);

  // Load the saved tape back and step it.
  CHECK(CallRecordBinding(bindings, "record.clear", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(CallRecordBinding(bindings, "record.load", {ScriptValue::Str(tape)},
                          {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.IsBool() && out.Truthy());
  CHECK(CallRecordBinding(bindings, "record.replay_step", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(CallRecordBinding(bindings, "record.replay_tick", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 1.0);

  // Not granted: denied, and nothing changes.
  ScriptValue denied;
  CHECK(CallRecordBinding(bindings, "record.start", {}, {}, &denied) ==
        ScriptStatus::CapabilityDenied);
  CHECK(CallRecordBinding(bindings, "record.save", {}, {}, &denied) ==
        ScriptStatus::CapabilityDenied);

  // No recorder: fail closed with a raised error on the calls that act, and
  // neutral values on the reads.
  ScriptApi::Services none;
  ScriptApi bare(none);
  ScriptBindings bareBindings;
  bare.InstallBindings(&bareBindings);
  ScriptValue bad;
  CHECK(CallRecordBinding(bareBindings, "record.start", {}, {"world"}, &bad) ==
        ScriptStatus::Error);
  CHECK(CallRecordBinding(bareBindings, "record.frames", {}, {"world"}, &bad) ==
        ScriptStatus::Ok);
  CHECK(bad.AsNumber() == 0.0);
}

int main() {
  TestLifecycle();
  TestCaptureFromSeam();
  TestCaptureRecordsDiffEvents();
  TestSaveLoadRoundTrip();
  TestReplayControls();
  TestEngineDrivesCapture();
  TestRecordBindings();
  if (g_fail == 0) printf("test_session_recorder: ALL OK\n");
  return g_fail;
}
