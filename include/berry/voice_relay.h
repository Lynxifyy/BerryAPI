#pragma once
// UDP forwarder for the proximity voice channel (F2).
//
// `SecureVoiceTransport` reaches peers on the same subnet through an IPv4
// multicast group. Two phones on different networks cannot hear each other that
// way, so this is the relay: a small, hostable UDP forwarder that moves
// *already-encrypted* voice datagrams between registered peers in the same room.
//
// The relay is deliberately blind and dumb:
//   * it never holds a key and never decrypts — the payload is the end-to-end
//     sealed frame (voice_secure_transport.h), so the relay learns nothing about
//     the audio and cannot forge it;
//   * the control plane is authenticated with a per-room secret; a datagram from
//     an unregistered address is never forwarded, so the relay is not an open
//     amplifier;
//   * TLS is only for issuing room tokens (see RoomTokenClient); the media plane
//     is plain UDP, because the media is already end-to-end encrypted and a
//     second transport layer would only add latency.
//
// Relay envelope (big-endian), one datagram per message:
//
//   offset size  field
//   0      4     magic 'B','V','R','Y'
//   4      1     version (1)
//   5      1     kind: 1 = register, 2 = data
//   6      64    room tag, lowercase hex of HMAC-SHA256(secret,
//                "berry-relay-room" || room)
//   70     ...   kind 1: auth, 64 hex chars of HMAC-SHA256(secret,
//                "berry-relay-register" || roomTag); kind 2: the inner secure
//                voice datagram, opaque to the relay.
//
// A register datagram registers the sender's source address for the room; a data
// datagram is forwarded to every *other* registered peer in that room. A peer
// ages out after `peerTtlMs` without a datagram, so a stale address stops
// receiving. The room tag is the lookup key; the register auth is compared in
// constant time, so a peer that does not know the secret cannot register.

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

struct BERRY_API VoiceRelayConfig {
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
class BERRY_API VoiceRelay {
 public:
  VoiceRelay();
  ~VoiceRelay();

  VoiceRelay(const VoiceRelay&) = delete;
  VoiceRelay& operator=(const VoiceRelay&) = delete;

  /// Registers a room under `secret` (raw bytes, >= 16). Only a client that can
  /// produce the room tag and register HMAC for this secret is accepted, so an
  /// unconfigured room has no members and forwards nothing. Returns false on a
  /// short secret, a duplicate name, or a room cap reached.
  bool AddRoom(const std::string& name, const std::string& secret);
  size_t roomCount() const;

  /// Binds the socket. Returns false (leaving the relay closed) when the bind
  /// fails or no room is configured.
  bool Open(const VoiceRelayConfig& cfg);
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
  /// sends and the relay matches. Exposed so a test (and a token issuer) derives
  /// it the same way.
  static std::string RoomTagHex(const std::string& name,
                                const std::string& secret);
  /// The register auth for `tagHex` under `secret`, lowercase hex.
  static std::string RegisterAuthHex(const std::string& tagHex,
                                     const std::string& secret);
  static const char* Magic();
  static constexpr size_t kTagBytes = 64;       // hex room tag
  static constexpr size_t kAuthBytes = 64;      // hex register auth
  static constexpr size_t kEnvelopeBytes = 70;  // magic+version+kind+tag
  static constexpr size_t kMinSecretBytes = 16;
  static constexpr u8 kVersion = 1;
  static constexpr u8 kKindRegister = 1;
  static constexpr u8 kKindData = 2;

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
  VoiceRelayConfig cfg_{};
  bool open_ = false;
  std::vector<Room> rooms_;
  mutable std::mutex mutex_;
  Stats stats_{};

  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace Berry
