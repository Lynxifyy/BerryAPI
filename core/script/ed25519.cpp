// Ed25519 (RFC 8032) wrappers around the vendored reference implementation.
//
// The signing path is deliberately asymmetric to the HMAC path it replaces:
// HMAC-SHA256 needs the same secret to sign and to verify, so any client that
// can verify a manifest can also mint one. Ed25519 separates the two — the
// device holds only a public key and can verify but never forge — which is what
// makes the signed manifest meaningful when the script and the verifier are on
// the same machine (the attacker's own phone).
//
// The vendored code is compiled as C with -DED25519_NO_SEED, so nothing reads
// /dev/urandom from inside the injected library; key generation is driven by a
// caller-supplied seed (see tools/berry-sign.py).
#include "berry/manifest.h"

#include "ed25519.h"

#include <cstring>

namespace Berry {
namespace crypto {

namespace {

bool IsHexChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

int HexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return c - 'A' + 10;
}

}  // namespace

std::string ToHexLower(const std::string& bytes) {
  static const char kHex[] = "0123456789abcdef";
  std::string out(bytes.size() * 2, '\0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    const u8 b = (u8)bytes[i];
    out[i * 2] = kHex[b >> 4];
    out[i * 2 + 1] = kHex[b & 0x0F];
  }
  return out;
}

bool FromHex(const std::string& hex, std::string* bytes, size_t expectedBytes) {
  if (!bytes) return false;
  if ((hex.size() % 2) != 0) return false;
  if (expectedBytes != 0 && hex.size() != expectedBytes * 2) return false;
  std::string out(hex.size() / 2, '\0');
  for (size_t i = 0; i < out.size(); ++i) {
    const char hi = hex[i * 2], lo = hex[i * 2 + 1];
    if (!IsHexChar(hi) || !IsHexChar(lo)) return false;
    out[i] = (char)((HexVal(hi) << 4) | HexVal(lo));
  }
  *bytes = std::move(out);
  return true;
}

bool Ed25519PublicKeyFromHex(const std::string& hex, std::string* out32) {
  return FromHex(hex, out32, 32);
}

// The 64-byte secret key is the conventional (seed || public key) pair, so it
// is interchangeable with the Python signer and with libsodium. The vendored
// code uses its own (clamped scalar || prefix) layout internally, so both are
// derived from the seed here rather than stored.
bool Ed25519KeypairFromSeed(const std::string& seed32, std::string* publicKey32,
                            std::string* secretKey64) {
  if (seed32.size() != 32 || !publicKey32 || !secretKey64) return false;
  unsigned char pk[32];
  unsigned char expanded[64];
  ed25519_create_keypair(pk, expanded, (const unsigned char*)seed32.data());
  publicKey32->assign((const char*)pk, sizeof pk);
  secretKey64->assign(seed32);
  secretKey64->append((const char*)pk, sizeof pk);
  return true;
}

bool Ed25519Sign(const std::string& message, const std::string& secretKey64,
                 std::string* signature64) {
  if (secretKey64.size() != 64 || !signature64) return false;
  const unsigned char* seed = (const unsigned char*)secretKey64.data();
  unsigned char pk[32];
  unsigned char expanded[64];
  ed25519_create_keypair(pk, expanded, seed);
  // Reject a secret key whose public half does not match its seed: signing
  // with a mismatched key would produce a signature nothing can verify.
  if (memcmp(pk, secretKey64.data() + 32, 32) != 0) return false;
  unsigned char sig[64];
  ed25519_sign(sig, (const unsigned char*)message.data(), message.size(), pk,
               expanded);
  signature64->assign((const char*)sig, sizeof sig);
  return true;
}

bool Ed25519Verify(const std::string& message, const std::string& signature64,
                   const std::string& publicKey32) {
  if (signature64.size() != 64 || publicKey32.size() != 32) return false;
  return ed25519_verify((const unsigned char*)signature64.data(),
                        (const unsigned char*)message.data(), message.size(),
                        (const unsigned char*)publicKey32.data()) != 0;
}

}  // namespace crypto
}  // namespace Berry
