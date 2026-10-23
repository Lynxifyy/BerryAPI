#pragma once
// LAN transport for the proximity voice channel.
//
// `VoiceTransport` (voice.h) is the seam the voice service hands an encoded
// frame to; this is the concrete implementation that puts it on the wire. A
// voice frame is small (one 20 ms frame, a few hundred bytes at most), so unlike
// the cosmetic-sync transport this needs no fragmentation: one frame is one
// datagram, and a lost datagram is simply a lost 20 ms of audio — voice is
// best-effort and never waits for a retransmit.
//
// Design:
//   * one UDP socket per host, bound to a fixed port and joined to a private
//     IPv4 multicast group (a different group from the cosmetic channel), so a
//     peer reaches every client on the subnet in one send and no server is
//     needed;
//   * a big-endian datagram header carrying the magic, version, sequence and
//     the speaker id, validated fail-closed on receive;
//   * a per-datagram size bound, so an oversized or truncated datagram is
//     dropped rather than decoded.
//
// Nothing here is authoritative and nothing leaves the subnet: the group is in
// the administratively-scoped 239.0.0.0/8 range, so it is never routed off the
// LAN.
//
// Wire header (big-endian):
//
//   offset  size  field
//   0       4     magic 'B','V','O','X'
//   4       1     version (1)
//   5       1     kind: 1 = voice frame
//   6       2     sequence (per-sender, wraps)
//   8       1     speaker-id length (1..64)
//   9       1     reserved (0)
//   10      ...   speaker id bytes
//   ...     ...   ADPCM payload
//
// A kind or field outside the contract is dropped.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/net.h"
#include "berry/voice.h"

#include <functional>
#include <mutex>
#include <string>

namespace Berry {

struct BERRY_API VoiceTransportConfig {
  /// The private multicast group the voice clients share. A different group
  /// from the cosmetic channel, so the two never mix. In the 239.0.0.0/8
  /// administratively-scoped range, so it is never routed off the subnet.
  std::string group = "239.66.0.2";
  /// The fixed port every voice client binds.
  u16 port = 47824;
  /// The interface to join and send on. The default ("0.0.0.0") uses the
  /// system's multicast route; a host test sets "127.0.0.1" to stay on loopback.
  std::string iface = "0.0.0.0";
  /// Datagram budget in bytes, including the header. Kept under the 1280-byte
  /// IPv6 minimum MTU so a datagram is not itself fragmented.
  size_t mtu = 1200;
};

class BERRY_API LanVoiceTransport final : public VoiceTransport {
 public:
  LanVoiceTransport();
  ~LanVoiceTransport() override;

  /// Binds the socket and joins the group. Returns false (fail closed, leaving
  /// the transport closed) when the socket cannot bind; a failed group join
  /// still leaves a usable unicast socket and is reported by `multicastJoined`.
  bool Open(const VoiceTransportConfig& cfg);
  void Close();
  bool open() const { return open_; }
  bool multicastJoined() const { return joined_; }
  const VoiceTransportConfig& config() const { return cfg_; }
  u16 localPort() const;
  UdpSocket& socket() { return sock_; }

  bool SendFrame(const std::string& speaker, u32 seq,
                 const std::vector<u8>& adpcm) override;
  void Poll(const FrameCb& onFrame, int budgetMs) override;
  std::string name() const override { return "lan"; }

  struct Stats {
    u64 datagramsSent = 0;
    u64 datagramsReceived = 0;
    u64 framesSent = 0;
    u64 framesReceived = 0;
    u64 datagramsDropped = 0;   // malformed, oversized, or a bad id length
  };
  Stats stats() const;

  /// The wire magic for this format.
  static const char* Magic();
  /// The fixed header size before the speaker id.
  static constexpr size_t kHeaderBytes = 10;
  /// The speaker-id cap, matching VoiceService::ValidId.
  static constexpr size_t kMaxIdBytes = 64;

 private:
  /// Builds one datagram. Returns false when the id or payload is out of range.
  bool BuildDatagram(const std::string& speaker, u32 seq,
                     const std::vector<u8>& adpcm, std::string* out) const;

  UdpSocket sock_;
  VoiceTransportConfig cfg_{};
  bool open_ = false;
  bool joined_ = false;
  mutable std::mutex mutex_;
  Stats stats_{};
};

}  // namespace Berry
