// Host tests for the packet capture path (berry/packet_capture.h).
//
// These exercise the three pieces end to end:
//   * the classification table (a known id classifies, an unknown id is Other
//     and never fabricates a name);
//   * the fan-out hub (every observer sees every packet, in order, and a
//     dispatch with no observer reports zero);
//   * the receive hook, installed on a real function through the shim backend,
//     calling the original through the trampoline and dispatching the decoded
//     packet — including the fail-closed cases (no decode ABI, a decode that
//     refuses, a second hook on the same direction).
#include "berry/packet_capture.h"
#include "berry/replay_capture.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

/// A recording observer, so the fan-out can be checked without the replay layer.
struct RecordingObserver final : public PacketObserver {
  struct Seen {
    PacketDir dir;
    u32 id;
    PacketClass cls;
    std::string payload;
    u64 tick;
  };
  std::vector<Seen> seen;
  void OnPacket(PacketDir dir, u32 id, PacketClass cls,
                const std::string& payload, u64 tick) override {
    seen.push_back(Seen{dir, id, cls, payload, tick});
  }
};

/// Bridges the hub to a `ReplayCapture`, so a packet captured on the receive
/// path lands in a replay. This is the shape the client wires in production.
struct CaptureObserver final : public PacketObserver {
  ReplayCapture* cap = nullptr;
  void OnPacket(PacketDir dir, u32 id, PacketClass cls,
                const std::string& payload, u64 tick) override {
    if (cap) cap->CapturePacket(dir, id, payload, tick, ReplayKindForClass(cls));
  }
};

}  // namespace

//--------------------------------------------------------------------------

static void TestTableClassifies() {
  const PacketIdTable& t = PacketIdTable::Default();
  CHECK(t.Lookup(57).cls == PacketClass::Chunk);      // LevelChunk
  CHECK(std::string(t.Lookup(57).name) == "LevelChunk");
  CHECK(t.Lookup(20).cls == PacketClass::Block);      // UpdateBlock
  CHECK(t.Lookup(18).cls == PacketClass::Entity);     // MovePlayer
  CHECK(t.Lookup(9).cls == PacketClass::Time);        // SetTime
  CHECK(t.Lookup(143).cls == PacketClass::Player);    // PlayerAuthInput

  // An unknown id is Other with no name, never a fabricated one.
  const PacketIdInfo u = t.Lookup(999999);
  CHECK(u.cls == PacketClass::Other);
  CHECK(u.name != nullptr && u.name[0] == '\0');
}

static void TestTableCustomRows() {
  PacketIdTable t;
  t.Set(0x1234, PacketClass::Block, "MyBlock");
  const PacketIdInfo i = t.Lookup(0x1234);
  CHECK(i.cls == PacketClass::Block);
  CHECK(std::string(i.name) == "MyBlock");
  CHECK(t.size() == 1);

  // Replacing a row keeps the interned name valid.
  t.Set(0x1234, PacketClass::Entity, "MyEntity");
  CHECK(t.Lookup(0x1234).cls == PacketClass::Entity);
  CHECK(std::string(t.Lookup(0x1234).name) == "MyEntity");

  // CopyRowsFrom seeds a table from another (the built-in table).
  t.CopyRowsFrom(PacketIdTable::Default());
  CHECK(t.Lookup(57).cls == PacketClass::Chunk);
  CHECK(t.Lookup(0x1234).cls == PacketClass::Other);   // the custom row is gone
}

static void TestKindMapping() {
  CHECK(ReplayKindForClass(PacketClass::Chunk) == ReplayRecordKind::Chunk);
  CHECK(ReplayKindForClass(PacketClass::Block) == ReplayRecordKind::Chunk);
  CHECK(ReplayKindForClass(PacketClass::Entity) == ReplayRecordKind::Packet);
  CHECK(ReplayKindForClass(PacketClass::Time) == ReplayRecordKind::Packet);
  CHECK(ReplayKindForClass(PacketClass::Player) == ReplayRecordKind::Packet);
  CHECK(ReplayKindForClass(PacketClass::Other) == ReplayRecordKind::Packet);
}

static void TestHubFanout() {
  PacketCaptureHub hub;
  CHECK(hub.observerCount() == 0);
  CHECK(hub.Dispatch(PacketDir::Inbound, 57, "x", 1) == 0);   // no observer

  RecordingObserver a, b;
  hub.AddObserver(&a);
  hub.AddObserver(&b);
  hub.AddObserver(&a);   // idempotent
  CHECK(hub.observerCount() == 2);

  CHECK(hub.Dispatch(PacketDir::Inbound, 57, "chunk-bytes", 10) == 2);
  CHECK(hub.Dispatch(PacketDir::Outbound, 143, "input", 11) == 2);
  // The no-observer dispatch above also counted, so this is 3, not 2.
  CHECK(hub.dispatched() == 3);

  CHECK(a.seen.size() == 2 && b.seen.size() == 2);
  CHECK(a.seen[0].cls == PacketClass::Chunk);
  CHECK(a.seen[0].payload == "chunk-bytes");
  CHECK(a.seen[0].tick == 10);
  CHECK(a.seen[1].dir == PacketDir::Outbound);
  CHECK(a.seen[1].cls == PacketClass::Player);

  hub.RemoveObserver(&a);
  CHECK(hub.observerCount() == 1);
  hub.Dispatch(PacketDir::Inbound, 9, "t", 12);
  CHECK(a.seen.size() == 2);      // no longer receives
  CHECK(b.seen.size() == 3);
}

//--------------------------------------------------------------------------
// The receive hook, driven through the shim backend on a real function.
//--------------------------------------------------------------------------

namespace {

// A fake "game" receive function with the conventional Bedrock shape. It counts
// calls, so the test can prove the hook calls the original.
std::atomic<int> g_origCalls{0};
std::atomic<int> g_lastPayload{0};

void FakeReceive(void* /*self*/, const void* packet) {
  ++g_origCalls;
  if (packet) g_lastPayload = *(const int*)packet;
}

/// A packet object the fake decode understands: the first 4 bytes are the id,
/// the rest is the payload.
struct FakePacket {
  u32 id;
  const char* body;
};

bool FakeDecode(const void* p, u32* id, std::string* payload) {
  if (!p) return false;
  const FakePacket* fp = (const FakePacket*)p;
  if (fp->id == 0xDEAD) return false;   // a "not a packet" sentinel
  *id = fp->id;
  *payload = fp->body ? fp->body : "";
  return true;
}

}  // namespace

static void TestHookCapturesAndCallsThrough() {
  g_origCalls = 0;
  PacketCaptureHub hub;
  RecordingObserver obs;
  hub.AddObserver(&obs);

  PacketReceiveAbi abi;
  abi.decode = FakeDecode;
  abi.tick = []() { return (u64)42; };

  PacketReceiveHook hook;
  CHECK(hook.Install((void*)&FakeReceive, abi, HookManager::Instance().Backend(), &hub));
  CHECK(hook.installed());
  CHECK(hook.direction() == PacketDir::Inbound);

  // A second hook on the same direction is refused (one detour per direction).
  PacketReceiveHook dup;
  CHECK(!dup.Install((void*)&FakeReceive, abi, HookManager::Instance().Backend(), &hub));

  FakePacket pkt{57, "the-chunk"};
  FakeReceive(nullptr, &pkt);   // goes through the detour

  // The original ran, and the packet was captured and classified.
  CHECK(g_origCalls == 1);
  CHECK(hook.captured() == 1);
  CHECK(obs.seen.size() == 1);
  CHECK(obs.seen[0].id == 57);
  CHECK(obs.seen[0].cls == PacketClass::Chunk);
  CHECK(obs.seen[0].payload == "the-chunk");
  CHECK(obs.seen[0].tick == 42);

  hook.Uninstall();
  CHECK(!hook.installed());
  // After uninstall the function is the original again.
  FakePacket pkt2{20, "block"};
  FakeReceive(nullptr, &pkt2);
  CHECK(g_origCalls == 2);
  CHECK(obs.seen.size() == 1);   // no capture after uninstall
}

static void TestHookFailClosed() {
  PacketCaptureHub hub;

  // No decode ABI: refuse, and leave no hook behind.
  PacketReceiveAbi noDecode;
  noDecode.tick = []() { return (u64)0; };
  PacketReceiveHook h1;
  CHECK(!h1.Install((void*)&FakeReceive, noDecode, HookManager::Instance().Backend(), &hub));
  CHECK(!h1.installed());

  // Null target: refuse.
  PacketReceiveAbi abi;
  abi.decode = FakeDecode;
  PacketReceiveHook h2;
  CHECK(!h2.Install(nullptr, abi, HookManager::Instance().Backend(), &hub));
  CHECK(!h2.installed());

  // Null hub: refuse.
  PacketReceiveHook h3;
  CHECK(!h3.Install((void*)&FakeReceive, abi, HookManager::Instance().Backend(), nullptr));
  CHECK(!h3.installed());

  // A decode that refuses the object captures nothing, but still calls the
  // original (the hook never changes client behaviour).
  g_origCalls = 0;
  RecordingObserver obs;
  hub.AddObserver(&obs);
  PacketReceiveHook h4;
  CHECK(h4.Install((void*)&FakeReceive, abi, HookManager::Instance().Backend(), &hub));
  FakePacket bad{0xDEAD, "nope"};
  FakeReceive(nullptr, &bad);
  CHECK(g_origCalls == 1);
  CHECK(h4.captured() == 0);
  CHECK(obs.seen.empty());
  h4.Uninstall();
}

static void TestHubFeedsReplayCapture() {
  // The whole path: a captured packet lands in a replay with the right kind.
  PacketCaptureHub hub;
  ReplayCapture cap;
  CaptureObserver obs;
  obs.cap = &cap;
  hub.AddObserver(&obs);

  cap.SetConfig([] {
    CaptureConfig c;
    c.capturePackets = true;
    return c;
  }());
  CHECK(cap.Start());

  hub.Dispatch(PacketDir::Inbound, 57, "chunk-data", 5);   // LevelChunk
  hub.Dispatch(PacketDir::Inbound, 20, "block-data", 6);   // UpdateBlock
  hub.Dispatch(PacketDir::Inbound, 18, "move", 7);         // MovePlayer

  const ReplayFile f = cap.Snapshot();
  size_t chunk = 0, packet = 0;
  for (const ReplaySection& s : f.sections) {
    for (const ReplayRecord& r : s.records) {
      if (r.kind == ReplayRecordKind::Chunk) ++chunk;
      else if (r.kind == ReplayRecordKind::Packet) ++packet;
    }
  }
  CHECK(chunk == 2);    // the chunk and the block packet
  CHECK(packet == 1);   // the entity move
}

int main() {
  TestTableClassifies();
  TestTableCustomRows();
  TestKindMapping();
  TestHubFanout();
  TestHookCapturesAndCallsThrough();
  TestHookFailClosed();
  TestHubFeedsReplayCapture();

  if (g_fail) {
    printf("test_packet_capture: %d FAILURE(S)\n", g_fail);
    return 1;
  }
  printf("test_packet_capture: ALL OK\n");
  return 0;
}
