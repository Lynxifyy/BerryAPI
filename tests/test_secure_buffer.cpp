// Host tests for the locked, zeroized key buffer (improvement 93).
#include "berry/secure_buffer.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace Berry;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
int main() {
  // Zero-size is safe.
  { SecureBuffer b(0); CHECK(b.size() == 0); b.Wipe(); }

  // A buffer holds what is assigned and wipes to zero.
  SecureBuffer b(32);
  CHECK(b.size() == 32);
  b.Assign("secret-key-material", 20);
  CHECK(memcmp(b.data(), "secret-key-material", 20) == 0);
  b.Wipe();
  for (size_t i = 0; i < b.size(); ++i) CHECK(b.data()[i] == 0);

  // Assign from a std::string.
  std::string s = "abcdef";
  SecureBuffer b2(8);
  b2.Assign(s);
  CHECK(memcmp(b2.data(), "abcdef", 6) == 0);

  // The cap is enforced.
  SecureBuffer big(SecureBuffer::kMaxBytes + 1000);
  CHECK(big.size() <= SecureBuffer::kMaxBytes);

  // A larger assign is truncated to the buffer size (never overruns).
  SecureBuffer small(4);
  small.Assign("abcdefgh", 8);
  CHECK(memcmp(small.data(), "abcd", 4) == 0);

  if (g_fail == 0) printf("test_secure_buffer: ALL OK\n");
  return g_fail;
}
