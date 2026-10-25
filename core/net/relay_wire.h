#pragma once
// Internal relay envelope shared by the UDP relays (voice F2, cosmetic sync).
//
// A relay is a small UDP forwarder that moves *already-encrypted / already-
// signed* payloads between registered peers in a room. The control plane — how
// a peer proves it knows the room and how the relay matches a datagram to a
// room — is identical whether the payload is a sealed voice frame or a signed
// cosmetic envelope, so it is defined once here rather than copied into each
// relay. `VoiceRelay`/`VoiceRelayTransport` and `CosmeticRelay`/
// `CosmeticRelayTransport` all speak this envelope.
//
// Envelope (big-endian), one datagram per message:
//
//   offset size  field
//   0      4     magic 'B','V','R','Y'
//   4      1     version (1)
//   5      1     kind: 1 = register, 2 = data
//   6      64    room tag, lowercase hex of HMAC-SHA256(secret,
//                "berry-relay-room" || room)
//   70     ...   kind 1: auth, 64 hex chars of HMAC-SHA256(secret,
//                "berry-relay-register" || roomTag); kind 2: the opaque inner
//                payload (a secure-voice datagram, or a cosmetic fragment).
//
// The relay never holds a key and never decrypts; it only matches the room tag
// (constant-time) and forwards a registered source's data to the room's other
// members. See include/berry/voice_relay.h for the voice relay's own threat
// model and include/berry/cosmetic_relay.h for the cosmetic one.

#include "berry/types.h"

#include <cstring>
#include <string>

namespace Berry {
namespace relaywire {

inline constexpr char kMagic[4] = {'B', 'V', 'R', 'Y'};
inline constexpr u8 kVersion = 1;
inline constexpr u8 kKindRegister = 1;
inline constexpr u8 kKindData = 2;
inline constexpr size_t kTagBytes = 64;       // hex room tag
inline constexpr size_t kAuthBytes = 64;      // hex register auth
inline constexpr size_t kEnvelopeBytes = 70;  // magic + version + kind + tag
inline constexpr size_t kMinSecretBytes = 16;
inline constexpr char kRoomDomain[] = "berry-relay-room";
inline constexpr char kRegisterDomain[] = "berry-relay-register";

inline bool HasPrefix(const char* data, size_t n) {
  return n >= kEnvelopeBytes && std::memcmp(data, kMagic, 4) == 0 &&
         (u8)data[4] == kVersion;
}

/// Builds the envelope prefix (magic + version + kind + tag).
inline void PutPrefix(u8 kind, const std::string& tagHex, std::string* out) {
  out->append(kMagic, 4);
  out->push_back((char)kVersion);
  out->push_back((char)kind);
  out->append(tagHex);
}

/// A data datagram: the prefix followed by the opaque inner payload. False when
/// `tagHex` is not exactly kTagBytes, `inner` is empty, or the result exceeds
/// `mtu`.
inline bool BuildData(const std::string& tagHex, const std::string& inner,
                      size_t mtu, std::string* out) {
  if (out == nullptr || tagHex.size() != kTagBytes || inner.empty()) return false;
  if (kEnvelopeBytes + inner.size() > mtu) return false;
  std::string dg;
  PutPrefix(kKindData, tagHex, &dg);
  dg.append(inner);
  *out = std::move(dg);
  return true;
}

/// A register datagram: the prefix followed by the auth hex. False on a bad tag
/// or auth length, or an oversize result.
inline bool BuildRegister(const std::string& tagHex, const std::string& authHex,
                          size_t mtu, std::string* out) {
  if (out == nullptr || tagHex.size() != kTagBytes ||
      authHex.size() != kAuthBytes) {
    return false;
  }
  if (kEnvelopeBytes + kAuthBytes > mtu) return false;
  std::string dg;
  PutPrefix(kKindRegister, tagHex, &dg);
  dg.append(authHex);
  *out = std::move(dg);
  return true;
}

}  // namespace relaywire
}  // namespace Berry
