#pragma once
// Voice transport over a UDP relay (F2).
//
// `SecureVoiceTransport` reaches peers on one subnet. This reaches peers through
// `VoiceRelay`: the same authenticated, encrypted datagrams, wrapped in the
// relay's envelope and sent to one relay address instead of a multicast group.
// A peer on another network is reached because both ends register with the same
// relay room; the relay forwards each datagram to the room's other members.
//
// The relay is blind: it sees only the end-to-end sealed frame (or an announce),
// never plaintext and never a key. This transport therefore keeps every property
// of the LAN transport — the identity handshake, the pairwise X25519/HKDF keys,
// ChaCha20-Poly1305, and the replay window — and adds only the room envelope.
//
// A client registers with the room secret (raw bytes) and a room name; the relay
// holds the same secret and matches the tag. TLS is not used here: the media is
// already end-to-end encrypted, and the room secret is what a room token
// (RoomTokenClient) delivers over HTTPS.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/net.h"
#include "berry/voice.h"
#include "berry/voice_crypto.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {

struct BERRY_API RelayVoiceConfig {
  /// The relay's numeric IPv4 address (a dotted quad). A name is not resolved:
  /// the embedder resolves it and passes the address, so the transport never
  /// blocks on DNS. `Open` refuses a malformed address.
  std::string relayHost = "127.0.0.1";
  /// The relay's UDP port.
  u16 relayPort = 47825;
  /// The room name; the relay must have a room with the same name.
  std::string room = "default";
  /// The room secret (raw bytes, >= 16). Must match the relay's for the room.
  std::string roomSecret;
  /// The local interface to bind. The default binds every interface.
  std::string bindAddr = "0.0.0.0";
  /// The local port to bind; 0 lets the kernel choose.
  u16 bindPort = 0;
  /// Datagram budget in bytes, including the relay envelope and the sealed
  /// header. Kept under the 1280-byte IPv6 minimum MTU.
  size_t mtu = 1200;
  /// How often to re-register and re-announce, in milliseconds.
  u64 announceIntervalMs = 2000;
};

/// A `VoiceTransport` that moves secure voice frames through a `VoiceRelay`.
/// Not thread-safe on its own; the voice service serialises SendFrame/Poll.
class BERRY_API VoiceRelayTransport final : public VoiceTransport {
 public:
  VoiceRelayTransport();
  ~VoiceRelayTransport() override;

  /// Binds the socket and registers with the room. Returns false, leaving the
  /// transport closed, when the bind fails, the room secret is too short, or the
  /// channel refuses the identity/anchor — voice then fails closed rather than
  /// falling back to plaintext.
  bool Open(const RelayVoiceConfig& cfg, const VoiceIdentity& identity,
            const VoiceTrustAnchor& anchor);
  void Close();
  bool open() const { return open_; }
  const RelayVoiceConfig& config() const { return cfg_; }
  u16 localPort() const;
  UdpSocket& socket() { return sock_; }

  /// The secure channel, for diagnostics (peer count) and tests.
  VoiceSecureChannel& channel() { return channel_; }

  bool SendFrame(const std::string& speaker, u32 seq,
                 const std::vector<u8>& adpcm) override;
  void Poll(const FrameCb& onFrame, int budgetMs) override;
  std::string name() const override { return "relay"; }

  /// Sends our announce to the relay now (also sent on `Open` and on the
  /// interval). False when closed or the channel has no announce yet.
  bool AnnounceNow();

  struct Stats {
    u64 datagramsSent = 0;
    u64 datagramsReceived = 0;
    u64 framesSent = 0;
    u64 framesReceived = 0;
    u64 announcesSent = 0;
    u64 announcesAccepted = 0;
    u64 announcesRejected = 0;
    u64 registrationsSent = 0;
    u64 datagramsDropped = 0;
  };
  Stats stats() const;

  static const char* Magic();
  static constexpr size_t kTagBytes = 64;
  static constexpr size_t kAuthBytes = 64;
  static constexpr size_t kEnvelopeBytes = 70;
  static constexpr size_t kMinSecretBytes = 16;
  static constexpr u8 kVersion = 1;
  static constexpr u8 kKindRegister = 1;
  static constexpr u8 kKindData = 2;

 private:
  /// Wraps `inner` (a secure-voice datagram) in the relay data envelope.
  bool WrapData(const std::string& inner, std::string* out) const;
  /// Sends a register datagram for our room.
  bool SendRegister();
  /// Sends `inner` (a secure-voice datagram) to the relay as a data datagram.
  bool SendToRelay(const std::string& inner);

  UdpSocket sock_;
  RelayVoiceConfig cfg_{};
  VoiceSecureChannel channel_;
  std::string tagHex_;
  std::string authHex_;
  bool open_ = false;
  u64 lastAnnounceMs_ = 0;
  std::unordered_map<std::string, u64> sendSeq_;
  mutable std::mutex mutex_;
  Stats stats_{};
};

}  // namespace Berry
