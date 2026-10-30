#pragma once
// Shared UDP relay engine for the voice (F2) and cosmetic-sync relays.
//
// Both relays do exactly the same job: a peer proves it knows a room's secret,
// registers its address for that room, and the relay forwards each of its
// opaque data datagrams to the room's *other* registered peers. Nothing about
// that control plane depends on what the payload is — a sealed voice frame or a
// signed cosmetic fragment — so it lives here once, and `VoiceRelay`
// (include/berry/voice_relay.h) and `CosmeticRelay`
// (include/berry/cosmetic_relay.h) are thin, typed front-ends over it.
//
// The relay is deliberately blind: it never holds a key and never decrypts. It
// matches a datagram to a room by the room tag (a constant-time compare against
// HMAC-SHA256(secret, "berry-relay-room" || room)) and to a registered source by
// its host:port. An unregistered source is never forwarded, so the relay is not
// an open amplifier, and a data datagram is never echoed to its sender.
//
// The wire envelope is `relaywire` (core/net/relay_wire.h).

#include "berry/export.h"
#include "berry/types.h"
#include "berry/net.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Berry {

/// Where a relay binds and how strictly it bounds its tables. Shared by both
/// relays; each public config maps onto this.
struct BERRY_API RoomRelayConfig {
  /// The UDP port to bind. 0 lets the kernel choose.
  u16 port = 47825;
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

/// One relay process. Owns the socket and the peer registry. `Pump` is not
/// thread-safe with itself; the internal thread serialises it, and a caller that
/// drives `Pump` directly must not also `Start` the thread.
class BERRY_API RoomRelay {
 public:
  RoomRelay();
  ~RoomRelay();

  RoomRelay(const RoomRelay&) = delete;
  RoomRelay& operator=(const RoomRelay&) = delete;

  /// Registers a room under `secret` (raw bytes, >= 16). Returns false on a
  /// short secret, a duplicate name, or the room cap being reached.
  bool AddRoom(const std::string& name, const std::string& secret);
  size_t roomCount() const;

  /// Binds the socket. Returns false (leaving the relay closed) when the bind
  /// fails, the mtu cannot hold the envelope, or no room is configured.
  bool Open(const RoomRelayConfig& cfg);
  void Close();
  bool open() const { return open_; }
  u16 localPort() const;

  /// Forwards every queued datagram and ages out stale peers. `budgetMs` bounds
  /// the wait for the first datagram (0 drains without blocking). Returns the
  /// number of datagrams forwarded.
  size_t Pump(int budgetMs = 20);

  /// The optional internal thread (start after `Open`, stop before `Close`).
  bool Start();
  void Stop();

  struct Stats {
    u64 datagramsReceived = 0;
    u64 datagramsForwarded = 0;
    u64 registrations = 0;
    u64 datagramsDropped = 0;  // bad magic, bad auth, unknown room, oversize
    u64 peersEvicted = 0;
  };
  Stats stats() const;
  size_t peerCount() const;

  /// The room tag for `name` under `secret`, lowercase hex — the value a client
  /// sends and the relay matches.
  static std::string RoomTagHex(const std::string& name,
                                const std::string& secret);
  /// The register auth for `tagHex` under `secret`, lowercase hex.
  static std::string RegisterAuthHex(const std::string& tagHex,
                                     const std::string& secret);

  static constexpr size_t kTagBytes = 64;       // hex room tag
  static constexpr size_t kAuthBytes = 64;      // hex register auth
  static constexpr size_t kMinSecretBytes = 16;

 private:
  struct Peer {
    u64 lastMs = 0;
  };
  struct Room {
    std::string name;
    std::string secret;
    std::string tagHex;
    std::unordered_map<std::string, Peer> peers;  // key: host:port
  };

  /// Finds a room by tag hex, or null. Caller holds `mutex_`.
  Room* FindRoomLocked(const std::string& tagHex);
  /// Handles one received datagram. Caller holds `mutex_`.
  void HandleLocked(const char* data, size_t n, const std::string& host,
                    u16 port, u64 now, size_t* forwarded);
  /// Evicts peers older than the TTL. Caller holds `mutex_`.
  void EvictLocked(u64 now);

  UdpSocket sock_;
  RoomRelayConfig cfg_{};
  bool open_ = false;
  std::vector<Room> rooms_;
  mutable std::mutex mutex_;
  Stats stats_{};

  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace Berry
