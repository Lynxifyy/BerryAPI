// Host tests for record & replay.
//
// Covers:
//   * Recording: strictly-increasing ticks, the per-frame caps, and the
//     recorder's capture-from-seam path.
//   * Serialisation: a round-trip preserves every field, output is
//     deterministic, and special characters survive escaping.
//   * Parsing: version checking, unknown keys, every rejection path, and the
//     strict-tick rule.
//   * ReplaySeam: step/seek/rewind, an empty world before the first frame,
//     every query, block lookup, raycasts, and level time.
//   * ReplayAll.
#include "berry/record.h"

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
  s.alive = true;
  s.health = 18.5f;
  s.maxHealth = 20;
  s.name = "test";
  s.lastSeenTick = 7;
  return s;
}

RecordedFrame Frame(u64 tick) {
  RecordedFrame f;
  f.tick = tick;
  f.time.absolute = (i64)tick * 20;
  f.time.daylight = 0.5f;
  f.time.day = true;
  return f;
}

}  // namespace

//----------------------------------------------------------------------------
// Recording
//----------------------------------------------------------------------------

static void TestRecording() {
  Recording tape;
  CHECK(tape.empty());

  RecordedFrame f1 = Frame(10);
  f1.entities.push_back(Snap(1, Vec3(0, 0, 0)));
  f1.events.push_back(RecordedEvent{"hit", "5"});
  f1.blocks.push_back(RecordedBlock{1, 2, 3, 42});
  CHECK(tape.Push(f1));

  // A repeat or a step back is refused: a tape always replays in order.
  CHECK(!tape.Push(Frame(10)));
  CHECK(!tape.Push(Frame(9)));
  CHECK(tape.Push(Frame(11)));

  CHECK(tape.frameCount() == 2);
  CHECK(tape.frames()[0].tick == 10);
  CHECK(tape.frames()[0].entities.size() == 1);
  CHECK(tape.frames()[0].events.size() == 1);
  CHECK(tape.frames()[0].blocks.size() == 1);

  tape.Clear();
  CHECK(tape.empty());
}

static void TestRecorder() {
  Recording tape;
  Recorder rec(&tape);
  CHECK(rec.active());

  rec.BeginFrame(1, LevelTime{});
  CHECK(rec.AddEntity(Snap(1, Vec3(0, 0, 0))));
  CHECK(rec.AddEvent("tick_count", "1"));
  CHECK(rec.AddBlock(0, 64, 0, 1));
  CHECK(rec.CommitFrame());
  CHECK(tape.frameCount() == 1);
  CHECK(tape.frames()[0].entities.size() == 1);

  // Adding outside a frame fails.
  CHECK(!rec.AddEntity(Snap(2, Vec3(0, 0, 0))));
  // Committing twice fails.
  CHECK(!rec.CommitFrame());
  // A same-tick commit is refused by the tape.
  rec.BeginFrame(1, LevelTime{});
  CHECK(!rec.CommitFrame());
  CHECK(tape.frameCount() == 1);

  // CaptureFrom a seam records every entity.
  Berry::Test::ShimLevel level;
  level.AddEntity(Id(1, 1), Snap(1, Vec3(0, 0, 0), EntityType::Player));
  level.AddEntity(Id(2, 2), Snap(2, Vec3(4, 0, 0)));
  rec.BeginFrame(2, level.GetTime());
  rec.CaptureFrom(level);
  CHECK(rec.CommitFrame());
  CHECK(tape.frameCount() == 2);
  CHECK(tape.frames()[1].entities.size() == 2);

  // The per-frame entity cap is enforced.
  Recorder capped(&tape);
  capped.BeginFrame(3, LevelTime{});
  for (size_t i = 0; i < Recording::kMaxEntitiesPerFrame; ++i) {
    CHECK(capped.AddEntity(Snap(i + 1, Vec3(0, 0, 0))));
  }
  CHECK(!capped.AddEntity(Snap(999999, Vec3(0, 0, 0))));
  CHECK(capped.overflowed());
  CHECK(capped.CommitFrame());
}

//----------------------------------------------------------------------------
// Serialisation round-trip
//----------------------------------------------------------------------------

static void TestRoundTrip() {
  Recording tape;
  RecordedFrame f1 = Frame(1);
  EntitySnapshot s = Snap(5, Vec3(1.5f, -2.25f, 3.0f), EntityType::Player);
  s.velocity = Vec3(0.1f, 0, 0);
  s.rotation = Vec3(10, 20, 30);
  s.onGround = false;
  s.name = "quote\"and\\back";   // needs escaping
  f1.entities.push_back(s);
  f1.events.push_back(RecordedEvent{"entityEntered", "5:player"});
  f1.blocks.push_back(RecordedBlock{3, 64, -5, 9});
  CHECK(tape.Push(f1));

  RecordedFrame f2 = Frame(2);
  f2.time.day = false;
  f2.time.rainLevel = 0.75f;
  CHECK(tape.Push(f2));

  const std::string json = tape.ToJson();
  // Deterministic: same tape, same bytes.
  CHECK(tape.ToJson() == json);

  Recording back;
  TapeError err = TapeError::Ok;
  std::string detail;
  CHECK(Recording::FromJson(json, &back, &err, &detail));
  CHECK(err == TapeError::Ok);
  CHECK(back.frameCount() == 2);

  const RecordedFrame& r1 = back.frames()[0];
  CHECK(r1.tick == 1);
  CHECK(r1.entities.size() == 1);
  CHECK(r1.entities[0].id.raw == 5);
  CHECK(r1.entities[0].id.runtime == 5);
  CHECK(r1.entities[0].type == EntityType::Player);
  CHECK(r1.entities[0].position.x == 1.5f);
  CHECK(r1.entities[0].position.y == -2.25f);
  CHECK(r1.entities[0].velocity.x == 0.1f);
  CHECK(r1.entities[0].rotation.z == 30);
  CHECK(r1.entities[0].health == 18.5f);
  CHECK(r1.entities[0].maxHealth == 20);
  CHECK(r1.entities[0].alive);
  CHECK(!r1.entities[0].onGround);
  CHECK(r1.entities[0].name == "quote\"and\\back");
  CHECK(r1.entities[0].lastSeenTick == 7);
  CHECK(r1.events.size() == 1);
  CHECK(r1.events[0].channel == "entityEntered");
  CHECK(r1.events[0].payload == "5:player");
  CHECK(r1.blocks.size() == 1);
  CHECK(r1.blocks[0].x == 3 && r1.blocks[0].y == 64 && r1.blocks[0].z == -5);
  CHECK(r1.blocks[0].block == 9);

  const RecordedFrame& r2 = back.frames()[1];
  CHECK(r2.tick == 2);
  CHECK(!r2.time.day);
  CHECK(r2.time.rainLevel == 0.75f);
  CHECK(r2.entities.empty());

  // Re-serialising the parsed tape reproduces the same bytes: the round trip
  // is a fixpoint.
  CHECK(back.ToJson() == json);
}

//----------------------------------------------------------------------------
// Parse rejection
//----------------------------------------------------------------------------

static void TestParseRejects() {
  Recording tape;
  TapeError err = TapeError::Ok;
  std::string detail;

  CHECK(!Recording::FromJson("not json", &tape, &err, &detail));
  CHECK(err == TapeError::BadJson);
  CHECK(!Recording::FromJson("[]", &tape, &err, &detail));
  CHECK(err == TapeError::BadJson);

  // Unknown top-level key.
  CHECK(!Recording::FromJson("{\"format\":1,\"frames\":[],\"nope\":1}", &tape, &err, &detail));
  CHECK(err == TapeError::BadJson);

  // Missing / wrong version.
  CHECK(!Recording::FromJson("{\"frames\":[{\"tick\":1}]}", &tape, &err, &detail));
  CHECK(err == TapeError::MissingVersion);
  CHECK(!Recording::FromJson("{\"format\":99,\"frames\":[{\"tick\":1}]}", &tape, &err, &detail));
  CHECK(err == TapeError::BadVersion);

  // No frames.
  CHECK(!Recording::FromJson("{\"format\":1}", &tape, &err, &detail));
  CHECK(err == TapeError::NoFrames);
  CHECK(!Recording::FromJson("{\"format\":1,\"frames\":[]}", &tape, &err, &detail));
  CHECK(err == TapeError::NoFrames);

  // A frame without a tick, or with an unknown key.
  CHECK(!Recording::FromJson("{\"format\":1,\"frames\":[{}]}", &tape, &err, &detail));
  CHECK(err == TapeError::BadFrame);
  CHECK(!Recording::FromJson(
      "{\"format\":1,\"frames\":[{\"tick\":1,\"nope\":1}]}", &tape, &err, &detail));
  CHECK(err == TapeError::BadFrame);

  // Non-increasing ticks.
  CHECK(!Recording::FromJson(
      "{\"format\":1,\"frames\":[{\"tick\":2},{\"tick\":1}]}", &tape, &err, &detail));
  CHECK(err == TapeError::BadTick);
  CHECK(!Recording::FromJson(
      "{\"format\":1,\"frames\":[{\"tick\":2},{\"tick\":2}]}", &tape, &err, &detail));
  CHECK(err == TapeError::BadTick);

  // A bad entity: missing id, unknown type, bad vec.
  CHECK(!Recording::FromJson(
      "{\"format\":1,\"frames\":[{\"tick\":1,\"entities\":[{}]}]}", &tape, &err, &detail));
  CHECK(err == TapeError::BadEntity);
  CHECK(!Recording::FromJson(
      "{\"format\":1,\"frames\":[{\"tick\":1,\"entities\":[{\"id\":1,\"type\":\"dragon\"}]}]}",
      &tape, &err, &detail));
  CHECK(err == TapeError::BadEntity);
  CHECK(!Recording::FromJson(
      "{\"format\":1,\"frames\":[{\"tick\":1,\"entities\":[{\"id\":1,\"pos\":[1,2]}]}]}",
      &tape, &err, &detail));
  CHECK(err == TapeError::BadEntity);

  // A bad event and a bad block.
  CHECK(!Recording::FromJson(
      "{\"format\":1,\"frames\":[{\"tick\":1,\"events\":[{\"channel\":\"x\"}]}]}",
      &tape, &err, &detail));
  CHECK(err == TapeError::BadEvent);
  CHECK(!Recording::FromJson(
      "{\"format\":1,\"frames\":[{\"tick\":1,\"blocks\":[{\"x\":1,\"y\":2}]}]}",
      &tape, &err, &detail));
  CHECK(err == TapeError::BadBlock);

  // A rejected tape leaves the output untouched: the caller's tape stays empty.
  Recording keep;
  CHECK(!Recording::FromJson("{\"format\":1,\"frames\":[]}", &keep, &err, &detail));
  CHECK(keep.empty());
}

//----------------------------------------------------------------------------
// ReplaySeam
//----------------------------------------------------------------------------

static void TestReplaySeam() {
  Recording tape;

  RecordedFrame f1 = Frame(100);
  f1.time.daylight = 0.9f;
  f1.time.day = true;
  f1.entities.push_back(Snap(1, Vec3(0, 0, 0), EntityType::Player));
  f1.entities.push_back(Snap(2, Vec3(5, 0, 0), EntityType::Mob));
  f1.blocks.push_back(RecordedBlock{0, 64, 0, 3});
  CHECK(tape.Push(f1));

  RecordedFrame f2 = Frame(101);
  f2.time.daylight = 0.1f;
  f2.time.day = false;
  f2.entities.push_back(Snap(1, Vec3(0, 0, 0), EntityType::Player));
  f2.entities.push_back(Snap(2, Vec3(10, 0, 0), EntityType::Mob));   // moved
  CHECK(tape.Push(f2));

  ReplaySeam seam(&tape);
  CHECK(std::string(seam.Name()) == "replay");
  CHECK(seam.frameCount() == 2);
  CHECK(seam.frameIndex() == -1);  // before the first step: nothing
  CHECK(seam.atEnd());
  CHECK(seam.AllEntities().empty());
  CHECK(seam.GetTime().daylight == 0);

  // Step to frame 1.
  CHECK(seam.Step());
  CHECK(seam.frameIndex() == 0);
  CHECK(seam.currentTick() == 100);
  CHECK(seam.AllEntities().size() == 2);

  EntitySnapshot got;
  CHECK(seam.GetEntity(Id(2, 2), &got));
  CHECK(got.position.x == 5);
  CHECK(!seam.GetEntity(Id(99, 99), &got));

  // Level time and blocks.
  CHECK(seam.GetTime().daylight == 0.9f);
  CHECK(seam.GetTime().day);
  CHECK(seam.GetBlock(Vec3(0.4f, 64.9f, 0.2f)) == 3);
  CHECK(seam.GetBlock(Vec3(9, 9, 9)) == 0);

  // Queries.
  CHECK(seam.EntitiesInRadius(Vec3(0, 0, 0), 6).size() == 2);
  CHECK(seam.EntitiesInRadius(Vec3(0, 0, 0), 3).size() == 1);
  CHECK(seam.EntitiesInAabb(Aabb(Vec3(-1, -1, -1), Vec3(6, 1, 1))).size() == 2);
  CHECK(seam.EntitiesInAabb(Aabb(Vec3(-1, -1, -1), Vec3(6, 1, 1)), EntityType::Player).size() == 1);

  // Raycast a mob standing at x=5, from x=2 looking along +x. The player at the
  // origin is behind the ray, so it is not a candidate.
  EntityId hitId;
  f32 hitDist = 0;
  CHECK(seam.RaycastEntity(Vec3(2, 0, 0), Vec3(1, 0, 0), 100, &hitId, &hitDist));
  CHECK(hitId.raw == 2);
  CHECK(hitDist > 2.9f && hitDist < 3.1f);
  // A ray that points away from everything misses.
  CHECK(!seam.RaycastEntity(Vec3(2, 0, 0), Vec3(0, 1, 0), 100, &hitId, &hitDist));

  // Raycast a block at (0,64,0): from above, looking down.
  Vec3 hitPos;
  u32 hitBlock = 0;
  CHECK(seam.RaycastBlock(Vec3(0.5f, 70, 0.5f), Vec3(0, -1, 0), 100, &hitPos, &hitBlock));
  CHECK(hitBlock == 3);
  CHECK(hitPos.y == 64);

  // Step to frame 2: the mob has moved.
  CHECK(seam.Step());
  CHECK(seam.frameIndex() == 1);
  CHECK(seam.currentTick() == 101);
  CHECK(seam.GetEntity(Id(2, 2), &got));
  CHECK(got.position.x == 10);
  CHECK(!seam.GetTime().day);

  // Past the end: an empty world, not a stale frame.
  CHECK(!seam.Step());
  CHECK(seam.atEnd());
  CHECK(seam.AllEntities().empty());
  CHECK(seam.currentTick() == 0);

  // Seek and rewind.
  CHECK(seam.SeekTo(0));
  CHECK(seam.currentTick() == 100);
  CHECK(!seam.SeekTo(5));
  CHECK(seam.currentTick() == 100);   // a failed seek does not move
  seam.Rewind();
  CHECK(seam.frameIndex() == -1);
  CHECK(seam.AllEntities().empty());
  CHECK(seam.atEnd());

  // A null tape answers an empty world rather than crashing.
  ReplaySeam nullSeam(nullptr);
  CHECK(!nullSeam.Step());
  CHECK(nullSeam.AllEntities().empty());
  CHECK(nullSeam.GetTime().absolute == 0);
}

//----------------------------------------------------------------------------
// ReplayAll
//----------------------------------------------------------------------------

static void TestReplayAll() {
  Recording tape;
  for (u64 t = 1; t <= 3; ++t) {
    RecordedFrame f = Frame(t);
    f.entities.push_back(Snap(t, Vec3((f32)t, 0, 0)));
    CHECK(tape.Push(f));
  }

  std::vector<u64> ticks;
  size_t totalEntities = 0;
  ReplayAll(tape, [&](ReplaySeam& seam, const RecordedFrame& f) {
    ticks.push_back(seam.currentTick());
    CHECK(&f == &tape.frames()[(size_t)seam.frameIndex()]);
    totalEntities += seam.AllEntities().size();
  });
  CHECK(ticks.size() == 3);
  CHECK(ticks[0] == 1 && ticks[1] == 2 && ticks[2] == 3);
  CHECK(totalEntities == 3);

  // An empty tape replays zero frames.
  Recording empty;
  size_t n = 0;
  ReplayAll(empty, [&](ReplaySeam&, const RecordedFrame&) { ++n; });
  CHECK(n == 0);
}

//----------------------------------------------------------------------------
// Record -> replay end-to-end
//----------------------------------------------------------------------------

static void TestEndToEnd() {
  Berry::Test::ShimLevel level;
  level.AddEntity(Id(1, 1), Snap(1, Vec3(0, 0, 0), EntityType::Player));
  level.AddEntity(Id(2, 2), Snap(2, Vec3(5, 0, 0)));

  // Record two ticks, with a block read each tick.
  Recording tape;
  Recorder rec(&tape);
  rec.BeginFrame(1, level.GetTime());
  rec.CaptureFrom(level);
  rec.AddBlock(0, 64, 0, level.GetBlock(Vec3(0, 64, 0)));
  CHECK(rec.CommitFrame());

  level.AddEntity(Id(2, 2), Snap(2, Vec3(8, 0, 0)));   // mob moves
  rec.BeginFrame(2, level.GetTime());
  rec.CaptureFrom(level);
  CHECK(rec.CommitFrame());

  // Serialise, parse back, and replay: the replay must match what was recorded.
  Recording back;
  std::string detail;
  TapeError err = TapeError::Ok;
  CHECK(Recording::FromJson(tape.ToJson(), &back, &err, &detail));

  ReplaySeam seam(&back);
  CHECK(seam.Step());
  EntitySnapshot s;
  CHECK(seam.GetEntity(Id(2, 2), &s));
  CHECK(s.position.x == 5);
  CHECK(seam.Step());
  CHECK(seam.GetEntity(Id(2, 2), &s));
  CHECK(s.position.x == 8);
}

int main() {
  TestRecording();
  TestRecorder();
  TestRoundTrip();
  TestParseRejects();
  TestReplaySeam();
  TestReplayAll();
  TestEndToEnd();
  if (g_fail == 0) printf("test_record: ALL OK\n");
  return g_fail;
}
