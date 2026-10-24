#pragma once
// Authenticated, encrypted LAN transport for the proximity voice channel.
//
// `LanVoiceTransport` moves voice frames in the clear: any host on the subnet
// can join the multicast group, listen to every frame, and inject its own. This
// is the production replacement. It speaks the same `VoiceTransport` seam, but
// every frame is authenticated and sealed (see `berry/voice_crypto.h`):
//
//   * each peer proves its identity with a signed announce (Ed25519, or an
//     HMAC under a backend-issued room key) before any key is agreed;
//   * the audio is sealed with ChaCha20-Poly1305 under a pairwise, per-direction
//     key derived by X25519 + HKDF, so a peer that cannot authenticate can
//     neither decrypt a frame nor produce one that passes the tag;
//   * a replayed or out-of-window frame is dropped by the session's replay
//     window, and a non-monotonic send sequence is refused, so a nonce is never
//     reused.
//
// Wire formats (big-endian):
//
//   announce datagram
//     offset size  field
//     0      4     magic 'B','V','X','S'
//     4      1     version (1)
//     5      1     kind = 1 (announce)
//     6      ...   the encoded announce (voice_crypto.h)
//
//   sealed frame datagram
//     offset size  field
//     0      4     magic 'B','V','X','S'
//     4      1     version (1)
//     5      1     kind = 2 (sealed frame)
//     6      8     route tag (the sender's, so the receiver picks the session)
//     14     8     sequence (the AEAD counter; also in the AAD, so tampering
//                  fails the tag)
//     22     2     speaker-id length (1..64)
//     24     1     reserved (0)
//     25     ...   speaker id, then ciphertext || tag
//
// The whole header up to and including the speaker id is the AEAD associated
// data, so a sealed frame cannot be re-labelled under a different route, id or
// sequence. A datagram that is not exactly one of these two shapes is dropped.
//
// Nothing leaves the subnet: the group is in 239.0.0.0/8. The announce is the
// only unauthenticated input and it is verified before it changes any state.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/net.h"
#include "berry/voice.h"
#include "berry/voice_crypto.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {

struct BERRY_API SecureVoiceConfig {
  /// The private multicast group (a different group from the cosmetic channel).
  std::string group = "239.66.0.2";
  /// The fixed port every voice client binds.
  u16 port = 47824;
  /// The interface to join and send on; "0.0.0.0" uses the system route.
  std::string iface = "0.0.0.0";
  /// Datagram budget in bytes, including the header. Kept under the 1280-byte
  /// IPv6 minimum MTU so a datagram is not itself fragmented.
  size_t mtu = 1200;
  /// How often to (re)send our announce, in milliseconds. A peer that joins
  /// later still learns our ephemeral key; the announce is idempotent.
  u64 announceIntervalMs = 2000;
};

/// A `VoiceTransport` that authenticates and encrypts every frame. One UDP
/// socket, one `VoiceSecureChannel`. Not thread-safe on its own; the voice
/// service serialises `SendFrame`/`Poll` (see `VoiceService::Pump`).
class BERRY_API SecureVoiceTransport final : public VoiceTransport {
 public:
  SecureVoiceTransport();
  ~SecureVoiceTransport() override;

  /// Binds the socket, joins the group and configures the secure channel with
  /// `identity` (must be able to sign) and `anchor`. Returns false, leaving the
  /// transport closed, when the socket cannot bind or the channel is not
  /// configured — voice then fails closed rather than falling back to
  /// plaintext.
  bool Open(const SecureVoiceConfig& cfg, const VoiceIdentity& identity,
            const VoiceTrustAnchor& anchor);
  void Close();
  bool open() const { return open_; }
  bool multicastJoined() const { return joined_; }
  const SecureVoiceConfig& config() const { return cfg_; }
  u16 localPort() const;
  UdpSocket& socket() { return sock_; }

  /// The secure channel, for diagnostics (peer count) and tests.
  VoiceSecureChannel& channel() { return channel_; }

  /// Seals one copy of `adpcm` per established peer and sends it. Returns false
  /// when the transport is closed, there is no peer yet, or a seal failed (the
  /// caller then drops the frame — voice is best-effort).
  bool SendFrame(const std::string& speaker, u32 seq,
                 const std::vector<u8>& adpcm) override;

  /// Drains incoming datagrams for up to `budgetMs`: verifies announces (which
  /// may establish a session) and opens sealed frames, invoking `onFrame` with
  /// the plaintext once per accepted frame.
  void Poll(const FrameCb& onFrame, int budgetMs) override;

  std::string name() const override { return "secure-lan"; }

  struct Stats {
    u64 datagramsSent = 0;
    u64 datagramsReceived = 0;
    u64 framesSent = 0;        // sealed frames put on the wire
    u64 framesReceived = 0;    // sealed frames opened and delivered
    u64 announcesSent = 0;
    u64 announcesAccepted = 0;
    u64 announcesRejected = 0;
    u64 datagramsDropped = 0;  // malformed, unknown route, bad tag, replay
  };
  Stats stats() const;

  /// The wire magic for this format.
  static const char* Magic();
  static constexpr u8 kVersion = 1;
  static constexpr u8 kKindAnnounce = 1;
  static constexpr u8 kKindSealedFrame = 2;
  static constexpr size_t kPrefixBytes = 6;         // magic + version + kind
  static constexpr size_t kRouteTagBytes = 8;
  static constexpr size_t kSealedHeaderBytes = 25;  // prefix + tag + seq + len + rsv
  static constexpr size_t kMaxIdBytes = 64;

  /// Sends our announce now (also sent on `Open` and every interval). False when
  /// closed or the channel has no announce yet.
  bool AnnounceNow();

 private:
  /// Builds one sealed datagram for `session` with `speaker`/`seq`. The header
  /// up to the speaker id is the AAD.
  bool BuildSealed(const VoiceSession& session, const std::string& routeTag,
                   const std::string& speaker, u64 seq,
                   const std::vector<u8>& adpcm, std::string* out) const;

  UdpSocket sock_;
  SecureVoiceConfig cfg_{};
  VoiceSecureChannel channel_;
  bool open_ = false;
  bool joined_ = false;
  u64 lastAnnounceMs_ = 0;
  /// Per-peer send counter, so each session's AEAD counter is monotonic even
  /// though the service's sequence is a wrapping u32.
  std::unordered_map<std::string, u64> sendSeq_;
  mutable std::mutex mutex_;
  Stats stats_{};
};

}  // namespace Berry
