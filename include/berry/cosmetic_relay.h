#pragma once
// UDP forwarder for the cosmetic-sync channel (the presence backend's media
// plane).
//
// `LanCosmeticTransport` reaches peers on the same subnet through an IPv4
// multicast group, and `LanDiscovery` names them. Two clients on different
// networks cannot see each other that way, so this is the relay: a small,
// hostable UDP forwarder that moves *already-signed* cosmetic-sync datagrams
// between registered peers in the same room — the transport half of the
// presence backend whose token half is `RoomTokenClient`.
//
// The relay is deliberately blind and dumb:
//   * it never holds a key and never verifies a signature — the payload is the
//     end-to-end signed envelope (cosmetic_sync.h), so the relay learns nothing
//     and cannot forge a loadout. Even if the relay were hostile, a peer applies
//     a loadout only after the sync channel verifies it and only for assets the
//     named player provably owns;
//   * the control plane is authenticated with a per-room secret; a datagram from
//     an unregistered address is never forwarded, so the relay is not an open
//     amplifier;
//   * TLS is only for issuing room tokens (RoomTokenClient); the media plane is
//     plain UDP, because the payload is already end-to-end signed.
//
// The forwarding engine, the envelope and the room-tag derivation are shared
// with the voice relay (core/net/room_relay.h, core/net/relay_wire.h): the two
// relays move different payloads but have identical control planes. This class
// is the cosmetic-typed front-end.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/net.h"

#include <memory>
#include <string>

namespace Berry {

class RoomRelay;  // core/net/room_relay.h — the shared forwarding engine

/// The cosmetic relay's bind and bound settings. The fields map onto the shared
/// `RoomRelayConfig`.
struct BERRY_API CosmeticRelayConfig {
  /// The UDP port to bind. 0 lets the kernel choose.
  u16 port = 47826;
  /// The address to bind. The default is every interface; a host test binds
  /// loopback.
  std::string bindAddr = "0.0.0.0";
  /// A datagram larger than this is dropped rather than forwarded, so a peer
  /// cannot make the relay amplify an oversized packet.
  size_t mtu = 1400;
  /// How long a registered peer stays registered without a datagram.
  u64 peerTtlMs = 15000;
  /// The cap on registered peers per room, so one room cannot exhaust memory.
  size_t maxPeersPerRoom = 64;
  /// The cap on rooms, so a flood of room tags cannot exhaust memory.
  size_t maxRooms = 64;
};

/// One cosmetic relay process. Owns the socket and the peer registry.
class BERRY_API CosmeticRelay {
 public:
  CosmeticRelay();
  ~CosmeticRelay();

  CosmeticRelay(const CosmeticRelay&) = delete;
  CosmeticRelay& operator=(const CosmeticRelay&) = delete;

  /// Registers a room under `secret` (raw bytes, >= 16). Only a client that can
  /// produce the room tag and register HMAC for this secret is accepted.
  bool AddRoom(const std::string& name, const std::string& secret);
  size_t roomCount() const;

  /// Binds the socket. Returns false (leaving the relay closed) when the bind
  /// fails or no room is configured.
  bool Open(const CosmeticRelayConfig& cfg);
  void Close();
  bool open() const;
  u16 localPort() const;

  /// Forwards every queued datagram and ages out stale peers. Returns the number
  /// of datagrams forwarded.
  size_t Pump(int budgetMs = 20);

  /// The optional internal thread (start after `Open`, stop before `Close`).
  bool Start();
  void Stop();

  struct Stats {
    u64 datagramsReceived = 0;
    u64 datagramsForwarded = 0;
    u64 registrations = 0;
    u64 datagramsDropped = 0;
    u64 peersEvicted = 0;
  };
  Stats stats() const;
  size_t peerCount() const;

  /// The room tag for `name` under `secret`, lowercase hex. The same derivation
  /// the voice relay and a room-token issuer use, so one backend can key both.
  static std::string RoomTagHex(const std::string& name,
                                const std::string& secret);
  /// The register auth for `tagHex` under `secret`, lowercase hex.
  static std::string RegisterAuthHex(const std::string& tagHex,
                                     const std::string& secret);
  static const char* Magic();
  static constexpr size_t kTagBytes = 64;
  static constexpr size_t kAuthBytes = 64;
  static constexpr size_t kEnvelopeBytes = 70;
  static constexpr size_t kMinSecretBytes = 16;
  static constexpr u8 kVersion = 1;
  static constexpr u8 kKindRegister = 1;
  static constexpr u8 kKindData = 2;

 private:
  std::unique_ptr<RoomRelay> relay_;
};

}  // namespace Berry
