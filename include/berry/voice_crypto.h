#pragma once
// Voice authentication and encryption.
//
// The proximity voice channel moves microphone audio between LAN peers. Without
// this layer any host on the subnet could *listen* to every voice frame and
// *inject* its own, because the transport is a plain UDP multicast group. This
// is the boundary that stops both:
//
//   * Authentication — every peer proves it holds an accepted identity before
//     its key is trusted. Two anchors are supported, mirroring the rest of the
//     library: a set of Ed25519 public keys (the production mode) or a single
//     backend-issued room key (HMAC, for a LAN that does not want to distribute
//     per-peer keys). A peer that cannot authenticate is never given a session,
//     so it can neither derive a key nor have its frames accepted.
//   * Key agreement — each side contributes an ephemeral X25519 key; the shared
//     secret is combined with both nonces through HKDF-SHA256 into a per-pair,
//     per-direction session key. The long-term Ed25519 key only authenticates
//     the handshake; it never encrypts audio, so a compromised session key does
//     not expose the identity key.
//   * Encryption — frames are sealed with ChaCha20-Poly1305 (AEAD). The nonce is
//     `direction || counter`, with the counter strictly monotonic per session,
//     so a nonce is never reused under a key. The frame header (magic, version,
//     kind, speaker id) is the associated data, so a sealed frame cannot be
//     reinterpreted under a different header.
//   * Replay defence — a sliding-window replay filter rejects a re-sent frame,
//     so a captured datagram cannot be replayed to replay audio or to reuse a
//     nonce.
//
// Every entry point fails closed: a malformed, unsigned, wrongly-signed, stale,
// replayed or undecryptable input changes no state and returns false.
//
// mbedTLS (X25519/ECDH, HKDF, ChaCha20-Poly1305) and the vendored Ed25519 are
// used through this header's std::string interface, so no caller needs either
// include path. This is a client-side feature, not a strict Berry:: module.

#include "berry/export.h"
#include "berry/types.h"

#include <string>
#include <utility>
#include <vector>

namespace Berry {

/// Fixed sizes, in bytes, of the values on the wire and in the key schedule.
struct BERRY_API VoiceCrypto {
  static constexpr size_t kKeyBytes = 32;      // Ed25519 pub / X25519 pub / AEAD key
  static constexpr size_t kSecretBytes = 64;   // Ed25519 secret key
  static constexpr size_t kSignatureBytes = 64;  // Ed25519 signature
  static constexpr size_t kNonceBytes = 16;    // handshake nonce (not the AEAD nonce)
  static constexpr size_t kMacBytes = 32;      // HMAC-SHA256 tag (room-key mode)
  static constexpr size_t kAeadNonceBytes = 12;
  static constexpr size_t kAeadTagBytes = 16;
  static constexpr size_t kEphemeralBytes = 32;  // X25519 secret or public key
};

/// The wire magic for the handshake announce ("BVAN").
BERRY_API const char* VoiceAnnounceMagic();

/// True when the crypto backend is compiled in (mbedTLS + Ed25519). A build
/// without it refuses every handshake, so voice fails closed rather than
/// falling back to plaintext.
BERRY_API bool VoiceCryptoAvailable();

/// Fills `out` with `n` cryptographically secure random bytes. Returns false
/// when the CSPRNG cannot be seeded (then nothing is written).
BERRY_API bool VoiceRandomBytes(void* out, size_t n);

/// X25519 public key for `secret32` (RFC 7748). False on a bad size or a
/// backend failure.
BERRY_API bool VoiceX25519PublicKey(const std::string& secret32, std::string* pub32);

/// X25519 shared secret from our `secret32` and the peer's `peerPub32`
/// (RFC 7748). False on a bad size, an invalid public key, or a backend
/// failure. The all-zero output of a low-order peer key is refused.
BERRY_API bool VoiceX25519SharedSecret(const std::string& secret32,
                                       const std::string& peerPub32,
                                       std::string* shared32);

/// HKDF-SHA256 (RFC 5869) expand/extract into exactly 32 bytes.
BERRY_API bool VoiceHkdfSha256(const std::string& ikm, const std::string& salt,
                               const std::string& info, std::string* out32);

/// An Ed25519 identity: the long-term key that authenticates a handshake. It is
/// derived deterministically from a 32-byte seed (nothing here reads
/// /dev/urandom), so a device stores the seed and a test pins the key.
class BERRY_API VoiceIdentity {
 public:
  VoiceIdentity() = default;

  /// Builds an identity from a 32-byte seed. Returns an invalid identity when
  /// the seed is the wrong size or the backend fails.
  static VoiceIdentity FromSeed(const std::string& seed32);
  /// Builds a *verification-only* identity from a 32-byte public key. It can
  /// verify but never sign, so a listener can be provisioned with peer keys and
  /// nothing else.
  static VoiceIdentity FromPublicKey(const std::string& pub32);

  bool valid() const { return pub_.size() == VoiceCrypto::kKeyBytes; }
  /// True when this identity holds a secret key and can sign.
  bool canSign() const { return sec_.size() == VoiceCrypto::kSecretBytes; }

  /// The 32-byte Ed25519 public key (raw).
  const std::string& publicKey() const { return pub_; }
  /// Lowercase hex of the public key, for config and fingerprints.
  std::string publicKeyHex() const;

  /// Signs `message`, writing a 64-byte signature. False when this identity has
  /// no secret key.
  bool Sign(const std::string& message, std::string* sig64) const;

 private:
  std::string pub_;
  std::string sec_;
};

/// What a peer's handshake is verified against. Fails closed: the default
/// (`None`) accepts nothing, so an unconfigured client has no voice peers.
struct BERRY_API VoiceTrustAnchor {
  enum class Kind : u32 { None = 0, Ed25519Keys, RoomKey };

  Kind kind = Kind::None;
  /// Ed25519Keys mode: the accepted peer public keys (raw 32 bytes each).
  std::vector<std::string> publicKeys;
  /// RoomKey mode: the shared secret issued by the backend (raw bytes).
  std::string roomKey;

  bool Valid() const;
  /// Ed25519Keys mode: true when `pub32` is an accepted peer key.
  bool Allows(const std::string& pub32) const;
  /// Adds a hex-encoded Ed25519 public key; returns false when it is malformed.
  bool AddPublicKeyHex(const std::string& hex);
  /// Sets the room key from raw bytes (any length >= 16 is accepted).
  bool SetRoomKey(const std::string& raw);
};

/// The decoded handshake announce. `auth` is an Ed25519 signature (Ed25519Keys
/// mode) or an HMAC-SHA256 tag (RoomKey mode) over `transcript`.
struct BERRY_API VoiceAnnounce {
  std::string identityKey;   // 32 raw bytes; empty in RoomKey mode
  std::string ephemeralKey;  // 32 raw X25519 public key
  std::string nonce;         // 16 raw bytes
  std::string auth;          // 64-byte signature or 32-byte MAC
};

/// The canonical bytes both sides sign/verify. Binds the format version, the
/// identity key, the ephemeral key and the nonce, so a field cannot be swapped.
BERRY_API std::string VoiceAnnounceTranscript(const VoiceAnnounce& a);

/// Encodes an announce for the wire (transcript || auth-length || auth).
BERRY_API std::string VoiceEncodeAnnounce(const VoiceAnnounce& a);

/// Decodes and structurally validates an announce. False on a bad magic,
/// version, length or field size. Does not verify the auth.
BERRY_API bool VoiceDecodeAnnounce(const std::string& wire, VoiceAnnounce* out);

/// Verifies an announce's auth against `anchor` and, on success, returns the
/// peer fingerprint (hex) that keys the session. In Ed25519Keys mode the
/// identity must be an allowed key and the signature must verify; in RoomKey
/// mode the HMAC over the transcript must verify and the fingerprint is derived
/// from the ephemeral key (there is no per-peer identity).
BERRY_API bool VoiceVerifyAnnounce(const VoiceAnnounce& a,
                                   const VoiceTrustAnchor& anchor,
                                   std::string* fingerprintHex);

/// A per-pair, per-direction AEAD session. One instance holds one peer's key and
/// is used both to seal frames to that peer and to open frames from it.
///
/// The nonce is `direction (4 bytes BE) || counter (8 bytes BE)`. The direction
/// is the *sender's* role in the pair (0 for the initiator, 1 for the
/// responder), so the two ends never share a nonce: this side seals with its own
/// role and opens with the peer's (the opposite). The counter is the frame's
/// sequence number and must be strictly increasing on send, so a nonce is never
/// reused under the key. On receive a sliding window rejects a replayed or
/// too-old sequence before the tag is even checked.
class BERRY_API VoiceSession {
 public:
  /// Derives a session from our ephemeral secret, the peer's ephemeral public
  /// key, and both handshake nonces. `initiator` fixes this side's role, so the
  /// two ends never share a nonce direction. Both ends derive the same key: the
  /// salt is SHA-256 over the two nonces in a fixed (sorted) order and the info
  /// string is constant.
  static bool Derive(const std::string& selfEphemeralSecret32,
                     const std::string& peerEphemeralPublic32,
                     const std::string& selfNonce16,
                     const std::string& peerNonce16, bool initiator,
                     VoiceSession* out);

  bool valid() const { return key_.size() == VoiceCrypto::kKeyBytes; }

  /// The 32-byte session key (for tests and diagnostics; never put on the wire).
  const std::string& key() const { return key_; }
  /// This session's AEAD nonce-direction byte, i.e. this side's send role
  /// (0 for the initiator, 1 for the responder).
  u8 direction() const { return direction_; }
  /// The direction used to open frames from the peer (the opposite role).
  u8 peerDirection() const { return (u8)(direction_ ^ 1); }

  /// Seals `plaintext` for frame `seq`, authenticating `aad` (the frame header).
  /// Refuses a `seq` that is not strictly greater than the last one sent, so a
  /// nonce is never reused. Output is `ciphertext || tag`.
  bool Seal(u64 seq, const std::string& aad, const std::string& plaintext,
            std::string* out);

  /// Opens a sealed frame from the peer, authenticating `aad`. False on a
  /// replayed/stale `seq`, a bad tag, a truncated input, or a malformed
  /// argument. On success the sequence is recorded, so a second call with the
  /// same `seq` fails.
  bool Open(u64 seq, const std::string& aad, const std::string& sealed,
            std::string* out);

  /// Forgets the send counter and the replay window, keeping the key.
  void Reset();

 private:
  std::string key_;
  u8 direction_ = 0;
  bool sentAny_ = false;
  u64 lastSent_ = 0;
  // Replay window (RFC 6479-style bitmap over the last 64 sequences).
  bool recvAny_ = false;
  u64 recvMax_ = 0;
  u64 recvBitmap_ = 0;
  static constexpr size_t kWindowBits = 64;

  bool AcceptSeq(u64 seq);
};

/// Owns the local identity, the trust anchor and the live sessions, and turns
/// announces into sessions. One instance per voice transport. Thread-safe is
/// the caller's job (the transport serialises access).
class BERRY_API VoiceSecureChannel {
 public:
  /// Configures the channel. `identity` must be able to sign (a listener that
  /// only verifies passes a signing identity too; it needs one to reply).
  /// Returns false when the identity is invalid, cannot sign, or the anchor is
  /// not valid. On failure the channel stays unconfigured and refuses
  /// everything.
  bool Configure(const VoiceIdentity& identity, const VoiceTrustAnchor& anchor);

  /// True when a signing identity and a valid anchor are both set.
  bool configured() const { return configured_; }
  /// True when the backend is compiled in.
  static bool available();

  /// Builds (once) and returns our signed announce, generating a fresh
  /// ephemeral X25519 key and nonce on the first call. The same announce is
  /// returned on every call until `RotateEphemeral`, so a re-sent announce is
  /// idempotent. Empty when unconfigured or on a backend failure.
  const std::string& announce();

  /// Verifies a received announce, derives the pairwise session and stores it
  /// under the peer's fingerprint. Returns the fingerprint (hex) on success, or
  /// an empty string when the announce is refused. The nonce direction is
  /// derived deterministically by comparing the two ephemeral public keys (the
  /// lexicographically lower key is the initiator), so both ends always pick
  /// opposite directions with no extra negotiation and this works in both
  /// trust modes.
  std::string AcceptAnnounce(const std::string& wire);

  /// The session for `fingerprintHex`, or null when none is established.
  VoiceSession* Session(const std::string& fingerprintHex);

  /// An 8-byte tag derived from a peer's (or our own) ephemeral public key. A
  /// sealed frame carries the *sender's* tag in the clear so the receiver can
  /// pick the session before decrypting; the tag is not secret and a forged one
  /// only fails the AEAD.
  static std::string RouteTagFor(const std::string& ephemeralPublic32);
  /// Our own route tag, stamped into every frame we seal. Empty when the
  /// channel has no ephemeral key yet.
  std::string ourRouteTag() const;
  /// The session a frame carrying `routeTag8` belongs to, or null. Scans the
  /// established peers; a tag is unique because it is derived from an ephemeral
  /// key and a peer that re-announces replaces its entry.
  VoiceSession* SessionByRoute(const std::string& routeTag8);
  /// Every established session, so a sender can seal one copy per peer (the
  /// multicast group is one-to-many but the keys are pairwise).
  std::vector<VoiceSession*> allSessions();

  /// Forgets every session (e.g. after rotating the ephemeral key). The identity
  /// and anchor are kept.
  void ClearSessions();
  size_t sessionCount() const { return sessions_.size(); }

  /// Discards the cached announce and every session, so the next `announce()`
  /// generates a new ephemeral key. Used on a reconnect.
  void RotateEphemeral();

 private:
  struct Peer {
    VoiceSession session;
    std::string nonce;      // the peer's handshake nonce
    std::string ephemeral;  // the peer's ephemeral public key (for its route tag)
    std::string routeTag;   // RouteTagFor(ephemeral), cached
  };

  /// Our own ephemeral public key, derived once when the announce is built.
  const std::string& localEphemeralPublic() const { return localEphPublic_; }

  bool configured_ = false;
  VoiceIdentity identity_;
  VoiceTrustAnchor anchor_;

  bool haveLocal_ = false;
  std::string localEphSecret_;
  std::string localEphPublic_;
  std::string localNonce_;
  std::string announce_;

  std::vector<std::pair<std::string, Peer>> sessions_;
};

}  // namespace Berry
