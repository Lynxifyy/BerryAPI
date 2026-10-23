// Host tests for the LAN cosmetic transport: the UDP socket primitive and the
// fragment/reassembly transport over multicast loopback.
//
// Covers:
//   * UdpSocket unicast loopback (send/receive, sender address, timeout) and
//     multicast loopback with two sockets sharing a port,
//   * a multi-fragment message surviving fragmentation and reassembly intact,
//     including when the sender and receiver use different MTUs,
//   * the sender tag: a client skips the copy of its own datagram that
//     multicast loops back, even before it announces,
//   * a direct unicast send to a known peer (P2P),
//   * peer discovery from announce datagrams and the TTL,
//   * mDNS discovery: publish/query/browse over `_berry._udp.local`, the SRV/TXT/A
//     records, and the cache TTL,
//   * a partial message expiring on the reassembly TTL (injected clock),
//   * the fail-closed cases: a bad MTU, a send while closed, an oversized
//     message, and a malformed fragment header.
#include "berry/lan_transport.h"
#include "berry/lan_discovery.h"
#include "berry/net.h"
#include "berry/cosmetic.h"
#include "berry/cosmetic_sync.h"
#include "berry/manifest.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

std::string Blob(size_t n, char seed) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) s[i] = (char)((seed + i * 31) & 0x7f);
  return s;
}

/// Builds a raw transport datagram, so a test can inject a fragment the
/// transport did not send (the wire format is pinned here independently).
std::string MakeDatagram(u8 kind, u16 msgId, u16 index, u16 count, u32 totalLen,
                         u16 payloadPer, u32 tag, const std::string& payload) {
  std::string d(24, '\0');
  std::memcpy(&d[0], "BLAN", 4);
  d[4] = 1;               // version
  d[5] = (char)kind;
  d[6] = (char)(msgId >> 8); d[7] = (char)msgId;
  d[8] = (char)(index >> 8); d[9] = (char)index;
  d[10] = (char)(count >> 8); d[11] = (char)count;
  d[12] = (char)(totalLen >> 24); d[13] = (char)(totalLen >> 16);
  d[14] = (char)(totalLen >> 8); d[15] = (char)totalLen;
  d[16] = (char)(payloadPer >> 8); d[17] = (char)payloadPer;
  // 18..19 reserved
  d[20] = (char)(tag >> 24); d[21] = (char)(tag >> 16);
  d[22] = (char)(tag >> 8); d[23] = (char)tag;
  d += payload;
  return d;
}

LanTransportConfig TestConfig(const char* group, u16 port) {
  LanTransportConfig cfg;
  cfg.group = group;
  cfg.port = port;
  cfg.iface = "127.0.0.1";
  return cfg;
}

/// Polls `t` until `out` is non-empty or the attempts run out, so delivery
/// timing does not make the test flaky.
void PollForOne(LanCosmeticTransport& t, std::string* out, int attempts = 60) {
  for (int i = 0; i < attempts && out->empty(); ++i) {
    t.Poll([&](const std::string& m, const std::string&, u16) {
      if (out->empty()) *out = m;
    }, 20);
  }
}

}  // namespace

int main() {
  // --- UdpSocket: unicast loopback ---
  {
    UdpSocket a, b;
    CHECK(a.Bind(0, "127.0.0.1"));
    CHECK(b.Bind(0, "127.0.0.1"));
    CHECK(a.valid() && b.valid());
    CHECK(a.localPort() != 0 && b.localPort() != 0);

    CHECK(a.SendTo("hello", 5, "127.0.0.1", b.localPort()));
    char buf[64];
    std::string host;
    u16 port = 0;
    ssize_t n = b.RecvFrom(buf, sizeof buf, &host, &port, 500);
    CHECK(n == 5);
    CHECK(std::string(buf, (size_t)(n > 0 ? n : 0)) == "hello");
    CHECK(host == "127.0.0.1");
    CHECK(port == a.localPort());

    // A quiet socket times out (0), it does not block or error.
    CHECK(b.RecvFrom(buf, sizeof buf, nullptr, nullptr, 20) == 0);
    // Sending on an unbound socket fails closed.
    UdpSocket unbound;
    CHECK(!unbound.SendTo("x", 1, "127.0.0.1", b.localPort()));
  }

  // --- UdpSocket: multicast loopback, two sockets on one port ---
  {
    UdpSocket a, b;
    CHECK(a.Bind(47898, "0.0.0.0", true));
    CHECK(b.Bind(47898, "0.0.0.0", true));   // SO_REUSEADDR allows the shared port
    CHECK(a.JoinMulticast("239.66.0.8", "127.0.0.1"));
    CHECK(b.JoinMulticast("239.66.0.8", "127.0.0.1"));
    CHECK(a.SetMulticastInterface("127.0.0.1"));
    // A bad group is refused, not silently accepted.
    CHECK(!a.JoinMulticast("10.0.0.1", "127.0.0.1"));

    CHECK(a.SendTo("mcast", 5, "239.66.0.8", 47898));
    char buf[64];
    ssize_t n = b.RecvFrom(buf, sizeof buf, nullptr, nullptr, 500);
    CHECK(n == 5);
    CHECK(std::string(buf, (size_t)(n > 0 ? n : 0)) == "mcast");
  }

  // --- Transport: a multi-fragment message survives intact ---
  const char* kGroup = "239.66.0.9";
  const u16 kPort = 47899;
  {
    LanTransportConfig cfg = TestConfig(kGroup, kPort);
    LanCosmeticTransport A, B;
    CHECK(A.Open(cfg));
    CHECK(B.Open(cfg));
    CHECK(A.multicastJoined() && B.multicastJoined());
    CHECK(A.localPort() == kPort);
    CHECK(LanCosmeticTransport::kHeaderBytes == 24);
    CHECK(std::string(LanCosmeticTransport::Magic()) == "BLAN");

    // Send before any announce: the tag is set at Open, so A still skips its
    // own looped-back datagram even though it never announced. B receives it,
    // which also confirms the message path works before discovery.
    const std::string pre = Blob(300, 'p');
    CHECK(A.Send(pre));
    bool preGot = false;
    for (int i = 0; i < 5; ++i) {
      A.Poll([&](const std::string&, const std::string&, u16) { preGot = true; }, 10);
    }
    CHECK(!preGot);
    CHECK(A.stats().selfSkipped >= 1);
    std::string preAtB;
    PollForOne(B, &preAtB);
    CHECK(preAtB == pre);

    A.Announce("alice");
    B.Announce("bob");

    // 5000 bytes over a 1200-byte MTU is five fragments.
    const std::string msg = Blob(5000, 'x');
    CHECK(A.Send(msg));

    std::string got;
    PollForOne(B, &got);
    CHECK(got.size() == msg.size());
    CHECK(got == msg);
    CHECK(B.stats().messagesReceived == 2);   // the pre-announce message and this

    // A's own datagram loops back to it and is skipped by the sender tag.
    bool selfGot = false;
    for (int i = 0; i < 5; ++i) {
      A.Poll([&](const std::string&, const std::string&, u16) { selfGot = true; }, 10);
    }
    CHECK(!selfGot);
    CHECK(A.stats().selfSkipped >= 1);

    // Peer discovery: B saw A's announce.
    std::vector<LanPeer> peers = B.peers();
    CHECK(!peers.empty());
    bool sawAlice = false;
    for (const LanPeer& p : peers) {
      if (p.playerId == "alice") sawAlice = true;
    }
    CHECK(sawAlice);
  }

  // --- Transport: a direct unicast send to a known peer (P2P) ---
  {
    // B listens on its own port, so the unicast delivery is unambiguous (two
    // sockets sharing one port would race for it).
    LanTransportConfig aCfg = TestConfig("239.66.0.14", 47904);
    LanTransportConfig bCfg = TestConfig("239.66.0.15", 47905);
    LanCosmeticTransport A, B;
    CHECK(A.Open(aCfg));
    CHECK(B.Open(bCfg));

    // A sends straight to B's port on loopback, bypassing the group.
    const std::string direct = Blob(2500, 'd');
    CHECK(A.SendTo("127.0.0.1", bCfg.port, direct));
    std::string got;
    PollForOne(B, &got);
    CHECK(got == direct);
    // A bad destination fails closed.
    CHECK(!A.SendTo("not-an-ip", bCfg.port, direct));
  }

  // --- Transport: sender and receiver with different MTUs ---
  {
    // The sender's fragment size is on the wire, so a receiver with a different
    // MTU still computes the right offsets. Before that field, this case would
    // have silently corrupted the message.
    LanTransportConfig aCfg = TestConfig("239.66.0.16", 47906);
    aCfg.mtu = 400;
    LanTransportConfig bCfg = TestConfig("239.66.0.17", 47907);
    bCfg.mtu = 1200;
    LanCosmeticTransport A, B;
    CHECK(A.Open(aCfg));
    CHECK(B.Open(bCfg));

    const std::string msg = Blob(3000, 'm');
    CHECK(A.SendTo("127.0.0.1", bCfg.port, msg));
    std::string got;
    PollForOne(B, &got);
    CHECK(got == msg);
  }

  // --- Transport: a partial message expires on the reassembly TTL ---
  {
    LanTransportConfig cfg = TestConfig("239.66.0.10", 47900);
    cfg.reassemblyTtlMs = 1000;
    LanCosmeticTransport T;
    CHECK(T.Open(cfg));

    u64 now = 5000;
    T.SetClock([&]() { return now; });

    UdpSocket raw;
    CHECK(raw.Bind(0, "127.0.0.1"));

    // Only fragment 0 of a 2-fragment message: it must never be delivered.
    // The default MTU is 1200, so a full fragment carries 1200-24 = 1176 bytes.
    const std::string frag0 = MakeDatagram(1, 7, 0, 2, 2000, 1176, 0, Blob(1176, 'y'));
    CHECK(raw.SendTo(frag0.data(), frag0.size(), "127.0.0.1", cfg.port));

    bool got = false;
    T.Poll([&](const std::string&, const std::string&, u16) { got = true; }, 100);
    CHECK(!got);
    CHECK(T.stats().messagesReceived == 0);

    // A malformed fragment (index past the count) is dropped, not stored.
    const std::string bad = MakeDatagram(1, 8, 5, 2, 2000, 1176, 0, Blob(100, 'z'));
    CHECK(raw.SendTo(bad.data(), bad.size(), "127.0.0.1", cfg.port));
    T.Poll([&](const std::string&, const std::string&, u16) { got = true; }, 100);
    CHECK(!got);
    CHECK(T.stats().fragmentsDropped >= 1);

    // Past the TTL the partial is dropped.
    now += cfg.reassemblyTtlMs + 1;
    T.Poll([&](const std::string&, const std::string&, u16) { got = true; }, 100);
    CHECK(!got);
    CHECK(T.stats().messagesDropped >= 1);
  }

  // --- Fail-closed: bad MTU, closed send, oversized message ---
  {
    LanTransportConfig bad = TestConfig("239.66.0.11", 47901);
    bad.mtu = 10;   // smaller than the header
    LanCosmeticTransport t;
    CHECK(!t.Open(bad));
    CHECK(!t.open());

    // Send while closed fails closed.
    CHECK(!t.Send("x"));
    CHECK(!t.Announce("nobody"));

    LanTransportConfig cfg = TestConfig("239.66.0.12", 47902);
    cfg.maxMessageBytes = 512;
    LanCosmeticTransport u;
    CHECK(u.Open(cfg));
    CHECK(!u.Send(Blob(513, 'a')));   // over the cap
    CHECK(u.stats().messagesDropped >= 1);
    CHECK(!u.Send(""));               // empty is refused
  }

  // --- End to end: a real signed envelope over the real transport ---
  {
    CosmeticRegistry::Instance().Clear();
    CosmeticRegistry::Instance().RegisterBuiltins();
    CosmeticOwnership::Instance().Clear();

    std::string pub, sec;
    const std::string seed(32, 'S');
    CHECK(crypto::Ed25519KeypairFromSeed(seed, &pub, &sec));

    CosmeticLoadout lo;
    CHECK(lo.Equip("builtin.cape.red"));
    std::string envelope;
    CHECK(CosmeticSyncSignEd25519("alice", lo, sec, &envelope));

    LanTransportConfig cfg = TestConfig("239.66.0.13", 47903);
    LanCosmeticTransport sender, receiver;
    CHECK(sender.Open(cfg));
    CHECK(receiver.Open(cfg));
    sender.Announce("alice");
    receiver.Announce("bob");

    // The receiver's channel trusts alice's public key and hands verified
    // messages to a service that grants alice the cape.
    CosmeticOwnership::Instance().Grant("alice", "builtin.cape.red");
    CosmeticService svc;
    CosmeticSyncChannel ch;
    ch.SetVerifyKey(CosmeticSyncKey::Ed25519PublicKey(crypto::ToHexLower(pub)));
    ch.SetTransport(&receiver);   // publishing would go back out the wire
    CHECK(ch.configured());

    CHECK(sender.Send(envelope));

    size_t applied = 0;
    for (int i = 0; i < 60 && applied == 0; ++i) {
      receiver.Poll([&](const std::string& m, const std::string&, u16) {
        if (applied != 0) return;
        size_t n = 0;
        if (ch.Receive(m, &svc, &n) == CosmeticSyncResult::Ok) applied = n;
      }, 20);
    }
    CHECK(applied == 1);          // the cape survived the ownership gate
    CHECK(ch.accepted() == 1);
    CHECK(svc.RemotePlayers().size() == 1);
  }

  // --- Discovery: publish, query, browse over mDNS loopback ---
  {
    LanDiscoveryConfig cfg;
    cfg.serviceType = "_berry._udp.local";
    cfg.iface = "127.0.0.1";
    LanDiscovery publisher, browser;
    CHECK(publisher.Open(cfg));
    CHECK(browser.Open(cfg));
    CHECK(publisher.multicastJoined() && browser.multicastJoined());

    // Pin the browser's clock before anything is cached, so the TTL is
    // testable without waiting on wall time.
    u64 now = 10000;
    browser.SetClock([&]() { return now; });

    // The browser publishes nothing; it only queries. The publisher answers.
    CHECK(publisher.Publish("alice", 47823, "id=alice"));

    bool found = false;
    LanService svc;
    for (int i = 0; i < 40 && !found; ++i) {
      browser.Query();
      publisher.Poll(5);   // the publisher sees the query and answers
      browser.Poll(20);
      for (const LanService& s : browser.services()) {
        if (s.instance.find("alice") != std::string::npos && s.port == 47823) {
          found = true;
          svc = s;
        }
      }
    }
    CHECK(found);
    CHECK(svc.host == "alice.local");
    CHECK(svc.address == "127.0.0.1");
    CHECK(svc.txt.find("id=alice") != std::string::npos);

    // The instance name carries the service type; a name that already has it is
    // left alone.
    CHECK(publisher.FullInstanceName("bob") == "bob._berry._udp.local");
    CHECK(publisher.FullInstanceName("x._berry._udp.local") == "x._berry._udp.local");

    // A service past the cache TTL is dropped.
    CHECK(!browser.services().empty());
    now += cfg.cacheTtlMs + 1;
    browser.Poll(20);
    CHECK(browser.services().empty());
    CHECK(browser.stats().servicesExpired >= 1);

    // Closed discovery fails closed.
    LanDiscovery closed;
    CHECK(!closed.Publish("x", 1));
    CHECK(!closed.Query());
  }

  if (g_fail == 0) printf("test_lan_transport: ALL OK\n");
  return g_fail;
}
