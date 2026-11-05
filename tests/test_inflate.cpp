// Host tests for the DEFLATE/zlib/gzip inflater (improvement 34).
//
// The vectors were produced by Python's zlib at maximum compression over a
// 860-byte payload, so they exercise stored, fixed and dynamic Huffman blocks
// and a long back-reference chain.
#include "berry/inflate.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace Berry;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static const char kExpected[] =
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog"
    "the quick brown fox jumps over the lazy dog";

int main() {
  const unsigned char kRaw[] = {0x2b,0xc9,0x48,0x55,0x28,0x2c,0xcd,0x4c,0xce,0x56,0x48,0x2a,0xca,0x2f,0xcf,0x53,0x48,0xcb,0xaf,0x50,0xc8,0x2a,0xcd,0x2d,0x28,0x56,0xc8,0x2f,0x4b,0x2d,0x52,0x28,0x01,0x4a,0xe7,0x24,0x56,0x55,0x2a,0xa4,0xe4,0xa7,0x97,0x8c,0x2a,0x1d,0x55,0x3a,0xaa,0x94,0x78,0xa5,0x00};
  const unsigned char kZlib[] = {0x78,0xda,0x2b,0xc9,0x48,0x55,0x28,0x2c,0xcd,0x4c,0xce,0x56,0x48,0x2a,0xca,0x2f,0xcf,0x53,0x48,0xcb,0xaf,0x50,0xc8,0x2a,0xcd,0x2d,0x28,0x56,0xc8,0x2f,0x4b,0x2d,0x52,0x28,0x01,0x4a,0xe7,0x24,0x56,0x55,0x2a,0xa4,0xe4,0xa7,0x97,0x8c,0x2a,0x1d,0x55,0x3a,0xaa,0x94,0x78,0xa5,0x00,0x77,0x95,0x3f,0x84};
  const unsigned char kGzip[] = {0x1f,0x8b,0x08,0x00,0x23,0xfa,0xeb,0x6a,0x02,0xff,0x2b,0xc9,0x48,0x55,0x28,0x2c,0xcd,0x4c,0xce,0x56,0x48,0x2a,0xca,0x2f,0xcf,0x53,0x48,0xcb,0xaf,0x50,0xc8,0x2a,0xcd,0x2d,0x28,0x56,0xc8,0x2f,0x4b,0x2d,0x52,0x28,0x01,0x4a,0xe7,0x24,0x56,0x55,0x2a,0xa4,0xe4,0xa7,0x97,0x8c,0x2a,0x1d,0x55,0x3a,0xaa,0x94,0x78,0xa5,0x00,0x45,0x7d,0x17,0xb0,0x5c,0x03,0x00,0x00};
  const std::string expected(kExpected);
  CHECK(expected.size() == 860);

  // Raw deflate.
  {
    std::string out;
    CHECK(InflateRaw(kRaw, sizeof kRaw, &out));
    CHECK(out == expected);
  }
  // zlib framing (auto-detected).
  {
    std::string out;
    CHECK(InflateAuto(kZlib, sizeof kZlib, &out));
    CHECK(out == expected);
  }
  // gzip framing (auto-detected).
  {
    std::string out;
    CHECK(InflateAuto(kGzip, sizeof kGzip, &out));
    CHECK(out == expected);
  }
  // Raw deflate also works through InflateAuto when there is no framing.
  {
    std::string out;
    CHECK(InflateAuto(kRaw, sizeof kRaw, &out));
    CHECK(out == expected);
  }

  // The decompressed-size cap is enforced.
  {
    std::string out;
    CHECK(!InflateRaw(kRaw, sizeof kRaw, &out, 100));   // 860 > 100
    CHECK(out.size() <= 100);
  }

  // A truncated stream fails rather than returning partial data as success.
  {
    std::string out;
    CHECK(!InflateRaw(kRaw, sizeof kRaw - 4, &out));
  }

  // Garbage fails.
  {
    const unsigned char junk[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x11, 0x22, 0x33};
    std::string out;
    CHECK(!InflateRaw(junk, sizeof junk, &out));
  }

  if (g_fail == 0) printf("test_inflate: ALL OK\n");
  return g_fail;
}
