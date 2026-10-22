#pragma once
// BerryAPI cosmetic sync channel.
//
// Cosmetics render on your own screen by default. Making another copy of the
// client show what you wear needs a channel that tells it — and that channel is
// the security boundary, because a peer that could send any loadout could wear
// anything. This layer is that boundary:
//
//   * A loadout is serialized with its owner into a canonical payload and
//     signed (Ed25519 in production, HMAC for local development, the same
//     anchors the script manifest uses).
//   * A received message is rejected unless the envelope is well formed, the
//     algorithm is known, the signature verifies over the exact payload, and
//     the loadout parses. Fail closed at every step.
//   * Even a validly signed loadout is applied only for assets the named
//     player provably owns (CosmeticService::SetRemoteLoadout enforces this),
//     so a compromised peer cannot wear someone else's cosmetics.
//
// The transport is a seam: the device wires a WebSocket (Berry::Network); the
// host harness uses an in-memory capture. Nothing here opens a socket itself.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/cosmetic.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {

/// Why a sync message was accepted or refused. Every non-Ok value means the
/// message was dropped and no state changed.
enum class CosmeticSyncResult : u32 {
  Ok = 0,
  EmptyMessage,
  TooLarge,
  BadEnvelope,        // missing/!matching magic, header, or length framing
  UnknownAlgorithm,
  BadSignature,
  BadLoadout,         // the loadout body did not parse
  NoTransport,
  NotConfigured,      // no verification key set
  Replayed,           // already accepted within the replay window
  RateLimited,        // the peer is sending faster than its budget
  Revoked,            // the peer's key is on the revocation list
};

const char* CosmeticSyncResultName(CosmeticSyncResult r);

/// Limits that keep a received stream from being abused. All are fail-closed:
/// exceeding one drops the message and changes no state.
///
/// The replay window remembers the digest of every accepted message for up to
/// `replayTtlMs`, keeping at most `replayWindow` of them. A message whose
/// digest is still remembered is refused, so a valid signed loadout cannot be
/// replayed to re-apply it (and re-broadcast it) after the fact. The window is
/// bounded, so a message older than the TTL — or pushed out by `replayWindow`
/// newer messages — is no longer covered; that is the standard trade-off.
///
/// The rate limit is a per-peer token bucket: a peer may burst `rateBurst`
/// messages, refilling at `ratePerSecond`, so a peer cannot flood the channel
/// with a stream of individually-valid loadouts. `maxPeers` caps the peer table
/// so a flood of distinct (attacker-chosen) ids cannot grow it without bound;
/// when it is full the least-recently-seen peer is evicted.
struct BERRY_API CosmeticSyncConfig {
  size_t replayWindow = 512;
  u64 replayTtlMs = 120000;      // 2 minutes
  double rateBurst = 16.0;       // tokens a peer starts with
  double ratePerSecond = 8.0;    // token refill rate
  size_t maxPeers = 64;
};

/// The wire format version tag. A peer that does not speak this exact version
/// is refused rather than guessed at.
BERRY_API const char* CosmeticSyncMagic();

/// Verification key for received messages. Mirrors TrustAnchor's two modes so a
/// device stores a public key and can verify but never mint.
struct BERRY_API CosmeticSyncKey {
  enum class Kind : u32 { Ed25519 = 0, Hmac };

  Kind kind = Kind::Ed25519;
  std::string keyHex;

  static CosmeticSyncKey Ed25519PublicKey(std::string hex);
  static CosmeticSyncKey HmacSecret(std::string secret);

  bool Valid() const;
};

/// Where a signed message goes. The device implementation writes to a
/// WebSocket; tests capture it. Returning false means "not delivered", which
/// Publish() reports to the caller.
class BERRY_API CosmeticSyncTransport {
 public:
  virtual ~CosmeticSyncTransport() = default;
  virtual bool Send(const std::string& message) = 0;
};

/// Cap on an accepted message, so a peer cannot make the client allocate an
/// unbounded buffer while parsing.
BERRY_API size_t CosmeticSyncMaxMessageBytes();

/// Builds the canonical, signed payload for (playerId, loadout). Exposed so the
/// signer and verifier provably agree on the bytes, and so a test can pin it.
BERRY_API std::string CosmeticSyncPayload(const std::string& playerId,
                                          const CosmeticLoadout& loadout);

/// Frames a signed message: the canonical payload plus the algorithm and
/// signature, in the documented envelope. `signatureHex` must already be the
/// signature over CosmeticSyncPayload(playerId, loadout).
BERRY_API std::string CosmeticSyncFrame(const std::string& playerId,
                                        const CosmeticLoadout& loadout,
                                        const std::string& algo,
                                        const std::string& signatureHex);

/// Signs the payload with a 64-byte Ed25519 secret key and frames it.
BERRY_API bool CosmeticSyncSignEd25519(const std::string& playerId,
                                       const CosmeticLoadout& loadout,
                                       const std::string& secretKey64,
                                       std::string* outMessage);

/// Signs the payload with an HMAC secret and frames it (local development).
BERRY_API bool CosmeticSyncSignHmac(const std::string& playerId,
                                    const CosmeticLoadout& loadout,
                                    const std::string& secret,
                                    std::string* outMessage);

/// A parsed, verified sync message.
struct BERRY_API CosmeticSyncMessage {
  std::string playerId;
  CosmeticLoadout loadout;
  std::string algo;
};

/// The channel: holds the verification key and the transport, publishes the
/// local loadout and applies verified remote ones.
///
/// Beyond verifying signatures, the channel defends against a valid-but-hostile
/// peer: it refuses a replayed message, rate-limits a flooding one, and refuses
/// a peer whose key has been revoked. These checks run after the signature
/// verifies, so an unauthenticated message never reaches them (and cannot fill
/// the replay window or a peer's bucket).
class BERRY_API CosmeticSyncChannel {
 public:
  CosmeticSyncChannel() = default;
  explicit CosmeticSyncChannel(const CosmeticSyncConfig& config)
      : config_(config) {}

  void SetConfig(const CosmeticSyncConfig& config);
  const CosmeticSyncConfig& config() const { return config_; }

  /// Supplies a monotonic millisecond clock for the replay TTL and the rate
  /// limiter. Defaults to a steady clock; a test injects a fake one so timing
  /// is deterministic. The callable must be safe to invoke under the channel's
  /// lock and must not call back into the channel.
  void SetClock(std::function<u64()> nowMs);

  void SetTransport(CosmeticSyncTransport* t) { transport_ = t; }
  void SetVerifyKey(const CosmeticSyncKey& key);
  bool configured() const;
  void ClearVerifyKey();

  //--- revocation -----------------------------------------------------------
  /// Revokes a peer id. Any later message from that id is refused, even with a
  /// valid signature. This is the moderation lever: a compromised or hostile
  /// peer can be shut out without rotating the shared verification key.
  void RevokePeer(const std::string& playerId);
  bool IsPeerRevoked(const std::string& playerId) const;
  void ClearRevocations();

  /// Signs the local loadout and hands it to the transport. `secretKey64` is
  /// the 64-byte Ed25519 secret (or the HMAC secret with `useHmac`). False when
  /// there is no transport or signing fails.
  bool Publish(const std::string& playerId, const CosmeticLoadout& loadout,
               const std::string& secretKey64, bool useHmac = false);

  /// Parses and verifies `message` without applying it. On Ok, `out` holds the
  /// decoded message. Fail closed on every malformed or unverifiable input.
  /// The replay/rate/revocation checks are applied by Receive, not here: Verify
  /// is a pure signature/shape check with no side effects.
  CosmeticSyncResult Verify(const std::string& message,
                            CosmeticSyncMessage* out) const;

  /// Verifies `message` and, when it passes, installs it on `service` (which
  /// further keeps only assets the player owns). `applied` receives how many
  /// slots survived. No state changes on a non-Ok result.
  CosmeticSyncResult Receive(const std::string& message,
                             CosmeticService* service,
                             size_t* applied = nullptr);

  u64 accepted() const;
  u64 rejected() const;
  /// The last rejection reason, for diagnostics.
  CosmeticSyncResult lastResult() const;

  /// How many distinct peers the rate limiter is currently tracking.
  size_t trackedPeers() const;
  /// How many digests the replay window is currently holding.
  size_t replayWindowSize() const;

 private:
  struct PeerState {
    double tokens = 0;
    u64 lastMs = 0;
    u64 lastSeen = 0;   // for LRU eviction
  };

  /// Applies the post-signature defences for an already-verified message.
  /// Caller must hold `mutex_`. Returns Ok to proceed, or the refusal reason.
  CosmeticSyncResult AdmitLocked(const std::string& playerId,
                                 const std::string& message, u64 now);

  /// Evicts expired digests (and trims to the window size). Caller holds the
  /// lock. `now` is the current clock.
  void ExpireReplayLocked(u64 now);

  /// Fetches (or creates) the rate-limiter bucket for a peer, evicting the
  /// least-recently-seen peer when the table is at `maxPeers`. A new bucket
  /// starts full and stamped with `now`, so the first message spends a token
  /// and the next refills from that instant. Caller holds the lock.
  PeerState& PeerLocked(const std::string& playerId, u64 now);

  CosmeticSyncTransport* transport_ = nullptr;
  mutable std::mutex mutex_;
  CosmeticSyncKey key_;
  bool hasKey_ = false;
  CosmeticSyncConfig config_;
  std::function<u64()> clock_;

  // Revoked peer ids. Small and expected to stay small, so a linear scan is
  // fine; checked under the lock.
  std::vector<std::string> revokedPeers_;

  // Replay window: digests of recently-accepted messages, oldest first, each
  // with the time it was accepted.
  struct SeenDigest {
    std::string digest;
    u64 atMs = 0;
  };
  std::vector<SeenDigest> seen_;

  // Per-peer rate limiting.
  std::unordered_map<std::string, PeerState> peers_;
  u64 peerClock_ = 0;

  u64 accepted_ = 0;
  u64 rejected_ = 0;
  CosmeticSyncResult last_ = CosmeticSyncResult::Ok;
};


}  // namespace Berry
