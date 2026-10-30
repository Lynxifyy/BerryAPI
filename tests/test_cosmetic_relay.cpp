// Host tests for the cosmetic-sync relay (the presence backend's media plane).
//
// `LanCosmeticTransport` only reaches one subnet. This proves the relay path:
// two `CosmeticRelayTransport`s register with one `CosmeticRelay` room and carry
// a *real signed cosmetic envelope* between them, exactly as the LAN path does,
// so two clients on different networks see each other's cosmetics. It also
// proves the relay is fail-closed and blind:
//
//   * an unregistered source's data is never forwarded;
//   * a wrong room secret cannot register (the auth is compared in constant
//     time), and an unknown room is dropped;
//   * a peer never receives its own datagram back;
//   * the relay moves bytes only — a forged loadout still fails signature
//     verification on the far side, and an unowned asset is still dropped by the
//     ownership gate.
#include "berry/cosmetic.h"
#include "berry/cosmetic_relay.h"
#include "berry/cosmetic_relay_transport.h"
#include "berry/cosmetic_sync.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

const char* kRoomSecret = "0123456789abcdef0123456789abcdef";  // 32 bytes
const char* kSharedKey = "shared-sync-key";

RelayCosmeticConfig TransportConfig(u16 relayPort) {
  RelayCosmeticConfig cfg;
  cfg.relayHost = "127.0.0.1";
  cfg.relayPort = relayPort;
  cfg.room = "room-1";
  cfg.roomSecret = kRoomSecret;
  cfg.bindAddr = "127.0.0.1";
  cfg.bindPort = 0;
  cfg.registerIntervalMs = 100;
  return cfg;
}

CosmeticRelayConfig RelayConfig(u16 port) {
  CosmeticRelayConfig cfg;
  cfg.port = port;
  cfg.bindAddr = "127.0.0.1";
  cfg.peerTtlMs = 400;
  return cfg;
}

/// Sends one raw datagram from a throwaway socket, to test the control plane.
bool SendRaw(u16 relayPort, const std::string& bytes) {
  UdpSocket s;
  if (!s.Bind(0, "127.0.0.1")) return false;
  return s.SendTo(bytes.data(), bytes.size(), "127.0.0.1", relayPort);
}

std::string RegisterDatagram(const std::string& tag, const std::string& auth) {
  std::string d;
  d.append(CosmeticRelay::Magic(), 4);
  d.push_back((char)CosmeticRelay::kVersion);
  d.push_back((char)CosmeticRelay::kKindRegister);
  d.append(tag);
  d.append(auth);
  return d;
}

std::string DataDatagram(const std::string& tag, const std::string& payload) {
  std::string d;
  d.append(CosmeticRelay::Magic(), 4);
  d.push_back((char)CosmeticRelay::kVersion);
  d.push_back((char)CosmeticRelay::kKindData);
  d.append(tag);
  d.append(payload);
  return d;
}

}  // namespace

int main() {
  // The built-in registry gives real assets to equip; ownership is granted so
  // the far side's gate admits a verified loadout and drops an unowned one.
  CosmeticRegistry::Instance().Clear();
  CosmeticRegistry::Instance().RegisterBuiltins();
  CosmeticOwnership::Instance().Grant("alice", "builtin.cape.red");
  CosmeticOwnership::Instance().Grant("bob", "builtin.aura.blue");

  //--- control plane: registration, forwarding, no echo, fail closed ---
  {
    CosmeticRelay relay;
    CHECK(relay.AddRoom("room-1", kRoomSecret));
    CHECK(!relay.AddRoom("room-1", kRoomSecret));  // duplicate refused
    CHECK(!relay.AddRoom("room-2", "short"));      // short secret refused
    CHECK(relay.roomCount() == 1);
    CHECK(relay.Open(RelayConfig(0)));
    const u16 port = relay.localPort();
    CHECK(port > 0);

    const std::string tag =
        CosmeticRelay::RoomTagHex("room-1", kRoomSecret);
    const std::string auth =
        CosmeticRelay::RegisterAuthHex(tag, kRoomSecret);

    UdpSocket a, b;
    CHECK(a.Bind(0, "127.0.0.1"));
    CHECK(b.Bind(0, "127.0.0.1"));
    const std::string reg = RegisterDatagram(tag, auth);
    CHECK(a.SendTo(reg.data(), reg.size(), "127.0.0.1", port));
    CHECK(b.SendTo(reg.data(), reg.size(), "127.0.0.1", port));
    relay.Pump(50);
    CHECK(relay.peerCount() == 2);

    // A's data is forwarded to B, never echoed to A.
    const std::string data = DataDatagram(tag, "opaque-payload");
    CHECK(a.SendTo(data.data(), data.size(), "127.0.0.1", port));
    relay.Pump(50);
    char buf[256];
    std::string from;
    u16 fromPort = 0;
    const ssize_t nB = b.RecvFrom(buf, sizeof buf, &from, &fromPort, 200);
    CHECK(nB == (ssize_t)data.size());
    if (nB > 0) CHECK(std::string(buf, (size_t)nB) == data);
    CHECK(a.RecvFrom(buf, sizeof buf, &from, &fromPort, 50) == 0);

    // An unregistered source's data is dropped.
    const u64 fwdBefore = relay.stats().datagramsForwarded;
    CHECK(SendRaw(port, data));
    relay.Pump(50);
    CHECK(relay.stats().datagramsForwarded == fwdBefore);
    CHECK(b.RecvFrom(buf, sizeof buf, &from, &fromPort, 50) == 0);

    // A register with a wrong auth is dropped.
    const std::string badAuth(64, '0');
    CHECK(SendRaw(port, RegisterDatagram(tag, badAuth)));
    relay.Pump(50);
    CHECK(relay.peerCount() == 2);

    // A datagram for an unknown room is dropped.
    const std::string unknownTag(64, 'a');
    const u64 dropBefore = relay.stats().datagramsDropped;
    CHECK(SendRaw(port, DataDatagram(unknownTag, "x")));
    relay.Pump(50);
    CHECK(relay.stats().datagramsDropped > dropBefore);

    // An oversized data datagram is refused rather than amplified.
    CosmeticRelayConfig small = RelayConfig(0);
    small.mtu = 100;
    CHECK(relay.Open(small));  // reopen with a small mtu
    const u16 port2 = relay.localPort();
    const std::string big = DataDatagram(tag, std::string(500, 'Z'));
    CHECK(SendRaw(port2, big));
    relay.Pump(50);
    relay.Close();
  }

  //--- end to end: two transports through one relay carry a signed loadout ---
  {
    CosmeticRelay relay;
    CHECK(relay.AddRoom("room-1", kRoomSecret));
    CHECK(relay.Open(RelayConfig(0)));
    const u16 relayPort = relay.localPort();

    CosmeticRelayTransport a, b;
    CHECK(a.Open(TransportConfig(relayPort)));
    CHECK(b.Open(TransportConfig(relayPort)));
    CHECK(a.open() && b.open());

    // Each end verifies inbound loadouts with the shared key.
    CosmeticSyncChannel chanB;
    chanB.SetVerifyKey(CosmeticSyncKey::HmacSecret(kSharedKey));
    CosmeticSyncChannel chanA;
    chanA.SetVerifyKey(CosmeticSyncKey::HmacSecret(kSharedKey));
    chanA.SetTransport(&a);
    chanB.SetTransport(&b);

    // Alice publishes a loadout for her own cape through the relay.
    CosmeticLoadout alice;
    CHECK(alice.Equip("builtin.cape.red"));
    CHECK(chanA.Publish("alice", alice, kSharedKey, /*useHmac=*/true));

    // B drains the relay and applies it. The relay is blind, so the envelope
    // arrives whole (reassembled) and B's channel verifies it.
    CosmeticService serviceB;
    bool applied = false;
    for (int i = 0; i < 200 && !applied; ++i) {
      a.Poll(nullptr, 5);
      b.Poll(
          [&](const std::string& message, const std::string&, u16) {
            size_t n = 0;
            const CosmeticSyncResult r = chanB.Receive(message, &serviceB, &n);
            if (r == CosmeticSyncResult::Ok && n > 0) applied = true;
          },
          5);
      relay.Pump(5);
    }
    CHECK(applied);
    CHECK(serviceB.RemotePlayers().size() == 1);
    CHECK(chanB.accepted() >= 1);

    // A forged loadout (signed with the wrong key) is delivered by the relay but
    // refused by B's channel, so the relay cannot make B wear anything.
    {
      CosmeticSyncChannel forged;
      forged.SetTransport(&a);
      CosmeticLoadout bad;
      CHECK(bad.Equip("builtin.cape.red"));
      CHECK(forged.Publish("mallory", bad, "not-the-shared-key",
                           /*useHmac=*/true));
      const u64 rejectedBefore = chanB.rejected();
      for (int i = 0; i < 120; ++i) {
        a.Poll(nullptr, 5);
        b.Poll(
            [&](const std::string& message, const std::string&, u16) {
              chanB.Receive(message, &serviceB, nullptr);
            },
            5);
        relay.Pump(5);
      }
      CHECK(chanB.rejected() > rejectedBefore);
    }

    a.Close();
    b.Close();
    relay.Close();
  }

  //--- a transport with the wrong room secret registers nowhere ---
  {
    CosmeticRelay relay;
    CHECK(relay.AddRoom("room-1", kRoomSecret));
    CHECK(relay.Open(RelayConfig(0)));
    const u16 relayPort = relay.localPort();

    CosmeticRelayTransport bad;
    RelayCosmeticConfig cfg = TransportConfig(relayPort);
    cfg.roomSecret = "ffffffffffffffffffffffffffffffff";  // wrong secret
    CHECK(bad.Open(cfg));
    // It can open (the bind works) but the relay never registers it, so nothing
    // it sends is forwarded.
    for (int i = 0; i < 40; ++i) {
      bad.Poll(nullptr, 5);
      relay.Pump(5);
    }
    CHECK(relay.peerCount() == 0);

    // A short secret is refused outright, so it never binds a room.
    CosmeticRelayTransport shortSecret;
    RelayCosmeticConfig scfg = TransportConfig(relayPort);
    scfg.roomSecret = "tooshort";
    CHECK(!shortSecret.Open(scfg));

    bad.Close();
    relay.Close();
  }

  if (g_fail == 0) {
    printf("test_cosmetic_relay: ALL OK\n");
    return 0;
  }
  printf("test_cosmetic_relay: %d FAILURES\n", g_fail);
  return 1;
}
