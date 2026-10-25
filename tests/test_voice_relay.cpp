// Host tests for the voice relay (F2): the UDP forwarder and the client
// transport that reaches peers through it.
//
// The relay moves *already-encrypted* voice datagrams, so the tests cover two
// layers:
//   * the relay's control plane — a room tag and a register auth are derived from
//     a secret, an unknown room / bad auth / unregistered source is dropped, a
//     registered peer's data is forwarded to the room's other members but never
//     echoed to the sender, and a stale peer ages out;
//   * the end-to-end path — two `VoiceRelayTransport`s, each with its own Ed25519
//     identity and an anchor holding the other's key, complete the real
//     announce -> verify -> X25519 -> HKDF -> ChaCha20-Poly1305 handshake through
//     a running relay and exchange a frame, and a peer without an accepted
//     identity cannot establish a session, so it can neither listen nor inject.
#include "berry/voice_relay.h"
#include "berry/voice_relay_transport.h"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

std::string Seed(u8 base) {
  std::string s(32, '\0');
  for (size_t i = 0; i < s.size(); ++i) s[i] = (char)(u8)(base + i * 11);
  return s;
}

VoiceTrustAnchor AnchorFor(const VoiceIdentity& id) {
  VoiceTrustAnchor a;
  a.kind = VoiceTrustAnchor::Kind::Ed25519Keys;
  a.AddPublicKeyHex(id.publicKeyHex());
  return a;
}

std::string RoomSecret() { return std::string(32, 'k'); }

VoiceRelayConfig RelayConfig(u16 port) {
  VoiceRelayConfig cfg;
  cfg.port = port;
  cfg.bindAddr = "127.0.0.1";
  cfg.peerTtlMs = 400;
  return cfg;
}

RelayVoiceConfig TransportConfig(u16 relayPort) {
  RelayVoiceConfig cfg;
  cfg.relayHost = "127.0.0.1";
  cfg.relayPort = relayPort;
  cfg.room = "room-1";
  cfg.roomSecret = RoomSecret();
  cfg.bindAddr = "127.0.0.1";
  cfg.bindPort = 0;
  cfg.announceIntervalMs = 100;
  return cfg;
}

/// Sends one raw datagram from a throwaway socket to the relay.
bool SendRaw(u16 relayPort, const std::string& bytes) {
  UdpSocket s;
  if (!s.Bind(0, "127.0.0.1")) return false;
  return s.SendTo(bytes.data(), bytes.size(), "127.0.0.1", relayPort);
}

}  // namespace

static void TestRoomTagDerivation() {
  const std::string secret = RoomSecret();
  // Deterministic, and domain-separated from the register auth.
  CHECK(VoiceRelay::RoomTagHex("r", secret) == VoiceRelay::RoomTagHex("r", secret));
  CHECK(VoiceRelay::RoomTagHex("r", secret) != VoiceRelay::RoomTagHex("s", secret));
  CHECK(VoiceRelay::RoomTagHex("r", secret) !=
        VoiceRelay::RoomTagHex("r", std::string(32, 'x')));
  const std::string tag = VoiceRelay::RoomTagHex("r", secret);
  CHECK(tag.size() == VoiceRelay::kTagBytes);
  CHECK(VoiceRelay::RegisterAuthHex(tag, secret).size() ==
        VoiceRelay::kAuthBytes);
  CHECK(VoiceRelay::RegisterAuthHex(tag, secret) != tag);
}

static void TestRelayRegistrationAndForwarding() {
  VoiceRelay relay;
  CHECK(relay.AddRoom("room-1", RoomSecret()));
  CHECK(!relay.AddRoom("room-1", RoomSecret()));  // duplicate refused
  CHECK(!relay.AddRoom("room-2", "short"));       // short secret refused
  CHECK(relay.AddRoom("room-2", std::string(20, 'z')));
  CHECK(relay.roomCount() == 2);
  CHECK(relay.Open(RelayConfig(0)));
  const u16 port = relay.localPort();
  CHECK(port > 0);

  const std::string tag = VoiceRelay::RoomTagHex("room-1", RoomSecret());
  const std::string auth = VoiceRelay::RegisterAuthHex(tag, RoomSecret());

  // Two registered peers (their own sockets, so the relay learns both addresses).
  UdpSocket peerA, peerB;
  CHECK(peerA.Bind(0, "127.0.0.1"));
  CHECK(peerB.Bind(0, "127.0.0.1"));
  std::string reg;
  reg.append(VoiceRelay::Magic(), 4);
  reg.push_back((char)VoiceRelay::kVersion);
  reg.push_back((char)VoiceRelay::kKindRegister);
  reg.append(tag);
  reg.append(auth);
  CHECK(peerA.SendTo(reg.data(), reg.size(), "127.0.0.1", port));
  CHECK(peerB.SendTo(reg.data(), reg.size(), "127.0.0.1", port));
  relay.Pump(50);
  CHECK(relay.peerCount() == 2);

  // A data datagram from A is forwarded to B, not echoed to A.
  std::string data;
  data.append(VoiceRelay::Magic(), 4);
  data.push_back((char)VoiceRelay::kVersion);
  data.push_back((char)VoiceRelay::kKindData);
  data.append(tag);
  data.append("payload-bytes");
  CHECK(peerA.SendTo(data.data(), data.size(), "127.0.0.1", port));
  relay.Pump(50);

  char buf[256];
  std::string from;
  u16 fromPort = 0;
  const ssize_t nB = peerB.RecvFrom(buf, sizeof buf, &from, &fromPort, 200);
  CHECK(nB == (ssize_t)data.size());
  if (nB > 0) CHECK(std::string(buf, (size_t)nB) == data);
  // A must NOT have received its own datagram back.
  CHECK(peerA.RecvFrom(buf, sizeof buf, &from, &fromPort, 50) == 0);

  // A data datagram from an *unregistered* source is dropped.
  const u64 fwdBefore = relay.stats().datagramsForwarded;
  CHECK(SendRaw(port, data));
  relay.Pump(50);
  CHECK(relay.stats().datagramsForwarded == fwdBefore);
  CHECK(peerB.RecvFrom(buf, sizeof buf, &from, &fromPort, 50) == 0);

  // A register with a wrong auth is dropped.
  std::string badReg;
  badReg.append(VoiceRelay::Magic(), 4);
  badReg.push_back((char)VoiceRelay::kVersion);
  badReg.push_back((char)VoiceRelay::kKindRegister);
  badReg.append(tag);
  badReg.append(std::string(VoiceRelay::kAuthBytes, '0'));
  CHECK(SendRaw(port, badReg));
  relay.Pump(50);
  CHECK(relay.peerCount() == 2);  // no new peer

  // A datagram for an unknown room is dropped.
  std::string unknown;
  unknown.append(VoiceRelay::Magic(), 4);
  unknown.push_back((char)VoiceRelay::kVersion);
  unknown.push_back((char)VoiceRelay::kKindData);
  unknown.append(std::string(VoiceRelay::kTagBytes, 'a'));
  unknown.append("x");
  const u64 droppedBefore = relay.stats().datagramsDropped;
  CHECK(SendRaw(port, unknown));
  relay.Pump(50);
  CHECK(relay.stats().datagramsDropped > droppedBefore);

  relay.Close();
}

static void TestRelayPeerEviction() {
  VoiceRelay relay;
  CHECK(relay.AddRoom("room-1", RoomSecret()));
  VoiceRelayConfig cfg = RelayConfig(0);
  cfg.peerTtlMs = 50;
  CHECK(relay.Open(cfg));
  const u16 port = relay.localPort();

  const std::string tag = VoiceRelay::RoomTagHex("room-1", RoomSecret());
  const std::string auth = VoiceRelay::RegisterAuthHex(tag, RoomSecret());
  std::string reg;
  reg.append(VoiceRelay::Magic(), 4);
  reg.push_back((char)VoiceRelay::kVersion);
  reg.push_back((char)VoiceRelay::kKindRegister);
  reg.append(tag);
  reg.append(auth);
  CHECK(SendRaw(port, reg));
  relay.Pump(50);
  CHECK(relay.peerCount() == 1);
  // After the TTL passes with no datagram, the peer is evicted.
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  relay.Pump(0);
  CHECK(relay.peerCount() == 0);
  CHECK(relay.stats().peersEvicted >= 1);
  relay.Close();
}

static void TestRelayThreaded() {
  VoiceRelay relay;
  CHECK(relay.AddRoom("room-1", RoomSecret()));
  CHECK(relay.Open(RelayConfig(0)));
  CHECK(relay.Start());
  const u16 port = relay.localPort();

  const std::string tag = VoiceRelay::RoomTagHex("room-1", RoomSecret());
  const std::string auth = VoiceRelay::RegisterAuthHex(tag, RoomSecret());
  UdpSocket a, b;
  CHECK(a.Bind(0, "127.0.0.1"));
  CHECK(b.Bind(0, "127.0.0.1"));
  std::string reg;
  reg.append(VoiceRelay::Magic(), 4);
  reg.push_back((char)VoiceRelay::kVersion);
  reg.push_back((char)VoiceRelay::kKindRegister);
  reg.append(tag);
  reg.append(auth);
  CHECK(a.SendTo(reg.data(), reg.size(), "127.0.0.1", port));
  CHECK(b.SendTo(reg.data(), reg.size(), "127.0.0.1", port));

  std::string data;
  data.append(VoiceRelay::Magic(), 4);
  data.push_back((char)VoiceRelay::kVersion);
  data.push_back((char)VoiceRelay::kKindData);
  data.append(tag);
  data.append("threaded-payload");

  char buf[256];
  std::string from;
  u16 fromPort = 0;
  bool got = false;
  for (int i = 0; i < 100 && !got; ++i) {
    a.SendTo(data.data(), data.size(), "127.0.0.1", port);
    const ssize_t n = b.RecvFrom(buf, sizeof buf, &from, &fromPort, 20);
    if (n > 0) got = std::string(buf, (size_t)n) == data;
  }
  CHECK(got);
  relay.Stop();
  relay.Close();
}

// The end-to-end path: two transports through one relay.
static void TestRelayTransportRoundTrip() {
  VoiceIdentity idA = VoiceIdentity::FromSeed(Seed(1));
  VoiceIdentity idB = VoiceIdentity::FromSeed(Seed(2));

  VoiceRelay relay;
  CHECK(relay.AddRoom("room-1", RoomSecret()));
  CHECK(relay.Open(RelayConfig(0)));
  const u16 relayPort = relay.localPort();
  CHECK(relayPort > 0);

  VoiceRelayTransport a, b;
  CHECK(a.Open(TransportConfig(relayPort), idA, AnchorFor(idB)));
  CHECK(b.Open(TransportConfig(relayPort), idB, AnchorFor(idA)));
  CHECK(a.open() && b.open());

  bool settled = false;
  for (int i = 0; i < 400 && !settled; ++i) {
    a.Poll(nullptr, 5);
    b.Poll(nullptr, 5);
    relay.Pump(5);
    settled = a.channel().sessionCount() > 0 && b.channel().sessionCount() > 0;
  }
  CHECK(settled);
  CHECK(a.channel().sessionCount() == 1);
  CHECK(b.channel().sessionCount() == 1);

  std::vector<u8> adpcm(64);
  for (size_t i = 0; i < adpcm.size(); ++i) adpcm[i] = (u8)(i * 7 + 3);
  CHECK(a.SendFrame("alice", 1, adpcm));

  std::string gotSpeaker;
  std::vector<u8> gotFrame;
  for (int i = 0; i < 200 && gotFrame.empty(); ++i) {
    b.Poll([&](const std::string& sp, u32, const std::vector<u8>& f,
               const std::string&, u16) {
      if (gotFrame.empty()) { gotSpeaker = sp; gotFrame = f; }
    }, 5);
    relay.Pump(5);
  }
  CHECK(gotSpeaker == "alice");
  CHECK(gotFrame == adpcm);

  a.Close();
  b.Close();
  relay.Close();
}

// A third client that the others do not trust cannot inject into their audio.
//
// The trust anchor is directional, so the test checks both halves of the
// property:
//   * a peer that trusts nobody establishes no session at all, so it cannot even
//     seal a frame ("can't listen");
//   * a peer that trusts A (so it may listen to A) still cannot inject into A or
//     B, because neither of them trusts it, so its sealed frames are dropped.
static void TestRelayUntrustedPeerCannotInject() {
  VoiceIdentity idA = VoiceIdentity::FromSeed(Seed(1));
  VoiceIdentity idB = VoiceIdentity::FromSeed(Seed(2));
  VoiceIdentity idC = VoiceIdentity::FromSeed(Seed(3));

  VoiceRelay relay;
  CHECK(relay.AddRoom("room-1", RoomSecret()));
  CHECK(relay.Open(RelayConfig(0)));
  const u16 relayPort = relay.localPort();

  // --- Scenario 1: C trusts only an unrelated identity D, so it establishes no
  // session with A or B at all, and cannot even seal a frame. ---
  {
    VoiceIdentity idD = VoiceIdentity::FromSeed(Seed(9));
    VoiceRelayTransport a, b, c;
    CHECK(a.Open(TransportConfig(relayPort), idA, AnchorFor(idB)));
    CHECK(b.Open(TransportConfig(relayPort), idB, AnchorFor(idA)));
    CHECK(c.Open(TransportConfig(relayPort), idC, AnchorFor(idD)));

    for (int i = 0; i < 200; ++i) {
      a.Poll(nullptr, 5);
      b.Poll(nullptr, 5);
      c.Poll(nullptr, 5);
      relay.Pump(5);
    }
    CHECK(a.channel().sessionCount() == 1);  // only B
    CHECK(b.channel().sessionCount() == 1);  // only A
    CHECK(c.channel().sessionCount() == 0);  // trusts nobody
    std::vector<u8> adpcm(32, 0x11);
    CHECK(!c.SendFrame("mallory", 1, adpcm));  // cannot seal without a session

    // Nothing C sent (or tried to) reaches A or B.
    size_t delivered = 0;
    auto count = [&](const std::string& sp, u32, const std::vector<u8>&,
                     const std::string&, u16) {
      if (sp == "mallory") ++delivered;
    };
    for (int i = 0; i < 60; ++i) {
      a.Poll(count, 5);
      b.Poll(count, 5);
      relay.Pump(5);
    }
    CHECK(delivered == 0);
    a.Close();
    b.Close();
    c.Close();
  }

  // --- Scenario 2: C trusts A (it may listen to A), but A and B do not trust C,
  // so C can never inject into their audio. ---
  {
    VoiceRelayTransport a, b, c;
    CHECK(a.Open(TransportConfig(relayPort), idA, AnchorFor(idB)));
    CHECK(b.Open(TransportConfig(relayPort), idB, AnchorFor(idA)));
    CHECK(c.Open(TransportConfig(relayPort), idC, AnchorFor(idA)));

    for (int i = 0; i < 200; ++i) {
      a.Poll(nullptr, 5);
      b.Poll(nullptr, 5);
      c.Poll(nullptr, 5);
      relay.Pump(5);
    }
    // A and B trust only each other; C is never a session for either.
    CHECK(a.channel().sessionCount() == 1);
    CHECK(b.channel().sessionCount() == 1);

    std::vector<u8> adpcm(32, 0x22);
    // C may seal a frame (it has a session with A), but A and B drop it.
    c.SendFrame("mallory", 1, adpcm);
    size_t delivered = 0;
    auto count = [&](const std::string& sp, u32, const std::vector<u8>&,
                     const std::string&, u16) {
      if (sp == "mallory") ++delivered;
    };
    for (int i = 0; i < 80; ++i) {
      a.Poll(count, 5);
      b.Poll(count, 5);
      relay.Pump(5);
    }
    CHECK(delivered == 0);
    a.Close();
    b.Close();
    c.Close();
  }

  relay.Close();
}

int main() {
  TestRoomTagDerivation();
  TestRelayRegistrationAndForwarding();
  TestRelayPeerEviction();
  TestRelayThreaded();
  TestRelayTransportRoundTrip();
  TestRelayUntrustedPeerCannotInject();

  if (g_fail == 0) {
    printf("test_voice_relay: ALL OK\n");
    return 0;
  }
  printf("test_voice_relay: %d FAILURES\n", g_fail);
  return 1;
}
