// Host tests for the compiled-bytecode cache (improvement 62).
#include "berry/bytecode_cache.h"
#include <cstdio>
#include <string>
using namespace Berry;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
int main() {
  BytecodeCache c(3, 1024);
  std::string bc;
  // Miss on empty.
  CHECK(!c.Get("a", &bc));
  CHECK(c.misses() == 1);

  c.Put("a", "AAAA");
  CHECK(c.Count() == 1);
  CHECK(c.Bytes() == 4);
  CHECK(c.Get("a", &bc));
  CHECK(bc == "AAAA");
  CHECK(c.hits() == 1);

  // Different source is a different key.
  CHECK(!c.Get("b", &bc));

  // The key is the source hash, so a changed source misses.
  c.Put("a", "AAAA");
  CHECK(c.Get("a", &bc) && bc == "AAAA");

  // Eviction by count (max 3): adding a 4th drops the oldest.
  c.Clear();
  c.Put("1", "x");
  c.Put("2", "x");
  c.Put("3", "x");
  c.Put("4", "x");   // evicts "1"
  CHECK(c.Count() == 3);
  CHECK(!c.Get("1", &bc));
  CHECK(c.Get("2", &bc));

  // Eviction by bytes: a large entry pushes the budget over.
  c.Clear();
  BytecodeCache small(64, 10);
  small.Put("big", std::string(8, 'z'));
  CHECK(small.Bytes() == 8);
  small.Put("big2", std::string(8, 'z'));   // 16 > 10 -> evict the older
  CHECK(small.Bytes() <= 10);
  CHECK(small.Count() == 1);

  // An entry larger than the whole budget is not stored.
  small.Put("huge", std::string(100, 'q'));
  CHECK(small.Count() == 1);

  // Clear.
  c.Clear();
  CHECK(c.Count() == 0 && c.Bytes() == 0);

  if (g_fail == 0) printf("test_bytecode_cache: ALL OK\n");
  return g_fail;
}
