#pragma once
// Internal framing shared by the voice transports.
//
// Two transports move the same secure datagrams: `SecureVoiceTransport` puts
// them on a LAN multicast group, `VoiceRelayTransport` hands them to a UDP
// relay for peers on another network. Both must speak byte-for-byte the same
// format — a sealed frame is only opened by a peer that parses it identically —
// so the format is defined once here rather than copied into each transport.
//
// This header is internal (not part of the public SDK surface) and lives under
// core/. See include/berry/voice_secure_transport.h for the threat model and the
// wire layout this encodes.

#include "berry/types.h"
#include "berry/voice_crypto.h"
#include "relay_wire.h"  // relaywire:: — shared by both UDP relays

#include <cstring>
#include <string>
#include <vector>

namespace Berry {
namespace voicewire {

/// The datagram magic for the secure voice format ("BVXS").
inline const char* Magic() {
  static char m[5] = {'B', 'V', 'X', 'S', '\0'};
  return m;
}
inline constexpr char kMagic[4] = {'B', 'V', 'X', 'S'};

inline constexpr u8 kVersion = 1;
inline constexpr u8 kKindAnnounce = 1;
inline constexpr u8 kKindSealedFrame = 2;
inline constexpr size_t kPrefixBytes = 6;         // magic + version + kind
inline constexpr size_t kRouteTagBytes = 8;
inline constexpr size_t kSealedHeaderBytes = 25;  // prefix + tag + seq + len + rsv
inline constexpr size_t kMaxIdBytes = 64;
inline constexpr size_t kAeadTagBytes = 16;

inline void PutU16(std::string* s, u16 v) {
  s->push_back((char)(u8)(v >> 8));
  s->push_back((char)(u8)v);
}

inline u16 GetU16(const char* p) {
  return (u16)(((u16)(u8)p[0] << 8) | (u8)p[1]);
}

inline void PutU64(std::string* s, u64 v) {
  for (int i = 0; i < 8; ++i) s->push_back((char)(u8)(v >> (8 * (7 - i))));
}

inline u64 GetU64(const char* p) {
  u64 v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | (u8)p[i];
  return v;
}

/// True when `data` starts with our magic and version. A datagram that fails
/// this is not ours and is dropped before any field is read.
inline bool HasPrefix(const char* data, size_t n) {
  return n >= kPrefixBytes && std::memcmp(data, kMagic, 4) == 0 &&
         (u8)data[4] == kVersion;
}

/// Builds an announce datagram: the fixed prefix followed by the encoded
/// announce. False when it would exceed `mtu`.
inline bool BuildAnnounce(const std::string& announce, size_t mtu,
                          std::string* out) {
  if (out == nullptr || announce.empty()) return false;
  if (kPrefixBytes + announce.size() > mtu) return false;
  std::string dg;
  dg.append(kMagic, 4);
  dg.push_back((char)kVersion);
  dg.push_back((char)kKindAnnounce);
  dg.append(announce);
  *out = std::move(dg);
  return true;
}

/// Builds one sealed frame datagram. The header up to and including the speaker
/// id is the AEAD associated data, so a sealed frame cannot be re-labelled under
/// a different route, id or sequence. False on a bad argument or an oversize
/// result.
inline bool BuildSealed(VoiceSession& session, const std::string& routeTag,
                        const std::string& speaker, u64 seq,
                        const std::vector<u8>& adpcm, size_t mtu,
                        std::string* out) {
  if (out == nullptr) return false;
  if (routeTag.size() != kRouteTagBytes) return false;
  if (speaker.empty() || speaker.size() > kMaxIdBytes) return false;
  if (adpcm.empty()) return false;

  std::string header;
  header.append(kMagic, 4);
  header.push_back((char)kVersion);
  header.push_back((char)kKindSealedFrame);
  header.append(routeTag);
  PutU64(&header, seq);
  PutU16(&header, (u16)speaker.size());
  header.push_back(0);  // reserved
  header.append(speaker);

  std::string sealed;
  if (!session.Seal(seq, header,
                    std::string((const char*)adpcm.data(), adpcm.size()),
                    &sealed)) {
    return false;
  }
  if (header.size() + sealed.size() > mtu) return false;
  header.append(sealed);
  *out = std::move(header);
  return true;
}

/// The outcome of decoding one received secure datagram.
struct Incoming {
  enum class Kind { Unknown, Announce, Frame };
  Kind kind = Kind::Unknown;
  /// Kind::Announce: the raw announce bytes (already offered to the channel).
  std::string announce;
  /// Kind::Frame: the decrypted frame.
  std::string speaker;
  u64 seq = 0;
  std::string plaintext;
  /// True when the announce was accepted (a session was established) or the
  /// frame opened; false when it was refused (bad auth, unknown route, replay).
  bool accepted = false;
};

/// Decodes one datagram and applies it to `channel`:
///   * an announce is verified through `channel.AcceptAnnounce`, which may
///     establish a session; `accepted` reports whether it did;
///   * a sealed frame is opened against the session its route tag names;
///     `accepted` is false on an unknown route, a bad tag or a replay.
/// A datagram that is not ours, or is malformed, returns Kind::Unknown. Every
/// failure changes no state beyond the channel's own replay bookkeeping.
inline Incoming Decode(const char* data, size_t n, VoiceSecureChannel& channel) {
  Incoming in;
  if (!HasPrefix(data, n)) return in;
  const u8 kind = (u8)data[5];
  if (kind == kKindAnnounce) {
    in.kind = Incoming::Kind::Announce;
    in.announce.assign(data + kPrefixBytes, n - kPrefixBytes);
    in.accepted = !channel.AcceptAnnounce(in.announce).empty();
    return in;
  }
  if (kind != kKindSealedFrame) return in;  // unknown kind: not ours
  if (n < kSealedHeaderBytes + 1) return in;
  in.kind = Incoming::Kind::Frame;
  const std::string routeTag(data + kPrefixBytes, kRouteTagBytes);
  in.seq = GetU64(data + kPrefixBytes + kRouteTagBytes);
  const size_t idLen = GetU16(data + kPrefixBytes + kRouteTagBytes + 8);
  if (idLen == 0 || idLen > kMaxIdBytes || n < kSealedHeaderBytes + idLen + kAeadTagBytes) {
    in.kind = Incoming::Kind::Unknown;
    return in;
  }
  const size_t idOff = kSealedHeaderBytes;
  in.speaker.assign(data + idOff, idLen);
  // The AAD is the header exactly as it arrived, up to and including the id.
  const std::string aad(data, idOff + idLen);
  const std::string sealed(data + idOff + idLen, n - idOff - idLen);
  VoiceSession* session = channel.SessionByRoute(routeTag);
  if (session == nullptr) return in;
  std::string plain;
  if (!session->Open(in.seq, aad, sealed, &plain) || plain.empty()) return in;
  in.plaintext = std::move(plain);
  in.accepted = true;
  return in;
}

}  // namespace voicewire

}  // namespace Berry
