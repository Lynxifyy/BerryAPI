#pragma once
// Room tokens for the voice relay (F2).
//
// The relay's media plane is plain UDP because the audio is already end-to-end
// encrypted (voice_secure_transport.h). What must be authentic is the *room*:
// which relay to use and what secret keys the room. That is delivered over HTTPS
// by a backend, so the only TLS in the voice path is the token fetch.
//
// `RoomTokenClient` fetches a room token from a presence backend:
//
//   POST {baseUrl}/voice/room-token        (TLS, verified chain + hostname)
//   { "room": "<name>", "identity": "<ed25519 public key, hex>" }
//
// The backend answers with a token the client then hands to the relay
// transport:
//
//   { "room": "...", "secret": "<hex>", "relay_host": "203.0.113.9",
//     "relay_port": 47825, "expires_at": 1893456000000,
//     "signature": "<hex Ed25519 over the canonical token bytes>" }
//
// Verification is two-stage and both stages are pure functions, so the parsing
// is testable without a socket:
//   1. TLS: the transport is `Berry::HttpRequest` over `Berry::TlsSocket`, so an
//      untrusted chain, an expired certificate or a wrong hostname fails the
//      fetch (there is no insecure mode).
//   2. When `backendPublicKeyHex` is set, the token must carry an Ed25519
//      signature over its canonical bytes under that key. A tampered field (a
//      swapped relay, a shortened expiry, a different secret) fails the
//      signature, so a compromised TLS terminator alone cannot redirect voice.
//
// Fail closed: a malformed, unsigned-when-required, wrongly-signed, expired or
// wrong-room token is refused and changes no state.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/manifest.h"
#include "berry/net.h"
#include "berry/voice_crypto.h"

#include <string>

namespace Berry {

/// A fetched room token: where the room's relay is and the secret that keys it.
struct BERRY_API RoomToken {
  std::string room;
  /// Raw room-secret bytes (hex-decoded from the response).
  std::string secret;
  /// The relay's numeric IPv4 address (as returned; the transport refuses a
  /// name, so the backend must send an address).
  std::string relayHost;
  u16 relayPort = 0;
  /// Wall-clock expiry in milliseconds since the epoch; 0 means "no expiry".
  u64 expiresAtMs = 0;

  bool expired(u64 nowMs) const {
    return expiresAtMs != 0 && nowMs >= expiresAtMs;
  }
  bool valid() const {
    return !room.empty() && secret.size() >= 16 && !relayHost.empty() &&
           relayPort != 0;
  }
};

struct BERRY_API RoomTokenClientConfig {
  /// The backend base URL, e.g. "https://presence.example.com". Must be https:
  /// a token over plaintext would hand the room secret to anything on the path.
  std::string baseUrl;
  /// The backend's Ed25519 public key (hex, 32 bytes). When set, the token's
  /// signature is required and verified. When empty, only TLS authenticates the
  /// token — acceptable for a deployment that terminates TLS at the backend.
  std::string backendPublicKeyHex;
  /// The request timeout in milliseconds (an absolute deadline for the whole
  /// request, like every other Berry request).
  int timeoutMs = 8000;
};

/// The canonical bytes a backend signs for a token, so a client can verify it
/// without the backend's JSON writer. Deterministic and domain-separated.
BERRY_API std::string RoomTokenSignedBytes(const RoomToken& token);

class BERRY_API RoomTokenClient {
 public:
  explicit RoomTokenClient(RoomTokenClientConfig cfg = {}) : cfg_(std::move(cfg)) {}
  void SetConfig(RoomTokenClientConfig cfg) { cfg_ = std::move(cfg); }
  const RoomTokenClientConfig& config() const { return cfg_; }

  /// Fetches and verifies a token for `room`, presenting `identity`'s public key
  /// to the backend. `nowMs` is the clock used for the expiry check (passed in
  /// so a test pins it). Returns false and sets `error` on any failure.
  bool Fetch(const std::string& room, const VoiceIdentity& identity,
             u64 nowMs, RoomToken* out, std::string* error = nullptr);

  /// Parses and verifies a token document (the backend's JSON body) without a
  /// socket, so the verification rules are testable on their own. `signedBytes`
  /// is built from the parsed token; a `signature` member is required when
  /// `backendPublicKeyHex` is set.
  bool VerifyDocument(const std::string& body, const std::string& room,
                      u64 nowMs, RoomToken* out, std::string* error = nullptr);

  /// The endpoint path appended to the base URL.
  static const char* EndpointPath() { return "/voice/room-token"; }

 private:
  RoomTokenClientConfig cfg_;
};

}  // namespace Berry
