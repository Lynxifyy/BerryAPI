#pragma once
// LAN transport for the cosmetic sync channel.
//
// `CosmeticSyncTransport` (cosmetic_sync.h) is the seam the channel hands a
// signed message to; this is the concrete implementation that puts it on the
// wire. A signed loadout envelope is larger than one datagram can carry, so the
// transport frames each message into fragments and reassembles them on the far
// side before handing the whole envelope up. Verification stays where it was:
// the channel verifies, this only moves bytes and never trusts a peer.
//
// Design:
//   * one UDP socket per host, bound to a fixed port and joined to a private
//     IPv4 multicast group, so a peer reaches every client on the subnet in one
//     send and no central server is needed;
//   * a 24-byte big-endian datagram header (magic/version/kind/msgId/fragment
//     index/count/total length/fragment payload size/reserved/sender tag),
//     validated fail-closed on receive;
//   * a per-sender reassembly table with a fragment-count bound and a TTL, so a
//     peer that sends half a message (or a flood of partial messages) cannot
//     grow memory without limit;
//   * a peer table fed by `Announce` datagrams, with a TTL and an LRU cap.
//
// Every failure is a dropped datagram, never a crash and never a partial apply:
// a message is delivered to the caller only when every fragment arrived and the
// declared length matched.
//
// Wire header (24 bytes, big-endian):
//
//   offset  size  field
//   0       4     magic 'B','L','A','N'
//   4       1     version (1)
//   5       1     kind: 1 = message fragment, 2 = announce
//   6       2     msgId (per-sender, wraps)
//   8       2     fragment index (0-based)
//   10      2     fragment count
//   12      4     total message length in bytes
//   16      2     fragment payload size: how many message bytes each fragment
//                 carries. The sender states it, so a receiver with a different
//                 MTU still computes the right offset instead of guessing.
//   18      2     reserved (0)
//   20      4     sender tag: a per-instance value the sender stamps on every
//                 datagram, so a client ignores the copy of its own datagram
//                 that multicast loops back
//
// A message fragment's payload is the message slice for that index; an announce
// carries the sender's player id. A kind or field outside the contract is
// dropped.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/cosmetic_sync.h"
#include "berry/net.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {

/// Internal (core/net/multicast_lock.h): the Android Wi-Fi multicast lock held
/// while the group is joined. Forward declared so this public header never
/// includes the internal one.
class MulticastLock;

/// Internal (core/net/cosmetic_frag.h): the shared fragment framing and
/// receiver-side reassembly used by both cosmetic transports. Forward declared
/// so this public header never includes the internal one.
namespace cosmeticfrag {
class Reassembler;
}

/// Where the transport listens and what it will accept.
struct BERRY_API LanTransportConfig {
  /// The private multicast group the clients share. In the 239.0.0.0/8
  /// administratively-scoped range, so it is never routed off the subnet.
  std::string group = "239.66.0.1";
  /// The fixed port every client binds, so a send to the group reaches them all.
  u16 port = 47823;
  /// The interface to join and send on. The default ("0.0.0.0") uses the
  /// system's multicast route; a host test sets "127.0.0.1" to stay on loopback.
  std::string iface = "0.0.0.0";
  /// Datagram budget in bytes, including the fragment header. Kept under the
  /// 1280-byte IPv6 minimum MTU so a fragment is not itself fragmented.
  size_t mtu = 1200;
  /// Cap on a reassembled message. Matches the channel's own cap, so a message
  /// the channel would refuse is not even assembled.
  size_t maxMessageBytes = 64 * 1024;
  /// How many partial messages one sender may have in flight at once.
  size_t maxReassemblyPerPeer = 4;
  /// How long a partial message is kept before it is dropped.
  u64 reassemblyTtlMs = 3000;
  /// Cap on the peer table (LRU-evicted when full).
  size_t maxPeers = 64;
  /// How long a peer stays listed after its last announce.
  u64 peerTtlMs = 15000;
};

/// A peer seen on the LAN, from its announce datagram.
struct BERRY_API LanPeer {
  std::string host;      // numeric address the announce came from
  u16 port = 0;          // the sender's port
  std::string playerId;  // as announced
  u64 lastSeenMs = 0;
};

/// Moves signed cosmetic-sync envelopes over the LAN. Not a verifier: a
/// reassembled message is handed up unverified, and the channel decides.
class BERRY_API LanCosmeticTransport final : public CosmeticSyncTransport {
 public:
  LanCosmeticTransport();
  ~LanCosmeticTransport() override;

  /// Binds the socket and joins the group. Returns false (fail closed, leaving
  /// the transport closed) when the socket cannot bind; a failed group join
  /// still leaves a usable unicast socket and is reported by `multicastJoined`.
  bool Open(const LanTransportConfig& cfg);
  void Close();
  bool open() const { return open_; }
  /// True when the group join succeeded (a unicast-only socket is still open).
  bool multicastJoined() const { return joined_; }
  const LanTransportConfig& config() const { return cfg_; }

  /// The bound port, or 0 when closed.
  u16 localPort() const;
  /// The socket, for a test that wants to bind a second one on loopback.
  UdpSocket& socket() { return sock_; }

  /// Frames `message` into fragments and sends them to the group. False when
  /// the transport is closed, the message exceeds the cap, or a send fails.
  bool Send(const std::string& message) override;

  /// Frames `message` and sends it directly to one peer (a discovered address),
  /// bypassing the group. Same framing and cap as `Send`. False on a bad
  /// address, a closed transport, or a send failure.
  bool SendTo(const std::string& host, u16 port, const std::string& message);

  /// Sends an announce datagram so peers can discover this client. The tag is
  /// per-instance and set at Open, so the id here only names the sender.
  bool Announce(const std::string& playerId);

  /// A complete, reassembled message plus where it came from.
  using MessageCb = std::function<void(const std::string& message,
                                       const std::string& fromHost,
                                       u16 fromPort)>;

  /// Drains incoming datagrams for up to `budgetMs`, reassembling fragments and
  /// invoking `onMessage` once per complete message (outside any lock). Also
  /// records announce datagrams in the peer table. A datagram carrying this
  /// transport's own tag (stamped at Open) is skipped, so a client never
  /// processes the copy of its traffic that multicast loops back.
  void Poll(const MessageCb& onMessage, int budgetMs = 20);

  /// Peers seen within `peerTtlMs`, most recently seen first.
  std::vector<LanPeer> peers() const;

  struct Stats {
    u64 datagramsSent = 0;
    u64 datagramsReceived = 0;
    u64 messagesSent = 0;
    u64 messagesReceived = 0;
    u64 fragmentsDropped = 0;   // malformed, out of range, or over a cap
    u64 messagesDropped = 0;    // partial messages evicted by TTL or a cap
    u64 selfSkipped = 0;        // own datagrams ignored after a loop back
  };
  Stats stats() const;

  /// Injects a monotonic millisecond clock for the TTLs (a test pins them).
  void SetClock(std::function<u64()> nowMs);

  /// The fragment header size in bytes.
  static constexpr size_t kHeaderBytes = 24;
  /// The wire magic for this format.
  static const char* Magic();

 private:
  /// A message that finished reassembling, ready to hand to the caller.
  struct Complete {
    std::string message;
    std::string host;
    u16 port = 0;
  };

  /// Builds a datagram's header. Caller fills the payload after `kHeaderBytes`.
  static void FillHeader(u8* out, u8 kind, u16 msgId, u16 index, u16 count,
                         u32 totalLen, u16 payloadPer, u32 tag);

  /// Frames `message` and sends it to `host:port` (the group for `Send`).
  bool SendFramed(const std::string& host, u16 port, const std::string& message);

  /// Handles one received datagram under the lock. On a complete message,
  /// appends it to `complete`.
  void HandleDatagramLocked(const u8* data, size_t len, const std::string& host,
                            u16 port, std::vector<Complete>* complete);

  /// Records or refreshes a peer. Caller holds the lock.
  void TouchPeerLocked(const std::string& host, u16 port,
                       const std::string& playerId, u64 now);

  UdpSocket sock_;
  LanTransportConfig cfg_{};
  /// Held while the group is joined (Android). Null elsewhere.
  std::unique_ptr<MulticastLock> mcastLock_;
  bool open_ = false;
  bool joined_ = false;
  u16 msgSeq_ = 0;
  u32 selfTag_ = 0;   // per-instance tag; a datagram carrying it is our own

  mutable std::mutex mutex_;
  /// Shared fragment reassembly (core/net/cosmetic_frag.h), rebuilt on Open so
  /// its caps come from the config.
  std::unique_ptr<cosmeticfrag::Reassembler> reassembler_;
  std::unordered_map<std::string, LanPeer> peers_;           // key: host:port
  Stats stats_{};
  std::function<u64()> clock_;
};

}  // namespace Berry
