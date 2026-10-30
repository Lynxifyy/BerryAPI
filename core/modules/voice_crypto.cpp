// Voice authentication and encryption. See include/berry/voice_crypto.h for the
// design and the threat model.
//
// mbedTLS (X25519 via ECP/ECDH, HKDF-SHA256, ChaCha20-Poly1305) and the vendored
// Ed25519 do the primitives; this file only frames them, enforces the sizes, and
// fails closed. The mbedTLS types stay in this translation unit, so no caller
// needs mbedTLS's include path.
#include "berry/voice_crypto.h"

#include "berry/manifest.h"   // crypto:: (SHA-256, HMAC, hex, Ed25519, constant-time)

#include <algorithm>
#include <cstring>
#include <mutex>

#include "mbedtls/chachapoly.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"

namespace Berry {

namespace {

constexpr char kAnnounceMagic[4] = {'B', 'V', 'A', 'N'};
constexpr u8 kAnnounceVersion = 1;
constexpr u8 kModeEd25519 = 0;
constexpr u8 kModeRoomKey = 1;

// The HKDF info string, bound into every derived session key. A change here
// changes every key, so it is a wire-format version, not a tuning knob.
constexpr char kKdfInfo[] = "berry-voice-session-v1";
// Domain separation for the handshake salt, so a nonce pair cannot be reused
// across a future protocol.
constexpr char kSaltLabel[] = "berry-voice-handshake-v1";

//--- CSPRNG -----------------------------------------------------------------
//
// One process-wide DRBG, seeded lazily from the platform entropy source. It is
// used for ephemeral X25519 keys, handshake nonces and the ECDH blinding. A
// failed seed is remembered, so nothing falls back to a weak source.
std::mutex g_rngMutex;
mbedtls_entropy_context g_entropy;
mbedtls_ctr_drbg_context g_drbg;
bool g_rngInit = false;
bool g_rngOk = false;

bool EnsureRng() {
  std::lock_guard<std::mutex> lk(g_rngMutex);
  if (g_rngInit) return g_rngOk;
  g_rngInit = true;
  mbedtls_entropy_init(&g_entropy);
  mbedtls_ctr_drbg_init(&g_drbg);
  const char* pers = "berry-voice-crypto";
  if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                            (const unsigned char*)pers,
                            std::strlen(pers)) != 0) {
    g_rngOk = false;
    return false;
  }
  g_rngOk = true;
  return true;
}

/// The DRBG callback mbedTLS's ECDH/ECP want, and a small helper that returns
/// false when the RNG is unusable (so a caller fails closed rather than using a
/// deterministic scalar).
int DrbgRandom(void* ctx, unsigned char* out, size_t len) {
  (void)ctx;
  return mbedtls_ctr_drbg_random(&g_drbg, out, len);
}

/// Decodes a hex digest (which the crypto helpers return) into raw bytes. The
/// input is always our own well-formed hex, so a failure here is a bug; it is
/// still checked so nothing trusts an unexpected value.
std::string Unhex(const std::string& hex) {
  std::string out;
  if (!crypto::FromHex(hex, &out, 0)) return {};
  return out;
}

void AppendU16(std::string* s, u16 v) {
  s->push_back((char)(v >> 8));
  s->push_back((char)v);
}

u16 ReadU16(const char* p) {
  return (u16)(((u16)(u8)p[0] << 8) | (u8)p[1]);
}

//--- X25519 -----------------------------------------------------------------

/// Clamps a raw X25519 secret in place, per RFC 7748 section 5. Applied by the
/// caller before the scalar is used, so a non-canonical seed cannot produce a
/// different key than the peer expects.
void ClampX25519(std::string* k) {
  u8* p = (u8*)&(*k)[0];
  p[0] &= 248;
  p[31] &= 127;
  p[31] |= 64;
}

/// R = d * P on Curve25519 (Montgomery ladder). `scalar32` must already be
/// clamped. Returns false on a bad size or a backend failure.
bool X25519Mul(std::string* out32, const std::string& scalar32,
               const std::string& point32) {
  if (out32 == nullptr) return false;
  if (scalar32.size() != VoiceCrypto::kEphemeralBytes) return false;
  if (point32.size() != VoiceCrypto::kEphemeralBytes) return false;
  if (!EnsureRng()) return false;

  mbedtls_ecp_group grp;
  mbedtls_mpi d;
  mbedtls_mpi z;
  mbedtls_ecp_point P;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_mpi_init(&z);
  mbedtls_ecp_point_init(&P);
  bool ok = false;
  do {
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) break;
    if (mbedtls_mpi_read_binary_le(&d, (const unsigned char*)scalar32.data(),
                                   scalar32.size()) != 0) {
      break;
    }
    // Reject a scalar that is not a valid X25519 private key (wrong bit
    // pattern); the peer's clamp would otherwise diverge from ours.
    if (mbedtls_ecp_check_privkey(&grp, &d) != 0) break;
    if (mbedtls_ecp_point_read_binary(&grp, &P,
                                      (const unsigned char*)point32.data(),
                                      point32.size()) != 0) {
      break;
    }
    if (mbedtls_ecdh_compute_shared(&grp, &z, &P, &d, DrbgRandom, nullptr) != 0) {
      break;
    }
    std::string out(VoiceCrypto::kEphemeralBytes, '\0');
    if (mbedtls_mpi_write_binary_le(&z, (unsigned char*)&out[0], out.size()) !=
        0) {
      break;
    }
    // A low-order peer key yields the all-zero shared secret; RFC 7748 says to
    // reject it, and it would make every such peer share one key.
    bool allZero = true;
    for (unsigned char c : out) {
      if (c != 0) { allZero = false; break; }
    }
    if (allZero) break;
    *out32 = std::move(out);
    ok = true;
  } while (false);

  mbedtls_ecp_point_free(&P);
  mbedtls_mpi_free(&z);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

}  // namespace

//----------------------------------------------------------------------------
// Primitives
//----------------------------------------------------------------------------

const char* VoiceAnnounceMagic() {
  static char magic[5] = {kAnnounceMagic[0], kAnnounceMagic[1], kAnnounceMagic[2],
                          kAnnounceMagic[3], '\0'};
  return magic;
}

bool VoiceCryptoAvailable() { return EnsureRng(); }

bool VoiceRandomBytes(void* out, size_t n) {
  if (out == nullptr && n != 0) return false;
  if (n == 0) return true;
  if (!EnsureRng()) return false;
  std::lock_guard<std::mutex> lk(g_rngMutex);
  return mbedtls_ctr_drbg_random(&g_drbg, (unsigned char*)out, n) == 0;
}

bool VoiceX25519PublicKey(const std::string& secret32, std::string* pub32) {
  if (pub32 == nullptr) return false;
  if (secret32.size() != VoiceCrypto::kEphemeralBytes) return false;
  if (!EnsureRng()) return false;

  std::string clamped = secret32;
  ClampX25519(&clamped);

  mbedtls_ecp_group grp;
  mbedtls_mpi d;
  mbedtls_ecp_point R;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_ecp_point_init(&R);
  bool ok = false;
  do {
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) break;
    if (mbedtls_mpi_read_binary_le(&d, (const unsigned char*)clamped.data(),
                                   clamped.size()) != 0) {
      break;
    }
    // The ladder needs an RNG (for blinding) even when the base point is fixed.
    if (mbedtls_ecp_mul(&grp, &R, &d, &grp.G, DrbgRandom, nullptr) != 0) break;
    std::string out(VoiceCrypto::kEphemeralBytes, '\0');
    size_t olen = 0;
    if (mbedtls_ecp_point_write_binary(&grp, &R, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &olen, (unsigned char*)&out[0],
                                       out.size()) != 0) {
      break;
    }
    if (olen != out.size()) break;
    *pub32 = std::move(out);
    ok = true;
  } while (false);

  mbedtls_ecp_point_free(&R);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

bool VoiceX25519SharedSecret(const std::string& secret32,
                             const std::string& peerPub32,
                             std::string* shared32) {
  if (shared32 == nullptr) return false;
  if (secret32.size() != VoiceCrypto::kEphemeralBytes) return false;
  if (peerPub32.size() != VoiceCrypto::kEphemeralBytes) return false;
  std::string clamped = secret32;
  ClampX25519(&clamped);
  return X25519Mul(shared32, clamped, peerPub32);
}

bool VoiceHkdfSha256(const std::string& ikm, const std::string& salt,
                     const std::string& info, std::string* out32) {
  if (out32 == nullptr) return false;
  if (ikm.empty()) return false;
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md == nullptr) return false;
  std::string out(VoiceCrypto::kKeyBytes, '\0');
  const int ret = mbedtls_hkdf(
      md, (const unsigned char*)salt.data(), salt.size(),
      (const unsigned char*)ikm.data(), ikm.size(),
      (const unsigned char*)info.data(), info.size(),
      (unsigned char*)&out[0], out.size());
  if (ret != 0) return false;
  *out32 = std::move(out);
  return true;
}

//----------------------------------------------------------------------------
// Identity
//----------------------------------------------------------------------------

VoiceIdentity VoiceIdentity::FromSeed(const std::string& seed32) {
  VoiceIdentity id;
  std::string pub, sec;
  if (!crypto::Ed25519KeypairFromSeed(seed32, &pub, &sec)) return id;
  id.pub_ = std::move(pub);
  id.sec_ = std::move(sec);
  return id;
}

VoiceIdentity VoiceIdentity::FromPublicKey(const std::string& pub32) {
  VoiceIdentity id;
  if (pub32.size() != VoiceCrypto::kKeyBytes) return id;
  id.pub_ = pub32;
  return id;
}

std::string VoiceIdentity::publicKeyHex() const {
  return crypto::ToHexLower(pub_);
}

bool VoiceIdentity::Sign(const std::string& message, std::string* sig64) const {
  if (!canSign() || sig64 == nullptr) return false;
  return crypto::Ed25519Sign(message, sec_, sig64);
}

//----------------------------------------------------------------------------
// Trust anchor
//----------------------------------------------------------------------------

bool VoiceTrustAnchor::Valid() const {
  if (kind == Kind::Ed25519Keys) {
    if (publicKeys.empty()) return false;
    for (const std::string& k : publicKeys) {
      if (k.size() != VoiceCrypto::kKeyBytes) return false;
    }
    return true;
  }
  if (kind == Kind::RoomKey) {
    // A room key shorter than 16 bytes is too weak to call a key; refuse it so a
    // truncated config fails closed instead of weakening the channel.
    return roomKey.size() >= 16;
  }
  return false;   // None: accepts nothing
}

bool VoiceTrustAnchor::Allows(const std::string& pub32) const {
  if (kind != Kind::Ed25519Keys) return false;
  for (const std::string& k : publicKeys) {
    if (crypto::ConstantTimeEquals(crypto::ToHexLower(k),
                                   crypto::ToHexLower(pub32))) {
      return true;
    }
  }
  return false;
}

bool VoiceTrustAnchor::AddPublicKeyHex(const std::string& hex) {
  std::string raw;
  if (!crypto::Ed25519PublicKeyFromHex(hex, &raw)) return false;
  publicKeys.push_back(std::move(raw));
  return true;
}

bool VoiceTrustAnchor::SetRoomKey(const std::string& raw) {
  if (raw.size() < 16) return false;
  roomKey = raw;
  kind = Kind::RoomKey;
  return true;
}

//----------------------------------------------------------------------------
// Announce framing
//----------------------------------------------------------------------------

std::string VoiceAnnounceTranscript(const VoiceAnnounce& a) {
  // Length-prefixed, so no field can be shifted into another's bytes.
  std::string t;
  t.append(kAnnounceMagic, 4);
  t.push_back((char)kAnnounceVersion);
  t.push_back(a.identityKey.empty() ? (char)kModeRoomKey : (char)kModeEd25519);
  AppendU16(&t, (u16)std::min<size_t>(a.identityKey.size(), 0xffff));
  t.append(a.identityKey);
  AppendU16(&t, (u16)std::min<size_t>(a.ephemeralKey.size(), 0xffff));
  t.append(a.ephemeralKey);
  AppendU16(&t, (u16)std::min<size_t>(a.nonce.size(), 0xffff));
  t.append(a.nonce);
  return t;
}

std::string VoiceEncodeAnnounce(const VoiceAnnounce& a) {
  const std::string t = VoiceAnnounceTranscript(a);
  std::string wire;
  wire.append(kAnnounceMagic, 4);
  wire.push_back((char)kAnnounceVersion);
  wire.push_back(a.identityKey.empty() ? (char)kModeRoomKey : (char)kModeEd25519);
  AppendU16(&wire, (u16)std::min<size_t>(t.size(), 0xffff));
  wire.append(t);
  AppendU16(&wire, (u16)std::min<size_t>(a.auth.size(), 0xffff));
  wire.append(a.auth);
  return wire;
}

bool VoiceDecodeAnnounce(const std::string& wire, VoiceAnnounce* out) {
  if (out == nullptr) return false;
  // magic(4) + version(1) + mode(1) + tlen(2) = 8
  if (wire.size() < 8) return false;
  if (std::memcmp(wire.data(), kAnnounceMagic, 4) != 0) return false;
  if ((u8)wire[4] != kAnnounceVersion) return false;
  const u8 mode = (u8)wire[5];
  if (mode != kModeEd25519 && mode != kModeRoomKey) return false;
  const size_t tlen = ReadU16(&wire[6]);
  if (tlen == 0 || 8 + tlen + 2 > wire.size()) return false;
  const std::string transcript = wire.substr(8, tlen);
  const size_t authLen = ReadU16(&wire[8 + tlen]);
  const size_t authOff = 8 + tlen + 2;
  if (authOff + authLen != wire.size()) return false;   // no trailing slack

  // The transcript is self-describing; re-parse it into fields and require it to
  // re-encode to the exact bytes we received, so a caller cannot be handed a
  // transcript that differs from what it will verify.
  if (transcript.size() < 5) return false;
  if (std::memcmp(transcript.data(), kAnnounceMagic, 4) != 0) return false;
  if ((u8)transcript[4] != kAnnounceVersion) return false;
  if ((u8)transcript[5] != mode) return false;
  size_t off = 6;
  auto readField = [&](std::string* field) -> bool {
    if (off + 2 > transcript.size()) return false;
    const size_t n = ReadU16(&transcript[off]);
    off += 2;
    if (off + n > transcript.size()) return false;
    field->assign(transcript, off, n);
    off += n;
    return true;
  };
  VoiceAnnounce a;
  std::string idKey, eph, nonce;
  if (!readField(&idKey) || !readField(&eph) || !readField(&nonce)) return false;
  if (off != transcript.size()) return false;

  const size_t expectedAuth = (mode == kModeEd25519)
                                  ? VoiceCrypto::kSignatureBytes
                                  : VoiceCrypto::kMacBytes;
  if (authLen != expectedAuth) return false;
  if (mode == kModeEd25519 && idKey.size() != VoiceCrypto::kKeyBytes) return false;
  if (mode == kModeRoomKey && !idKey.empty()) return false;
  if (eph.size() != VoiceCrypto::kEphemeralBytes) return false;
  if (nonce.size() != VoiceCrypto::kNonceBytes) return false;

  a.identityKey = std::move(idKey);
  a.ephemeralKey = std::move(eph);
  a.nonce = std::move(nonce);
  a.auth = wire.substr(authOff, authLen);
  *out = std::move(a);
  return true;
}

bool VoiceVerifyAnnounce(const VoiceAnnounce& a,
                         const VoiceTrustAnchor& anchor,
                         std::string* fingerprintHex) {
  if (fingerprintHex == nullptr) return false;
  if (!anchor.Valid()) return false;
  const std::string transcript = VoiceAnnounceTranscript(a);

  if (anchor.kind == VoiceTrustAnchor::Kind::Ed25519Keys) {
    if (a.identityKey.size() != VoiceCrypto::kKeyBytes) return false;
    if (a.auth.size() != VoiceCrypto::kSignatureBytes) return false;
    if (!anchor.Allows(a.identityKey)) return false;
    if (!crypto::Ed25519Verify(transcript, a.auth, a.identityKey)) return false;
    // The peer is keyed by its identity, so two announces from the same
    // identity collapse to one session even across an ephemeral rotation.
    *fingerprintHex = crypto::ToHexLower(a.identityKey);
    return true;
  }

  // RoomKey mode: the HMAC proves the peer holds the backend-issued key. There
  // is no per-peer identity, so the peer is keyed by its ephemeral key (which
  // its HMAC also covers, so it cannot be swapped by a third party).
  if (a.identityKey.size() != 0) return false;
  if (a.auth.size() != VoiceCrypto::kMacBytes) return false;
  const std::string expect =
      crypto::HmacSha256Hex(anchor.roomKey, transcript);
  if (!crypto::ConstantTimeEquals(expect, crypto::ToHexLower(a.auth))) {
    return false;
  }
  *fingerprintHex = crypto::ToHexLower(a.ephemeralKey);
  return true;
}

//----------------------------------------------------------------------------
// Session
//----------------------------------------------------------------------------

namespace {

/// The handshake salt: SHA-256 over the label and the two nonces in a fixed
/// order, so both ends compute the same bytes regardless of who is initiator.
/// Returns the raw 32-byte digest.
std::string HandshakeSalt(const std::string& nonceA, const std::string& nonceB) {
  const std::string& lo = (nonceA <= nonceB) ? nonceA : nonceB;
  const std::string& hi = (nonceA <= nonceB) ? nonceB : nonceA;
  std::string cat(kSaltLabel);
  cat.append(lo);
  cat.append(hi);
  return Unhex(crypto::Sha256Hex(cat));
}

}  // namespace

bool VoiceSession::Derive(const std::string& selfEphemeralSecret32,
                          const std::string& peerEphemeralPublic32,
                          const std::string& selfNonce16,
                          const std::string& peerNonce16, bool initiator,
                          VoiceSession* out) {
  if (out == nullptr) return false;
  if (selfEphemeralSecret32.size() != VoiceCrypto::kEphemeralBytes) return false;
  if (peerEphemeralPublic32.size() != VoiceCrypto::kEphemeralBytes) return false;
  if (selfNonce16.size() != VoiceCrypto::kNonceBytes) return false;
  if (peerNonce16.size() != VoiceCrypto::kNonceBytes) return false;

  std::string shared;
  if (!VoiceX25519SharedSecret(selfEphemeralSecret32, peerEphemeralPublic32,
                               &shared)) {
    return false;
  }
  const std::string salt = HandshakeSalt(selfNonce16, peerNonce16);
  std::string key;
  if (!VoiceHkdfSha256(shared, salt, kKdfInfo, &key)) return false;

  VoiceSession s;
  s.key_ = std::move(key);
  s.direction_ = initiator ? 0 : 1;
  *out = std::move(s);
  return true;
}

void VoiceSession::Reset() {
  sentAny_ = false;
  lastSent_ = 0;
  recvAny_ = false;
  recvMax_ = 0;
  recvBitmap_ = 0;
}

bool VoiceSession::AcceptSeq(u64 seq) {
  // Convention: bit `delta` in `recvBitmap_` is set when sequence
  // `recvMax_ - delta` has been accepted, so bit 0 always tracks the maximum.
  if (!recvAny_) {
    recvAny_ = true;
    recvMax_ = seq;
    recvBitmap_ = 1;
    return true;
  }
  if (seq > recvMax_) {
    const u64 shift = seq - recvMax_;
    if (shift >= kWindowBits) {
      recvBitmap_ = 1;   // everything older than the window is forgotten
    } else {
      // The old maximum (bit 0) moves up to bit `shift`; bit 0 becomes the new
      // maximum.
      recvBitmap_ = (recvBitmap_ << shift) | 1;
    }
    recvMax_ = seq;
    return true;
  }
  const u64 delta = recvMax_ - seq;
  if (delta >= kWindowBits) return false;   // older than the window: refuse
  const u64 bit = 1ull << delta;
  if ((recvBitmap_ & bit) != 0) return false;   // already seen: a replay
  recvBitmap_ |= bit;
  return true;
}

namespace {

/// Builds the 12-byte AEAD nonce: direction (4 bytes BE) || counter (8 bytes BE).
/// The direction is a fixed per-session constant and the counter is the frame
/// sequence, so a nonce never repeats under the key.
void BuildNonce(u8 direction, u64 counter, unsigned char out[12]) {
  out[0] = 0;
  out[1] = 0;
  out[2] = 0;
  out[3] = direction;
  for (int i = 0; i < 8; ++i) {
    out[4 + i] = (unsigned char)(counter >> (8 * (7 - i)));
  }
}

}  // namespace

bool VoiceSession::Seal(u64 seq, const std::string& aad,
                        const std::string& plaintext, std::string* out) {
  if (out == nullptr) return false;
  if (key_.size() != VoiceCrypto::kKeyBytes) return false;
  // Strictly increasing, so the counter never wraps onto a used nonce. Voice is
  // real-time; a wrap would need 2^64 frames, but refuse rather than risk it.
  if (sentAny_ && seq <= lastSent_) return false;
  if (plaintext.size() > (size_t)0x7fffffff) return false;

  unsigned char nonce[12];
  BuildNonce(direction_, seq, nonce);

  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  bool ok = false;
  do {
    if (mbedtls_chachapoly_setkey(&ctx, (const unsigned char*)key_.data()) != 0) {
      break;
    }
    std::string sealed(plaintext.size() + VoiceCrypto::kAeadTagBytes, '\0');
    const int ret = mbedtls_chachapoly_encrypt_and_tag(
        &ctx, plaintext.size(), nonce,
        (const unsigned char*)aad.data(), aad.size(),
        (const unsigned char*)plaintext.data(), (unsigned char*)&sealed[0],
        (unsigned char*)&sealed[plaintext.size()]);
    if (ret != 0) break;
    lastSent_ = seq;
    sentAny_ = true;
    *out = std::move(sealed);
    ok = true;
  } while (false);
  mbedtls_chachapoly_free(&ctx);
  return ok;
}

bool VoiceSession::Open(u64 seq, const std::string& aad,
                        const std::string& sealed, std::string* out) {
  if (out == nullptr) return false;
  if (key_.size() != VoiceCrypto::kKeyBytes) return false;
  if (sealed.size() < VoiceCrypto::kAeadTagBytes) return false;

  // Snapshot the replay window so a frame whose tag fails does not consume a
  // window slot: a forged frame must not be able to push a real one out of the
  // window and lock it out. The accept is committed only after the tag verifies.
  const bool savedAny = recvAny_;
  const u64 savedMax = recvMax_;
  const u64 savedBitmap = recvBitmap_;
  if (!AcceptSeq(seq)) return false;

  const size_t ctLen = sealed.size() - VoiceCrypto::kAeadTagBytes;
  unsigned char nonce[12];
  BuildNonce(peerDirection(), seq, nonce);

  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  bool ok = false;
  do {
    if (mbedtls_chachapoly_setkey(&ctx, (const unsigned char*)key_.data()) != 0) {
      break;
    }
    std::string plain(ctLen, '\0');
    const int ret = mbedtls_chachapoly_auth_decrypt(
        &ctx, ctLen, nonce, (const unsigned char*)aad.data(), aad.size(),
        (const unsigned char*)&sealed[ctLen],
        (const unsigned char*)sealed.data(), (unsigned char*)&plain[0]);
    if (ret != 0) break;   // bad tag: not authentic
    *out = std::move(plain);
    ok = true;
  } while (false);
  mbedtls_chachapoly_free(&ctx);

  if (!ok) {
    recvAny_ = savedAny;
    recvMax_ = savedMax;
    recvBitmap_ = savedBitmap;
    return false;
  }
  return true;
}

//----------------------------------------------------------------------------
// Secure channel
//----------------------------------------------------------------------------

bool VoiceSecureChannel::available() { return VoiceCryptoAvailable(); }

bool VoiceSecureChannel::Configure(const VoiceIdentity& identity,
                                   const VoiceTrustAnchor& anchor) {
  if (!identity.valid() || !identity.canSign()) return false;
  if (!anchor.Valid()) return false;
  identity_ = identity;
  anchor_ = anchor;
  configured_ = true;
  RotateEphemeral();
  return true;
}

void VoiceSecureChannel::RotateEphemeral() {
  haveLocal_ = false;
  localEphSecret_.clear();
  localEphPublic_.clear();
  localNonce_.clear();
  announce_.clear();
  sessions_.clear();
}

const std::string& VoiceSecureChannel::announce() {
  if (!configured_) return announce_;
  if (haveLocal_) return announce_;

  std::string ephSecret(VoiceCrypto::kEphemeralBytes, '\0');
  if (!VoiceRandomBytes(&ephSecret[0], ephSecret.size())) return announce_;
  std::string ephPub;
  if (!VoiceX25519PublicKey(ephSecret, &ephPub)) return announce_;

  std::string nonce(VoiceCrypto::kNonceBytes, '\0');
  if (!VoiceRandomBytes(&nonce[0], nonce.size())) return announce_;

  VoiceAnnounce a;
  a.ephemeralKey = ephPub;
  a.nonce = nonce;
  if (anchor_.kind == VoiceTrustAnchor::Kind::Ed25519Keys) {
    a.identityKey = identity_.publicKey();
    if (!identity_.Sign(VoiceAnnounceTranscript(a), &a.auth)) return announce_;
  } else {
    // RoomKey mode: no identity key on the wire; the HMAC is over the
    // transcript, which already binds the ephemeral key and the nonce.
    a.identityKey.clear();
    a.auth = Unhex(
        crypto::HmacSha256Hex(anchor_.roomKey, VoiceAnnounceTranscript(a)));
  }

  localEphSecret_ = ephSecret;
  localEphPublic_ = ephPub;
  localNonce_ = nonce;
  announce_ = VoiceEncodeAnnounce(a);
  haveLocal_ = !announce_.empty();
  return announce_;
}

std::string VoiceSecureChannel::AcceptAnnounce(const std::string& wire) {
  if (!configured_) return {};
  if (announce().empty()) return {};   // we must be able to reply with our nonce

  VoiceAnnounce a;
  if (!VoiceDecodeAnnounce(wire, &a)) return {};
  std::string fingerprint;
  if (!VoiceVerifyAnnounce(a, anchor_, &fingerprint)) return {};

  // A peer whose ephemeral key equals ours is a reflection of our own announce
  // (or a copy of it): refuse, so we never derive a session with ourselves and
  // never accept a replayed announce as a peer.
  const std::string& ourPub = localEphPublic_;
  if (ourPub.empty()) return {};
  if (ourPub == a.ephemeralKey) return {};

  // Direction: the lexicographically lower ephemeral key is the initiator. Both
  // ends compare the same two keys, so they always disagree — no extra
  // negotiation, and it works in both trust modes.
  const bool initiator = ourPub < a.ephemeralKey;

  VoiceSession session;
  if (!VoiceSession::Derive(localEphSecret_, a.ephemeralKey, localNonce_, a.nonce,
                            initiator, &session)) {
    return {};
  }

  for (auto& [fp, peer] : sessions_) {
    if (fp == fingerprint) {
      peer.session = std::move(session);
      peer.nonce = a.nonce;
      peer.ephemeral = a.ephemeralKey;
      peer.routeTag = RouteTagFor(a.ephemeralKey);
      return fingerprint;
    }
  }
  Peer p;
  p.session = std::move(session);
  p.nonce = a.nonce;
  p.ephemeral = a.ephemeralKey;
  p.routeTag = RouteTagFor(a.ephemeralKey);
  sessions_.emplace_back(fingerprint, std::move(p));
  return fingerprint;
}

VoiceSession* VoiceSecureChannel::Session(const std::string& fingerprintHex) {
  for (auto& [fp, peer] : sessions_) {
    if (fp == fingerprintHex) return &peer.session;
  }
  return nullptr;
}

std::string VoiceSecureChannel::RouteTagFor(const std::string& ephemeralPublic32) {
  if (ephemeralPublic32.size() != VoiceCrypto::kEphemeralBytes) return {};
  // SHA-256 over a domain label and the ephemeral key; the first 8 bytes are the
  // tag. Truncation is fine here: the tag only routes, the AEAD authenticates.
  std::string cat("berry-voice-route-v1");
  cat.append(ephemeralPublic32);
  const std::string digest = Unhex(crypto::Sha256Hex(cat));
  if (digest.size() < 8) return {};
  return digest.substr(0, 8);
}

std::string VoiceSecureChannel::ourRouteTag() const {
  if (!haveLocal_) return {};
  return RouteTagFor(localEphemeralPublic());
}

VoiceSession* VoiceSecureChannel::SessionByRoute(const std::string& routeTag8) {
  if (routeTag8.empty()) return nullptr;
  for (auto& [fp, peer] : sessions_) {
    if (peer.routeTag == routeTag8) return &peer.session;
  }
  return nullptr;
}

std::vector<VoiceSession*> VoiceSecureChannel::allSessions() {
  std::vector<VoiceSession*> out;
  out.reserve(sessions_.size());
  for (auto& [fp, peer] : sessions_) out.push_back(&peer.session);
  return out;
}

void VoiceSecureChannel::ClearSessions() { sessions_.clear(); }

}  // namespace Berry
