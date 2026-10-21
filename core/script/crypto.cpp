// SHA-256 + HMAC-SHA256 (FIPS 180-4 / RFC 2104). Dependency-free so the
// injected library never links an external crypto runtime.
#include "berry/manifest.h"

#include <cstring>
#include <vector>

namespace Berry {
namespace crypto {
namespace {

constexpr u32 kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline u32 Rotr(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }

struct Sha256 {
  u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  u64 total = 0;
  u8 buf[64];
  size_t bufLen = 0;

  void Block(const u8* p) {
    u32 w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) |
             ((u32)p[i * 4 + 2] << 8) | ((u32)p[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      u32 s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      u32 s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3];
    u32 e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      u32 S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
      u32 ch = (e & f) ^ (~e & g);
      u32 t1 = hh + S1 + ch + kK[i] + w[i];
      u32 S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
      u32 maj = (a & b) ^ (a & c) ^ (b & c);
      u32 t2 = S0 + maj;
      hh = g; g = f; f = e; e = d + t1;
      d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }

  void Update(const u8* data, size_t len) {
    total += len;
    while (len > 0) {
      size_t take = 64 - bufLen;
      if (take > len) take = len;
      memcpy(buf + bufLen, data, take);
      bufLen += take;
      data += take;
      len -= take;
      if (bufLen == 64) {
        Block(buf);
        bufLen = 0;
      }
    }
  }

  void Final(u8 out[32]) {
    u64 bitLen = total * 8;
    u8 pad = 0x80;
    Update(&pad, 1);
    u8 zero = 0;
    while (bufLen != 56) Update(&zero, 1);
    u8 lenBytes[8];
    for (int i = 0; i < 8; ++i) lenBytes[i] = (u8)(bitLen >> (56 - i * 8));
    Update(lenBytes, 8);
    for (int i = 0; i < 8; ++i) {
      out[i * 4] = (u8)(h[i] >> 24);
      out[i * 4 + 1] = (u8)(h[i] >> 16);
      out[i * 4 + 2] = (u8)(h[i] >> 8);
      out[i * 4 + 3] = (u8)(h[i]);
    }
  }
};

std::string ToHex(const u8* bytes, size_t n) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.resize(n * 2);
  for (size_t i = 0; i < n; ++i) {
    out[i * 2] = kHex[bytes[i] >> 4];
    out[i * 2 + 1] = kHex[bytes[i] & 0x0F];
  }
  return out;
}

}  // namespace

std::string Sha256Hex(const void* data, size_t len) {
  Sha256 s;
  s.Update(reinterpret_cast<const u8*>(data), len);
  u8 digest[32];
  s.Final(digest);
  return ToHex(digest, 32);
}

std::string Sha256Hex(const std::string& data) {
  return Sha256Hex(data.data(), data.size());
}

std::string HmacSha256Hex(const std::string& key, const std::string& data) {
  u8 blockKey[64];
  memset(blockKey, 0, sizeof(blockKey));
  if (key.size() > 64) {
    u8 d[32];
    Sha256 s;
    s.Update(reinterpret_cast<const u8*>(key.data()), key.size());
    s.Final(d);
    memcpy(blockKey, d, 32);
  } else {
    memcpy(blockKey, key.data(), key.size());
  }

  u8 ipad[64], opad[64];
  for (int i = 0; i < 64; ++i) {
    ipad[i] = (u8)(blockKey[i] ^ 0x36);
    opad[i] = (u8)(blockKey[i] ^ 0x5c);
  }

  u8 inner[32];
  {
    Sha256 s;
    s.Update(ipad, 64);
    s.Update(reinterpret_cast<const u8*>(data.data()), data.size());
    s.Final(inner);
  }
  u8 outer[32];
  {
    Sha256 s;
    s.Update(opad, 64);
    s.Update(inner, 32);
    s.Final(outer);
  }
  return ToHex(outer, 32);
}

bool ConstantTimeEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  u8 diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    diff |= (u8)(a[i] ^ b[i]);
  }
  return diff == 0;
}

bool IsHex(const std::string& s) {
  if (s.empty() || (s.size() % 2) != 0) return false;
  for (char c : s) {
    bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    if (!hex) return false;
  }
  return true;
}

}  // namespace crypto
}  // namespace Berry
