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
};

const char* CosmeticSyncResultName(CosmeticSyncResult r);

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
class BERRY_API CosmeticSyncChannel {
 public:
  CosmeticSyncChannel() = default;

  void SetTransport(CosmeticSyncTransport* t) { transport_ = t; }
  void SetVerifyKey(const CosmeticSyncKey& key);
  bool configured() const;
  void ClearVerifyKey();

  /// Signs the local loadout and hands it to the transport. `secretKey64` is
  /// the 64-byte Ed25519 secret (or the HMAC secret with `useHmac`). False when
  /// there is no transport or signing fails.
  bool Publish(const std::string& playerId, const CosmeticLoadout& loadout,
               const std::string& secretKey64, bool useHmac = false);

  /// Parses and verifies `message` without applying it. On Ok, `out` holds the
  /// decoded message. Fail closed on every malformed or unverifiable input.
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

 private:
  CosmeticSyncTransport* transport_ = nullptr;
  mutable std::mutex mutex_;
  CosmeticSyncKey key_;
  bool hasKey_ = false;
  u64 accepted_ = 0;
  u64 rejected_ = 0;
  CosmeticSyncResult last_ = CosmeticSyncResult::Ok;
};

}  // namespace Berry
