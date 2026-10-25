#pragma once
// Cosmetic-sync transport over a UDP relay (the presence backend).
//
// `LanCosmeticTransport` reaches peers on one subnet. This reaches peers through
// `CosmeticRelay`: the same signed cosmetic-sync envelopes, framed with the
// shared fragment format (core/net/cosmetic_frag.h) and wrapped in the relay
// envelope, sent to one relay address instead of a multicast group. A peer on
// another network is reached because both ends register with the same relay
// room; the relay forwards each datagram to the room's other members.
//
// The relay is blind: it sees only the fragmented, end-to-end signed envelope,
// never plaintext and never a key. This transport therefore keeps every property
// of the LAN transport — the fragment framing and reassembly, the per-sender
// bounds — and adds only the room envelope. Verification stays where it was:
// `CosmeticSyncChannel::Receive` verifies and applies, this only moves bytes and
// never trusts a peer.
//
// A client registers with the room secret (raw bytes) and a room name; the relay
// holds the same secret and matches the tag. TLS is not used here: the payload
// is already end-to-end signed, and the room secret is what a room token
// (RoomTokenClient) delivers over HTTPS.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/net.h"
#include "berry/cosmetic_sync.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace Berry {

/// Internal (core/net/cosmetic_frag.h): the shared fragment reassembly.
namespace cosmeticfrag {
class Reassembler;
}

struct BERRY_API RelayCosmeticConfig {
  /// The relay's numeric IPv4 address (a dotted quad). A name is not resolved:
  /// the embedder resolves it and passes the address, so the transport never
  /// blocks on DNS. `Open` refuses a malformed address.
  std::string relayHost = "127.0.0.1";
  /// The relay's UDP port.
  u16 relayPort = 47826;
  /// The room name; the relay must have a room with the same name.
  std::string room = "default";
  /// The room secret (raw bytes, >= 16). Must match the relay's for the room.
  std::string roomSecret;
  /// The local interface to bind. The default binds every interface.
  std::string bindAddr = "0.0.0.0";
  /// The local port to bind; 0 lets the kernel choose.
  u16 bindPort = 0;
  /// Datagram budget in bytes, including the relay envelope and the fragment
  /// header. Kept under the 1280-byte IPv6 minimum MTU.
  size_t mtu = 1200;
  /// Cap on a reassembled message. Matches the channel's own cap, so a message
  /// the channel would refuse is not even assembled.
  size_t maxMessageBytes = 64 * 1024;
  /// How many partial messages one sender may have in flight at once.
  size_t maxReassemblyPerPeer = 4;
  /// How long a partial message is kept before it is dropped.
  u64 reassemblyTtlMs = 3000;
  /// How often to re-register, in milliseconds, so a relay that restarted (and
  /// a late peer) still finds us.
  u64 registerIntervalMs = 5000;
};

/// A `CosmeticSyncTransport` that moves signed cosmetic envelopes through a
/// `CosmeticRelay`. Not thread-safe on its own; the client serialises Send/Poll.
class BERRY_API CosmeticRelayTransport final : public CosmeticSyncTransport {
 public:
  CosmeticRelayTransport();
  ~CosmeticRelayTransport() override;

  /// Binds the socket and registers with the room. Returns false, leaving the
  /// transport closed, when the bind fails, the room secret is too short, or the
  /// relay address is malformed.
  bool Open(const RelayCosmeticConfig& cfg);
  void Close();
  bool open() const { return open_; }
  const RelayCosmeticConfig& config() const { return cfg_; }
  u16 localPort() const;
  UdpSocket& socket() { return sock_; }

  /// Frames `message` into fragments and sends them to the relay. False when the
  /// transport is closed, the message exceeds the cap, or a send fails.
  bool Send(const std::string& message) override;

  /// A complete, reassembled message plus where it came from. (The relay
  /// address, not the originating peer's — the relay is the sender we see.)
  using MessageCb = std::function<void(const std::string& message,
                                       const std::string& fromHost,
                                       u16 fromPort)>;

  /// Drains incoming datagrams for up to `budgetMs`, reassembling fragments and
  /// invoking `onMessage` once per complete message (outside any lock). Only a
  /// data envelope from the configured relay is accepted.
  void Poll(const MessageCb& onMessage, int budgetMs = 20);

  /// Sends a register datagram for our room now (also sent on `Open` and on the
  /// interval).
  bool SendRegister();

  struct Stats {
    u64 datagramsSent = 0;
    u64 datagramsReceived = 0;
    u64 messagesSent = 0;
    u64 messagesReceived = 0;
    u64 fragmentsDropped = 0;   // malformed, out of range, or over a cap
    u64 messagesDropped = 0;    // partial messages evicted by TTL or a cap
    u64 registrationsSent = 0;
    u64 datagramsDropped = 0;   // not from the relay, or a non-data envelope
  };
  Stats stats() const;

  /// Injects a monotonic millisecond clock for the TTLs (a test pins them).
  void SetClock(std::function<u64()> nowMs);

  static const char* Magic();
  static constexpr size_t kHeaderBytes = 24;    // fragment header
  static constexpr size_t kTagBytes = 64;       // hex room tag
  static constexpr size_t kAuthBytes = 64;      // hex register auth
  static constexpr size_t kEnvelopeBytes = 70;  // magic + version + kind + tag
  static constexpr size_t kMinSecretBytes = 16;
  static constexpr u8 kVersion = 1;
  static constexpr u8 kKindRegister = 1;
  static constexpr u8 kKindData = 2;

 private:
  /// Wraps `inner` (a cosmetic fragment datagram) in the relay data envelope.
  bool WrapData(const std::string& inner, std::string* out) const;

  UdpSocket sock_;
  RelayCosmeticConfig cfg_{};
  std::string tagHex_;
  std::string authHex_;
  bool open_ = false;
  u16 msgSeq_ = 0;
  u64 lastRegisterMs_ = 0;
  std::unique_ptr<cosmeticfrag::Reassembler> reassembler_;
  mutable std::mutex mutex_;
  Stats stats_{};
  std::function<u64()> clock_;
};

}  // namespace Berry
