// Host tests for the replay capture service (berry/replay_capture.h).
//
// The capture layer is what turns the live stream into a replay, so these tests
// drive it exactly as the client would: a shim seam that moves, spawns and
// despawns entities across ticks, a packet feed, and a save/reload round trip
// through the container. They also pin the bounds (byte budget, ring window,
// thermal throttle) and the privacy behaviour (anonymize).
#include "berry/replay_capture.h"
#include "berry/replay_format.h"

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
  s.velocity = Vec3(0, 0, 0);
  s.rotation = Vec3(0, 0, 0);
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

//--------------------------------------------------------------------------

void TestCodecRoundTrip() {
  EntitySnapshot s = MakeSnap(7, EntityType::Player, 1.5f, 64.0f, -3.25f, "Alice");
  const std::string enc = ReplayCodec::EncodeEntity(s, /*full=*/true);
  EntitySnapshot out;
  bool full = false;
  CHECK(ReplayCodec::DecodeEntity(enc, &out, &full));
  CHECK(full);
  CHECK(out.id.raw == 7);
  CHECK(out.type == EntityType::Player);
  CHECK(out.position.x == 1.5f && out.position.y == 64.0f && out.position.z == -3.25f);
  CHECK(out.health == 20 && out.maxHealth == 20);
  CHECK(out.alive && out.onGround);
  CHECK(out.name == "Alice");

  // A short payload fails closed at every truncation point.
  for (size_t cut = 0; cut < enc.size(); ++cut) {
    EntitySnapshot t;
    bool f = false;
    CHECK(!ReplayCodec::DecodeEntity(enc.substr(0, cut), &t, &f));
  }
  // Trailing garbage is refused, not ignored.
  CHECK(!ReplayCodec::DecodeEntity(enc + "x", &out, &full));

  LevelTime tm;
  tm.absolute = 12345;
  tm.daylight = 0.75f;
  tm.day = false;
  tm.rainLevel = 0.2f;
  tm.thunderLevel = 0.1f;
  const std::string te = ReplayCodec::EncodeTime(tm);
  LevelTime to;
  CHECK(ReplayCodec::DecodeTime(te, &to));
  CHECK(to.absolute == 12345);
  CHECK(to.daylight == 0.75f);
  CHECK(!to.day);
  CHECK(to.rainLevel == 0.2f && to.thunderLevel == 0.1f);
  for (size_t cut = 0; cut < te.size(); ++cut) {
    LevelTime t;
    CHECK(!ReplayCodec::DecodeTime(te.substr(0, cut), &t));
  }

  const std::string ee = ReplayCodec::EncodeEvent("combatConfirmed", "1:2");
  std::string ch, pl;
  CHECK(ReplayCodec::DecodeEvent(ee, &ch, &pl));
  CHECK(ch == "combatConfirmed");
  CHECK(pl == "1:2");
  for (size_t cut = 0; cut < ee.size(); ++cut) {
    std::string c, p;
    CHECK(!ReplayCodec::DecodeEvent(ee.substr(0, cut), &c, &p));
  }
}

void TestCaptureKeyframeAndDeltas() {
  ReplayCapture cap;
  CaptureConfig cfg;
  cfg.keyframeTicks = 10;
  cfg.capturePackets = true;
  cap.SetConfig(cfg);
  ReplayFingerprint fp;
  fp.buildId = 1;
  fp.label = "test";
  fp.protocolVersion = 2;
  cap.SetFingerprint(fp);
  CHECK(cap.Start());
  CHECK(cap.recording());
  CHECK(cap.state() == CaptureState::Recording);

  ShimLevel seam;
  EntityId player = Id(1);
  seam.AddEntity(player, MakeSnap(1, EntityType::Player, 0, 0, 0, "Me"));
  seam.AddEntity(Id(2), MakeSnap(2, EntityType::Mob, 10, 0, 0, "Zombie"));
  LevelTime t;
  t.absolute = 100;
  t.daylight = 0.5f;
  seam.SetTime(t);

  // Tick 0: a keyframe (the first tick always is).
  cap.CaptureTick(0, seam, {}, player);
  CHECK(cap.sectionCount() >= 1);

  // Ticks 1..9: the zombie moves each tick, so a delta carries its move.
  for (u64 tick = 1; tick <= 9; ++tick) {
    seam.AddEntity(Id(2), MakeSnap(2, EntityType::Mob, 10 + (f32)tick, 0, 0, "Zombie"));
    cap.CaptureTick(tick, seam, {}, player);
  }
  // Tick 10: a keyframe again.
  cap.CaptureTick(10, seam, {}, player);

  CHECK(cap.Stop());
  CHECK(!cap.recording());
  CHECK(cap.lastTick() == 10);
  CHECK(cap.firstTick() == 0);
  CHECK(cap.sectionCount() >= 2);
  CHECK(!cap.overflowed());

  // The saved replay round-trips and carries an index over its keyframes.
  std::string bytes;
  CHECK(cap.Save(&bytes) == ReplayError::Ok);
  ReplayFile f;
  ReplayReader r;
  CHECK(r.Read(bytes, &f) == ReplayError::Ok);
  CHECK(f.fingerprint.buildId == 1);
  CHECK(!f.sections.empty());
  CHECK(f.index.size() >= 2);   // at least the tick-0 and tick-10 keyframes
  CHECK(f.firstTick() == 0);
  CHECK(f.lastTick() == 10);

  // The keyframe at tick 0 carries both entities as full samples.
  const ReplaySection& first = f.sections.front();
  CHECK(first.kind == ReplaySectionKind::Keyframe);
  size_t spawns = 0;
  for (const ReplayRecord& rec : first.records) {
    if (rec.kind == ReplayRecordKind::EntitySpawn) {
      ++spawns;
      EntitySnapshot s;
      bool full = false;
      CHECK(ReplayCodec::DecodeEntity(rec.payload, &s, &full));
      CHECK(full);
    }
  }
  CHECK(spawns == 2);

  // A delta carries the zombie's move, and not a full re-send of the player.
  bool sawMove = false;
  for (const ReplaySection& s : f.sections) {
    if (s.kind != ReplaySectionKind::Delta) continue;
    for (const ReplayRecord& rec : s.records) {
      if (rec.kind == ReplayRecordKind::EntityMove) {
        sawMove = true;
        EntitySnapshot es;
        bool full = false;
        CHECK(ReplayCodec::DecodeEntity(rec.payload, &es, &full));
        CHECK(!full);
      }
    }
  }
  CHECK(sawMove);
}

void TestCapturePackets() {
  ReplayCapture cap;
  CaptureConfig cfg;
  cfg.keyframeTicks = 20;
  cfg.capturePackets = true;
  cap.SetConfig(cfg);
  cap.SetFingerprint(ReplayFingerprint{});
  CHECK(cap.Start());

  ShimLevel seam;
  seam.AddEntity(Id(1), MakeSnap(1, EntityType::Player, 0, 0, 0));
  cap.CaptureTick(0, seam, {}, Id(1));

  CHECK(cap.CapturePacket(PacketDir::Inbound, 0x2a, std::string("\x01\x02\x03", 3), 1));
  CHECK(cap.CapturePacket(PacketDir::Outbound, 0x2b, "hello", 2));
  // A packet over the cap is refused and reported.
  CaptureConfig small = cfg;
  small.maxPacketBytes = 4;
  cap.SetConfig(small);
  CHECK(!cap.CapturePacket(PacketDir::Inbound, 1, "toolong", 3));
  CHECK(cap.overflowed());
  cap.SetConfig(cfg);
  CHECK(cap.Stop());

  std::string bytes;
  CHECK(cap.Save(&bytes) == ReplayError::Ok);
  ReplayFile f;
  ReplayReader r;
  CHECK(r.Read(bytes, &f) == ReplayError::Ok);

  // Find the two packets and check their bytes, id, direction and tick survive.
  size_t packets = 0;
  bool sawInbound = false, sawOutbound = false;
  for (const ReplaySection& s : f.sections) {
    for (const ReplayRecord& rec : s.records) {
      if (rec.kind != ReplayRecordKind::Packet) continue;
      ++packets;
      if (rec.dir == 0 && rec.tag == 0x2a && rec.payload == std::string("\x01\x02\x03", 3)) {
        sawInbound = true;
        CHECK(rec.tick == 1);
      }
      if (rec.dir == 1 && rec.tag == 0x2b && rec.payload == "hello") {
        sawOutbound = true;
        CHECK(rec.tick == 2);
      }
    }
  }
  CHECK(packets == 2);
  CHECK(sawInbound && sawOutbound);
}

void TestPacketsDisabled() {
  ReplayCapture cap;
  CaptureConfig cfg;
  cfg.capturePackets = false;
  cap.SetConfig(cfg);
  CHECK(cap.Start());
  CHECK(!cap.CapturePacket(PacketDir::Inbound, 1, "x", 0));
  CHECK(cap.Stop());
}

void TestRingAndByteBudget() {
  // The ring keeps only the trailing window: "save the last N seconds".
  ReplayCapture cap;
  CaptureConfig cfg;
  cfg.keyframeTicks = 2;      // a keyframe every 2 ticks
  cfg.ringSeconds = 1.0f;     // ~20 ticks
  cfg.capturePackets = true;
  cap.SetConfig(cfg);
  CHECK(cap.Start());

  ShimLevel seam;
  EntityId player = Id(1);
  seam.AddEntity(player, MakeSnap(1, EntityType::Player, 0, 0, 0, "Me"));
  for (u64 tick = 0; tick < 100; ++tick) {
    seam.AddEntity(player, MakeSnap(1, EntityType::Player, (f32)tick, 0, 0, "Me"));
    cap.CaptureTick(tick, seam, {}, player);
  }
  CHECK(cap.evicted() > 0);
  CHECK(cap.firstTick() > 0);
  CHECK(cap.lastTick() == 99);

  // The byte budget evicts too.
  ReplayCapture cap2;
  CaptureConfig cfg2;
  cfg2.keyframeTicks = 1;
  cfg2.maxBytes = 4096;   // small, so the budget forces eviction
  cap2.SetConfig(cfg2);
  CHECK(cap2.Start());
  for (u64 tick = 0; tick < 200; ++tick) {
    seam.AddEntity(player, MakeSnap(1, EntityType::Player, (f32)tick, 0, 0, "Me"));
    cap2.CaptureTick(tick, seam, {}, player);
  }
  CHECK(cap2.bytes() <= 4096 + 4096);   // held near the budget
  CHECK(cap2.evicted() > 0);
}

void TestThermalThrottle() {
  ReplayCapture cap;
  CaptureConfig cfg;
  cfg.keyframeTicks = 5;
  cap.SetConfig(cfg);
  CHECK(cap.Start());
  CHECK(cap.state() == CaptureState::Recording);

  cap.SetThermalLevel(ReplayCapture::kThermalThrottleLevel);
  CHECK(cap.state() == CaptureState::Throttled);
  CHECK(cap.recording());   // still a session, just not adding deltas

  ShimLevel seam;
  EntityId player = Id(1);
  seam.AddEntity(player, MakeSnap(1, EntityType::Player, 0, 0, 0));
  // The first captured tick always lands a keyframe, even while throttled, so
  // take the baseline after it.
  cap.CaptureTick(0, seam, {}, player);
  const size_t before = cap.sectionCount();
  // While throttled, a moving entity adds no delta...
  for (u64 tick = 1; tick <= 3; ++tick) {
    seam.AddEntity(player, MakeSnap(1, EntityType::Player, (f32)tick, 0, 0));
    cap.CaptureTick(tick, seam, {}, player);
  }
  CHECK(cap.sectionCount() == before);   // no new sections

  // ...but a keyframe still lands, so the replay stays anchored.
  cap.CaptureTick(5, seam, {}, player);
  CHECK(cap.sectionCount() > before);

  // Cooling down resumes deltas.
  cap.SetThermalLevel(0);
  CHECK(cap.state() == CaptureState::Recording);
  CHECK(cap.Stop());
}

void TestAnonymize() {
  ReplayCapture cap;
  CaptureConfig cfg;
  cfg.keyframeTicks = 1;
  cap.SetConfig(cfg);
  CHECK(cap.Start());

  ShimLevel seam;
  EntityId a = Id(1), b = Id(2);
  seam.AddEntity(a, MakeSnap(1, EntityType::Player, 0, 0, 0, "RealNameAlice"));
  seam.AddEntity(b, MakeSnap(2, EntityType::Player, 1, 0, 0, "RealNameBob"));
  cap.CaptureTick(0, seam, {}, a);
  cap.CaptureTick(1, seam, {}, a);
  CHECK(cap.Stop());

  // Snapshot before anonymizing carries the real names.
  ReplayFile before = cap.Snapshot();
  bool sawReal = false;
  for (const ReplaySection& s : before.sections) {
    for (const ReplayRecord& r : s.records) {
      EntitySnapshot snap;
      bool full = false;
      if (ReplayCodec::DecodeEntity(r.payload, &snap, &full) &&
          snap.name == "RealNameAlice") {
        sawReal = true;
      }
    }
  }
  CHECK(sawReal);

  cap.Anonymize();
  ReplayFile after = cap.Snapshot();
  bool sawRealAfter = false, sawPseudonym = false;
  for (const ReplaySection& s : after.sections) {
    for (const ReplayRecord& r : s.records) {
      EntitySnapshot snap;
      bool full = false;
      if (!ReplayCodec::DecodeEntity(r.payload, &snap, &full)) continue;
      if (snap.name == "RealNameAlice" || snap.name == "RealNameBob") {
        sawRealAfter = true;
      }
      if (snap.name.rfind("Player", 0) == 0) sawPseudonym = true;
    }
  }
  CHECK(!sawRealAfter);
  CHECK(sawPseudonym);
}

void TestAnonymizeOnSave() {
  ReplayCapture cap;
  CaptureConfig cfg;
  cfg.keyframeTicks = 1;
  cfg.anonymizeOnSave = true;
  cap.SetConfig(cfg);
  CHECK(cap.Start());
  ShimLevel seam;
  seam.AddEntity(Id(1), MakeSnap(1, EntityType::Player, 0, 0, 0, "SecretPilot"));
  cap.CaptureTick(0, seam, {}, Id(1));
  CHECK(cap.Stop());

  std::string bytes;
  CHECK(cap.Save(&bytes) == ReplayError::Ok);
  ReplayFile f;
  ReplayReader r;
  CHECK(r.Read(bytes, &f) == ReplayError::Ok);
  bool sawSecret = false;
  for (const ReplaySection& s : f.sections) {
    for (const ReplayRecord& rec : s.records) {
      EntitySnapshot snap;
      bool full = false;
      if (ReplayCodec::DecodeEntity(rec.payload, &snap, &full) &&
          snap.name == "SecretPilot") {
        sawSecret = true;
      }
    }
  }
  CHECK(!sawSecret);
  // The live capture is unaffected: a snapshot without the save flag keeps the
  // real name, because anonymize-on-save only touches the exported copy.
  ReplayFile live = cap.Snapshot();
  bool sawLive = false;
  for (const ReplaySection& s : live.sections) {
    for (const ReplayRecord& rec : s.records) {
      EntitySnapshot snap;
      bool full = false;
      if (ReplayCodec::DecodeEntity(rec.payload, &snap, &full) &&
          snap.name == "SecretPilot") {
        sawLive = true;
      }
    }
  }
  CHECK(sawLive);
}

void TestEventMarkersAndSpawnLeave() {
  ReplayCapture cap;
  CaptureConfig cfg;
  cfg.keyframeTicks = 100;
  cap.SetConfig(cfg);
  CHECK(cap.Start());

  ShimLevel seam;
  EntityId a = Id(1);
  seam.AddEntity(a, MakeSnap(1, EntityType::Player, 0, 0, 0));
  cap.CaptureTick(0, seam, {}, a);

  // A spawn on tick 1.
  seam.AddEntity(Id(2), MakeSnap(2, EntityType::Mob, 5, 0, 0));
  cap.CaptureTick(1, seam, {}, a);

  // A timeline marker on tick 2.
  CHECK(cap.CaptureEvent("combatConfirmed", "1:2", 2));
  cap.CaptureTick(2, seam, {}, a);

  // The mob leaves on tick 3.
  seam.RemoveEntity(Id(2));
  cap.CaptureTick(3, seam, {}, a);
  CHECK(cap.Stop());

  std::string bytes;
  CHECK(cap.Save(&bytes) == ReplayError::Ok);
  ReplayFile f;
  ReplayReader r;
  CHECK(r.Read(bytes, &f) == ReplayError::Ok);

  bool sawSpawn = false, sawLeave = false, sawEvent = false;
  for (const ReplaySection& s : f.sections) {
    for (const ReplayRecord& rec : s.records) {
      if (rec.kind == ReplayRecordKind::EntitySpawn && rec.tick == 1) sawSpawn = true;
      if (rec.kind == ReplayRecordKind::EntityLeave && rec.tick == 3) sawLeave = true;
      if (rec.kind == ReplayRecordKind::Event && rec.tick == 2) {
        std::string ch, pl;
        CHECK(ReplayCodec::DecodeEvent(rec.payload, &ch, &pl));
        if (ch == "combatConfirmed" && pl == "1:2") sawEvent = true;
      }
    }
  }
  CHECK(sawSpawn);
  CHECK(sawLeave);
  CHECK(sawEvent);
}

void TestStopFlushesLastTick() {
  ReplayCapture cap;
  CaptureConfig cfg;
  cfg.keyframeTicks = 1000;   // effectively one keyframe, then deltas
  cap.SetConfig(cfg);
  CHECK(cap.Start());
  ShimLevel seam;
  EntityId a = Id(1);
  seam.AddEntity(a, MakeSnap(1, EntityType::Player, 0, 0, 0));
  cap.CaptureTick(0, seam, {}, a);
  // A delta built at tick 1 that would otherwise still be pending at Stop.
  seam.AddEntity(Id(2), MakeSnap(2, EntityType::Mob, 1, 0, 0));
  cap.CaptureTick(1, seam, {}, a);
  CHECK(cap.Stop());
  // The pending delta was flushed, so the last tick is 1.
  CHECK(cap.lastTick() == 1);

  std::string bytes;
  CHECK(cap.Save(&bytes) == ReplayError::Ok);
  ReplayFile f;
  ReplayReader r;
  CHECK(r.Read(bytes, &f) == ReplayError::Ok);
  CHECK(f.lastTick() == 1);
}

void TestSaveRing() {
  ReplayCapture cap;
  CaptureConfig cfg;
  cfg.keyframeTicks = 2;
  cap.SetConfig(cfg);
  CHECK(cap.Start());
  ShimLevel seam;
  EntityId a = Id(1);
  seam.AddEntity(a, MakeSnap(1, EntityType::Player, 0, 0, 0));
  for (u64 tick = 0; tick < 60; ++tick) {
    seam.AddEntity(a, MakeSnap(1, EntityType::Player, (f32)tick, 0, 0));
    cap.CaptureTick(tick, seam, {}, a);
  }
  CHECK(cap.Stop());
  std::string full;
  CHECK(cap.Save(&full) == ReplayError::Ok);
  std::string tail;
  CHECK(cap.SaveRing(1.0f, &tail) == ReplayError::Ok);
  // The tail export is smaller than the whole replay and still valid.
  CHECK(tail.size() < full.size());
  ReplayFile f;
  ReplayReader r;
  CHECK(r.Read(tail, &f) == ReplayError::Ok);
  CHECK(!f.sections.empty());
  CHECK(f.lastTick() == 59);
}

void TestStartStopLifecycle() {
  ReplayCapture cap;
  CHECK(cap.state() == CaptureState::Idle);
  CHECK(!cap.Stop());          // not recording
  CHECK(cap.Start());
  CHECK(!cap.Start());         // already recording
  CHECK(cap.Stop());
  CHECK(!cap.Stop());
  CHECK(cap.state() == CaptureState::Idle);

  // A capture tick while idle is a no-op.
  ShimLevel seam;
  seam.AddEntity(Id(1), MakeSnap(1, EntityType::Player, 0, 0, 0));
  cap.CaptureTick(0, seam, {}, Id(1));
  CHECK(cap.empty());

  // Start again clears the previous replay.
  CHECK(cap.Start());
  cap.CaptureTick(0, seam, {}, Id(1));
  CHECK(!cap.empty());
  CHECK(cap.Stop());
}

void TestEmptySaveIsValid() {
  ReplayCapture cap;
  CHECK(cap.Start());
  CHECK(cap.Stop());
  std::string bytes;
  CHECK(cap.Save(&bytes) == ReplayError::Ok);
  ReplayFile f;
  ReplayReader r;
  CHECK(r.Read(bytes, &f) == ReplayError::Ok);
  CHECK(f.sections.empty());
}

}  // namespace

int main() {
  TestCodecRoundTrip();
  TestCaptureKeyframeAndDeltas();
  TestCapturePackets();
  TestPacketsDisabled();
  TestRingAndByteBudget();
  TestThermalThrottle();
  TestAnonymize();
  TestAnonymizeOnSave();
  TestEventMarkersAndSpawnLeave();
  TestStopFlushesLastTick();
  TestSaveRing();
  TestStartStopLifecycle();
  TestEmptySaveIsValid();

  if (g_fail == 0) {
    printf("test_replay_capture: ALL OK\n");
    return 0;
  }
  printf("test_replay_capture: %d FAILURES\n", g_fail);
  return 1;
}
