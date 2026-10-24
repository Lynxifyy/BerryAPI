// Host tests for the loopback replay server (berry/replay_server.h).
//
// The server plays a replay back to the game's own client over loopback. These
// tests stand in for that client with a real UDP socket on 127.0.0.1: they
// join, receive the accept frame and the streamed packets, seek, pause, and
// leave — and they prove the server refuses any non-loopback host, which is the
// invariant that keeps a recorded session from leaving the device.
#include "berry/replay_capture.h"
#include "berry/replay_format.h"
#include "berry/replay_server.h"

#include "berry/net.h"
#include "shim_level.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace Berry;
using Berry::Test::ShimLevel;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

EntitySnapshot MakeSnap(u64 raw, EntityType type, f32 x, f32 y, f32 z) {
  EntitySnapshot s;
  s.id.raw = raw;
  s.id.runtime = (u16)raw;
  s.type = type;
  s.position = Vec3(x, y, z);
  s.health = 20;
  s.maxHealth = 20;
  s.alive = true;
  return s;
}
EntityId Id(u64 raw) {
  EntityId id;
  id.raw = raw;
  id.runtime = (u16)raw;
  return id;
}

void PutU32(std::string* out, u32 v) {
  out->push_back((char)((v >> 24) & 0xff));
  out->push_back((char)((v >> 16) & 0xff));
  out->push_back((char)((v >> 8) & 0xff));
  out->push_back((char)(v & 0xff));
}
bool GetU32(const std::string& s, size_t off, u32* out) {
  if (off + 4 > s.size()) return false;
  *out = ((u32)(unsigned char)s[off] << 24) |
         ((u32)(unsigned char)s[off + 1] << 16) |
         ((u32)(unsigned char)s[off + 2] << 8) |
         (u32)(unsigned char)s[off + 3];
  return true;
}

/// Builds a replay whose packets are tagged with their tick, so a test can
/// check the order and the seek.
ReplayFile BuildReplay(u64 ticks) {
  ReplayCapture cap;
  CaptureConfig cfg;
  cfg.keyframeTicks = 5;
  cfg.capturePackets = true;
  cap.SetConfig(cfg);
  cap.Start();
  ShimLevel seam;
  EntityId player = Id(1);
  seam.AddEntity(player, MakeSnap(1, EntityType::Player, 0, 64, 0));
  for (u64 t = 0; t <= ticks; ++t) {
    // One packet per tick, id 0x10, payload = the tick as 4 bytes.
    std::string payload;
    PutU32(&payload, (u32)t);
    cap.CapturePacket(PacketDir::Inbound, 0x10, payload, t);
    cap.CaptureTick(t, seam, {}, player);
  }
  cap.Stop();
  return cap.Snapshot();
}

/// A test client that speaks the wire protocol.
class TestClient {
 public:
  bool Open() { return sock_.Bind(0, "127.0.0.1"); }
  void Close() { sock_.Close(); }
  bool Join(const char* host, u16 port) {
    const std::string f("BRJO", 4);
    return sock_.SendTo(f.data(), f.size(), host, port);
  }
  bool Goto(const char* host, u16 port, u32 tick) {
    std::string f("BRGO", 4);
    PutU32(&f, tick);
    return sock_.SendTo(f.data(), f.size(), host, port);
  }
  bool Bye(const char* host, u16 port) {
    const std::string f("BRBY", 4);
    return sock_.SendTo(f.data(), f.size(), host, port);
  }
  /// Receives one datagram (0 on timeout).
  ssize_t Recv(std::string* out, int timeoutMs = 200) {
    std::vector<char> buf(70000);
    std::string from;
    u16 fp = 0;
    const ssize_t n = sock_.RecvFrom(buf.data(), buf.size(), &from, &fp, timeoutMs);
    if (n > 0) out->assign(buf.data(), (size_t)n);
    return n;
  }
  UdpSocket& socket() { return sock_; }

 private:
  UdpSocket sock_;
};

//--------------------------------------------------------------------------

void TestRefusesNonLoopback() {
  ReplayServer srv;
  ReplayServerConfig cfg;
  cfg.host = "0.0.0.0";
  CHECK(srv.Start(cfg) == ReplayServerError::NotLoopback);
  cfg.host = "192.168.1.5";
  CHECK(srv.Start(cfg) == ReplayServerError::NotLoopback);
  cfg.host = "example.com";
  CHECK(srv.Start(cfg) == ReplayServerError::NotLoopback);
  CHECK(!srv.running());
  // The loopback names are accepted.
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  CHECK(srv.Start(cfg) == ReplayServerError::Ok);
  CHECK(srv.running());
  CHECK(srv.port() != 0);
  CHECK(srv.Start(cfg) == ReplayServerError::AlreadyRunning);
  srv.Stop();
  CHECK(!srv.running());
  CHECK(srv.port() == 0);
}

void TestStreamsPackets() {
  ReplayFile f = BuildReplay(20);
  ReplayServer srv;
  ReplayServerConfig cfg;
  cfg.port = 0;
  CHECK(srv.Start(cfg) == ReplayServerError::Ok);
  srv.Attach(&f);
  const u16 port = srv.port();

  TestClient cli;
  CHECK(cli.Open());
  CHECK(cli.Join("127.0.0.1", port));

  // Pump until the join is accepted and the accept frame arrives.
  std::string accept;
  bool gotAccept = false;
  for (int i = 0; i < 50 && !gotAccept; ++i) {
    srv.Pump(5);
    if (cli.Recv(&accept, 20) > 0 && accept.size() >= 8 &&
        accept.compare(0, 4, "BRAC") == 0) {
      gotAccept = true;
    }
  }
  CHECK(gotAccept);
  CHECK(srv.clientConnected());
  u32 startTick = 0;
  CHECK(GetU32(accept, 4, &startTick));
  CHECK(startTick == 0);

  // Pump the whole stream; collect the packets and check they arrive in order
  // with the recorded payloads.
  std::vector<u32> ids;
  std::vector<u32> ticks;
  bool gotEnd = false;
  for (int i = 0; i < 400 && !gotEnd; ++i) {
    srv.Pump(2);
    std::string frame;
    while (cli.Recv(&frame, 5) > 0) {
      if (frame.size() >= 4 && frame.compare(0, 4, "BREN") == 0) {
        gotEnd = true;
        break;
      }
      if (frame.size() >= 12 && frame.compare(0, 4, "BRPK") == 0) {
        u32 id = 0, len = 0;
        CHECK(GetU32(frame, 4, &id));
        CHECK(GetU32(frame, 8, &len));
        CHECK(frame.size() == 12 + len);
        CHECK(id == 0x10);
        u32 tick = 0;
        CHECK(GetU32(frame, 12, &tick));
        ids.push_back(id);
        ticks.push_back(tick);
      }
    }
  }
  CHECK(gotEnd);
  // Every recorded packet (ticks 0..20) was streamed, in order.
  CHECK(ticks.size() == 21);
  for (size_t i = 0; i < ticks.size(); ++i) CHECK(ticks[i] == (u32)i);
  CHECK(srv.packetsSent() == 21);

  CHECK(cli.Bye("127.0.0.1", port));
  srv.Pump(10);
  CHECK(!srv.clientConnected());
  srv.Stop();
}

void TestSeek() {
  ReplayFile f = BuildReplay(20);
  ReplayServer srv;
  ReplayServerConfig cfg;
  cfg.port = 0;
  CHECK(srv.Start(cfg) == ReplayServerError::Ok);
  srv.Attach(&f);
  const u16 port = srv.port();

  TestClient cli;
  CHECK(cli.Open());
  CHECK(cli.Join("127.0.0.1", port));
  for (int i = 0; i < 50 && !srv.clientConnected(); ++i) srv.Pump(5);
  CHECK(srv.clientConnected());
  // Drain whatever the server already streamed from tick 0, so the assertion
  // below sees only the post-seek stream.
  {
    std::string frame;
    while (cli.Recv(&frame, 5) > 0) {}
  }

  // Seek to tick 10; the stream should restart near the keyframe at or before
  // it (keyframes every 5 ticks, so 10).
  CHECK(srv.Seek(10));
  CHECK(srv.streamTick() == 10);

  // Pump a little and confirm the first packet's payload tick is >= 10.
  std::vector<u32> ticks;
  for (int i = 0; i < 60; ++i) {
    srv.Pump(2);
    std::string frame;
    while (cli.Recv(&frame, 2) > 0) {
      if (frame.size() >= 16 && frame.compare(0, 4, "BRPK") == 0) {
        u32 tick = 0;
        CHECK(GetU32(frame, 12, &tick));
        ticks.push_back(tick);
      }
    }
    if (!ticks.empty()) break;
  }
  CHECK(!ticks.empty());
  CHECK(ticks.front() >= 10);
  srv.Stop();
}

void TestPause() {
  ReplayFile f = BuildReplay(20);
  ReplayServer srv;
  ReplayServerConfig cfg;
  cfg.port = 0;
  CHECK(srv.Start(cfg) == ReplayServerError::Ok);
  srv.Attach(&f);
  const u16 port = srv.port();
  TestClient cli;
  CHECK(cli.Open());
  CHECK(cli.Join("127.0.0.1", port));
  for (int i = 0; i < 50 && !srv.clientConnected(); ++i) srv.Pump(5);

  srv.Pause();
  CHECK(srv.paused());
  const u64 sentBefore = srv.packetsSent();
  for (int i = 0; i < 20; ++i) srv.Pump(1);
  CHECK(srv.packetsSent() == sentBefore);   // paused: nothing sent

  srv.Resume();
  CHECK(!srv.paused());
  for (int i = 0; i < 20; ++i) srv.Pump(1);
  CHECK(srv.packetsSent() > sentBefore);
  srv.Stop();
}

void TestEmptyReplaySendsEnd() {
  ReplayServer srv;
  ReplayServerConfig cfg;
  cfg.port = 0;
  CHECK(srv.Start(cfg) == ReplayServerError::Ok);
  // No Attach: nothing to stream.
  const u16 port = srv.port();
  TestClient cli;
  CHECK(cli.Open());
  CHECK(cli.Join("127.0.0.1", port));
  bool gotEnd = false;
  for (int i = 0; i < 50 && !gotEnd; ++i) {
    srv.Pump(5);
    std::string frame;
    while (cli.Recv(&frame, 5) > 0) {
      if (frame.size() >= 4 && frame.compare(0, 4, "BREN") == 0) gotEnd = true;
    }
  }
  CHECK(gotEnd);
  srv.Stop();
}

void TestUnknownFrameDropped() {
  ReplayFile f = BuildReplay(5);
  ReplayServer srv;
  ReplayServerConfig cfg;
  cfg.port = 0;
  CHECK(srv.Start(cfg) == ReplayServerError::Ok);
  srv.Attach(&f);
  const u16 port = srv.port();
  TestClient cli;
  CHECK(cli.Open());
  // A garbage frame before joining must not crash or connect.
  const std::string junk = "ZZZZnot-a-frame";
  cli.socket().SendTo(junk.data(), junk.size(), "127.0.0.1", port);
  srv.Pump(5);
  CHECK(!srv.clientConnected());
  // A short frame too.
  const std::string shortf = "BR";
  cli.socket().SendTo(shortf.data(), shortf.size(), "127.0.0.1", port);
  srv.Pump(5);
  CHECK(!srv.clientConnected());
  srv.Stop();
}

void TestSeekWithoutClient() {
  ReplayFile f = BuildReplay(5);
  ReplayServer srv;
  ReplayServerConfig cfg;
  cfg.port = 0;
  CHECK(srv.Start(cfg) == ReplayServerError::Ok);
  srv.Attach(&f);
  CHECK(!srv.Seek(3));   // no client connected
  srv.Stop();
}

}  // namespace

int main() {
  TestRefusesNonLoopback();
  TestStreamsPackets();
  TestSeek();
  TestPause();
  TestEmptyReplaySendsEnd();
  TestUnknownFrameDropped();
  TestSeekWithoutClient();

  if (g_fail == 0) {
    printf("test_replay_server: ALL OK\n");
    return 0;
  }
  printf("test_replay_server: %d FAILURES\n", g_fail);
  return 1;
}
