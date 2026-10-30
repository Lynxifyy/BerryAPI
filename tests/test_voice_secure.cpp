// Host tests for the authenticated, encrypted LAN voice transport
// (include/berry/voice_secure_transport.h).
//
// Two real transports run on the loopback multicast group: each is configured
// with its own Ed25519 identity and a trust anchor holding the other's key. The
// test drives the real announce -> verify -> X25519 -> HKDF -> ChaCha20-Poly1305
// path over real UDP datagrams, and checks that:
//   * a peer that is *not* in the anchor cannot establish a session, so its
//     sealed frames never arrive;
//   * a sealed frame round-trips (the audio reaches the listener);
//   * a frame whose ciphertext, header (AAD), or route tag is tampered with is
//     dropped;
//   * a replayed frame is dropped;
//   * a datagram with the wrong magic is dropped;
//   * a transport configured with an unconfigured channel refuses to open.
#include "berry/voice_secure_transport.h"

#include <cstdio>
#include <string>
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

// Drives both transports until each has a session (announces cross).
bool Settle(SecureVoiceTransport* a, SecureVoiceTransport* b, int rounds = 200) {
  for (int i = 0; i < rounds; ++i) {
    a->Poll(nullptr, 5);
    b->Poll(nullptr, 5);
    if (a->channel().sessionCount() > 0 && b->channel().sessionCount() > 0) {
      return true;
    }
  }
  return false;
}

SecureVoiceConfig LoopbackConfig(u16 port) {
  SecureVoiceConfig cfg;
  cfg.group = "239.66.0.77";
  cfg.port = port;
  cfg.iface = "127.0.0.1";
  cfg.announceIntervalMs = 1000;
  return cfg;
}

}  // namespace

static void TestSecureRoundTrip() {
  VoiceIdentity idA = VoiceIdentity::FromSeed(Seed(1));
  VoiceIdentity idB = VoiceIdentity::FromSeed(Seed(2));

  SecureVoiceTransport a, b;
  CHECK(a.Open(LoopbackConfig(47951), idA, AnchorFor(idB)));
  CHECK(b.Open(LoopbackConfig(47951), idB, AnchorFor(idA)));
  CHECK(a.open() && b.open());

  CHECK(Settle(&a, &b));
  CHECK(a.channel().sessionCount() == 1);
  CHECK(b.channel().sessionCount() == 1);

  // A sends a frame; B must receive the exact ADPCM bytes.
  std::vector<u8> adpcm(80);
  for (size_t i = 0; i < adpcm.size(); ++i) adpcm[i] = (u8)(i * 13 + 5);
  CHECK(a.SendFrame("alice", 1, adpcm));

  std::string gotSpeaker;
  std::vector<u8> gotFrame;
  for (int i = 0; i < 100 && gotFrame.empty(); ++i) {
    b.Poll([&](const std::string& sp, u32, const std::vector<u8>& f,
               const std::string&, u16) {
      if (gotFrame.empty()) { gotSpeaker = sp; gotFrame = f; }
    }, 10);
  }
  CHECK(gotSpeaker == "alice");
  CHECK(gotFrame == adpcm);
  CHECK(b.stats().framesReceived >= 1);
  CHECK(a.stats().framesSent >= 1);

  // Several frames in a row all arrive (the per-session counter advances).
  for (int k = 0; k < 5; ++k) {
    CHECK(a.SendFrame("alice", (u32)(k + 2), adpcm));
  }
  size_t delivered = 0;
  for (int i = 0; i < 100 && delivered < 5; ++i) {
    b.Poll([&](const std::string&, u32, const std::vector<u8>&,
               const std::string&, u16) { ++delivered; }, 10);
  }
  CHECK(delivered == 5);

  a.Close();
  b.Close();
}

static void TestUntrustedPeerCannotInject() {
  VoiceIdentity idA = VoiceIdentity::FromSeed(Seed(3));
  VoiceIdentity idB = VoiceIdentity::FromSeed(Seed(4));
  VoiceIdentity idEve = VoiceIdentity::FromSeed(Seed(5));

  SecureVoiceTransport a, b, eve;
  CHECK(a.Open(LoopbackConfig(47952), idA, AnchorFor(idB)));
  CHECK(b.Open(LoopbackConfig(47952), idB, AnchorFor(idA)));
  // Eve trusts nobody relevant and is trusted by nobody. Her channel configures
  // (she has her own anchor) but A and B do not hold her key.
  CHECK(eve.Open(LoopbackConfig(47952), idEve, AnchorFor(idA)));
  CHECK(Settle(&a, &b));
  CHECK(a.channel().sessionCount() == 1);
  CHECK(b.channel().sessionCount() == 1);
  // Eve never establishes a session with A or B: her announces are refused.
  CHECK(eve.channel().sessionCount() == 0);
  CHECK(eve.stats().announcesRejected > 0 || a.stats().announcesRejected > 0);

  // Eve cannot send a frame that A accepts: she has no session, so SendFrame
  // refuses.
  std::vector<u8> adpcm(64, 0x41);
  CHECK(!eve.SendFrame("eve", 1, adpcm));

  // A's real frame still reaches B.
  CHECK(a.SendFrame("alice", 1, adpcm));
  size_t delivered = 0;
  for (int i = 0; i < 100 && delivered == 0; ++i) {
    b.Poll([&](const std::string&, u32, const std::vector<u8>&,
               const std::string&, u16) { ++delivered; }, 10);
  }
  CHECK(delivered == 1);

  // A datagram with the wrong magic is dropped by both.
  const char junk[32] = {'X', 'X', 'X', 'X', 1, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(a.socket().SendTo(junk, 14, "127.0.0.1", b.localPort()));
  for (int i = 0; i < 20; ++i) {
    b.Poll([&](const std::string&, u32, const std::vector<u8>&, const std::string&,
               u16) { ++delivered; }, 5);
  }
  CHECK(delivered == 1);   // the junk did not add a frame

  a.Close();
  b.Close();
  eve.Close();
}

static void TestTamperAndReplayRefused() {
  VoiceIdentity idA = VoiceIdentity::FromSeed(Seed(6));
  VoiceIdentity idB = VoiceIdentity::FromSeed(Seed(7));
  const u16 port = 47953;
  SecureVoiceTransport a, b;
  CHECK(a.Open(LoopbackConfig(port), idA, AnchorFor(idB)));
  CHECK(b.Open(LoopbackConfig(port), idB, AnchorFor(idA)));
  CHECK(Settle(&a, &b));

  // A third socket on the same group/port captures the raw datagrams A sends.
  // SO_REUSEADDR (set by UdpSocket::Bind) lets it share the port with A and B.
  UdpSocket sniffer;
  CHECK(sniffer.Bind(port, "0.0.0.0", true));
  CHECK(sniffer.JoinMulticast("239.66.0.77", "127.0.0.1"));
  CHECK(sniffer.SetMulticastInterface("127.0.0.1"));

  std::vector<u8> adpcm(48, 0x22);
  CHECK(a.SendFrame("alice", 1, adpcm));

  // Capture the sealed datagram and let B receive the legitimate frame, so the
  // tamper checks below start from an empty inbox.
  std::string raw;
  size_t delivered = 0;
  for (int i = 0; i < 80 && (raw.empty() || delivered == 0); ++i) {
    std::vector<char> buf(1400);
    std::string host;
    u16 fromPort = 0;
    const ssize_t n = sniffer.RecvFrom(buf.data(), buf.size(), &host, &fromPort, 5);
    if (n > 0 && (u8)buf[5] == SecureVoiceTransport::kKindSealedFrame) {
      raw.assign(buf.data(), (size_t)n);
    }
    b.Poll([&](const std::string&, u32, const std::vector<u8>&,
               const std::string&, u16) { ++delivered; }, 5);
  }
  CHECK(!raw.empty());
  CHECK(delivered == 1);
  delivered = 0;

  // 1) A flipped ciphertext byte is refused.
  std::string tampered = raw;
  tampered[tampered.size() - 1] ^= 1;
  CHECK(sniffer.SendTo(tampered.data(), tampered.size(), "239.66.0.77", port));
  for (int i = 0; i < 30; ++i) {
    b.Poll([&](const std::string&, u32, const std::vector<u8>&,
               const std::string&, u16) { ++delivered; }, 5);
  }
  CHECK(delivered == 0);   // tampered ciphertext dropped

  // 2) A flipped route-tag byte is refused (no session matches / AEAD AAD).
  std::string badTag = raw;
  badTag[SecureVoiceTransport::kPrefixBytes] ^= 1;
  CHECK(sniffer.SendTo(badTag.data(), badTag.size(), "239.66.0.77", port));
  for (int i = 0; i < 30; ++i) {
    b.Poll([&](const std::string&, u32, const std::vector<u8>&,
               const std::string&, u16) { ++delivered; }, 5);
  }
  CHECK(delivered == 0);

  // 3) A flipped sequence byte changes the AAD, so the tag fails.
  std::string badSeq = raw;
  badSeq[SecureVoiceTransport::kPrefixBytes + SecureVoiceTransport::kRouteTagBytes] ^= 1;
  CHECK(sniffer.SendTo(badSeq.data(), badSeq.size(), "239.66.0.77", port));
  for (int i = 0; i < 30; ++i) {
    b.Poll([&](const std::string&, u32, const std::vector<u8>&,
               const std::string&, u16) { ++delivered; }, 5);
  }
  CHECK(delivered == 0);

  // 4) The original frame, replayed from the sniffer, is dropped by the replay
  //    window (B already opened it when A first sent it).
  CHECK(sniffer.SendTo(raw.data(), raw.size(), "239.66.0.77", port));
  for (int i = 0; i < 30; ++i) {
    b.Poll([&](const std::string&, u32, const std::vector<u8>&,
               const std::string&, u16) { ++delivered; }, 5);
  }
  CHECK(delivered == 0);

  // A fresh frame from A still arrives (the tamper/replay attempts did not wedge
  // the session).
  CHECK(a.SendFrame("alice", 2, adpcm));
  for (int i = 0; i < 60 && delivered == 0; ++i) {
    b.Poll([&](const std::string&, u32, const std::vector<u8>&,
               const std::string&, u16) { ++delivered; }, 10);
  }
  CHECK(delivered == 1);

  a.Close();
  b.Close();
}

static void TestOpenRefusesUnconfiguredChannel() {
  VoiceIdentity idA = VoiceIdentity::FromSeed(Seed(8));
  SecureVoiceTransport a;
  // An empty anchor accepts nothing, so Configure fails and Open must refuse.
  CHECK(!a.Open(LoopbackConfig(47954), idA, VoiceTrustAnchor{}));
  CHECK(!a.open());

  // A verification-only identity cannot sign, so the channel refuses it.
  VoiceIdentity pubOnly = VoiceIdentity::FromPublicKey(idA.publicKey());
  SecureVoiceTransport b;
  CHECK(!b.Open(LoopbackConfig(47954), pubOnly, AnchorFor(idA)));
  CHECK(!b.open());

  // A room-key anchor works and lets two peers talk.
  VoiceTrustAnchor room;
  CHECK(room.SetRoomKey(std::string(32, 'Q')));
  VoiceIdentity idB = VoiceIdentity::FromSeed(Seed(9));
  SecureVoiceTransport c, d;
  CHECK(c.Open(LoopbackConfig(47955), idA, room));
  CHECK(d.Open(LoopbackConfig(47955), idB, room));
  CHECK(Settle(&c, &d));
  CHECK(c.channel().sessionCount() == 1);
  CHECK(d.channel().sessionCount() == 1);
  std::vector<u8> adpcm(32, 0x55);
  CHECK(c.SendFrame("carol", 1, adpcm));
  size_t delivered = 0;
  for (int i = 0; i < 80 && delivered == 0; ++i) {
    d.Poll([&](const std::string&, u32, const std::vector<u8>&,
               const std::string&, u16) { ++delivered; }, 10);
  }
  CHECK(delivered == 1);
  c.Close();
  d.Close();
}

int main() {
  TestSecureRoundTrip();
  TestUntrustedPeerCannotInject();
  TestTamperAndReplayRefused();
  TestOpenRefusesUnconfiguredChannel();

  if (g_fail == 0) {
    printf("test_voice_secure: ALL OK\n");
    return 0;
  }
  printf("test_voice_secure: %d FAILURES\n", g_fail);
  return 1;
}
