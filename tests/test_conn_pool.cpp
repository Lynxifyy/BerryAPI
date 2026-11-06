// Host tests for the keep-alive connection pool (improvement 36).
#include "berry/conn_pool.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace Berry;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
int main() {
  // A trivial connection: an id. The "close" callback records closed ids.
  struct C { int id = 0; bool operator==(const C& o) const { return id == o.id; } };
  std::vector<int> closed;
  auto close = [&](C& c) { if (c.id) closed.push_back(c.id); };

  ConnPool<C> pool(2, 1000);
  // Acquire with nothing idle returns a default (id 0).
  CHECK(pool.Acquire("h:80", 0, close).id == 0);

  // Release two, then acquire them back (LIFO within the key is not required,
  // but the connection must come back).
  pool.Release("h:80", C{1}, 0, close);
  pool.Release("h:80", C{2}, 0, close);
  CHECK(pool.IdleCount() == 2);
  C a = pool.Acquire("h:80", 0, close);
  C b = pool.Acquire("h:80", 0, close);
  CHECK(a.id != 0 && b.id != 0 && a.id != b.id);
  CHECK(pool.IdleCount() == 0);

  // A key with no idle connection returns a default.
  CHECK(pool.Acquire("other:80", 0, close).id == 0);

  // Idle timeout: a connection older than the timeout is closed on acquire.
  pool.Release("h:80", C{7}, 0, close);
  closed.clear();
  CHECK(pool.Acquire("h:80", 5000, close).id == 0);   // expired -> discarded
  CHECK(closed.size() == 1 && closed[0] == 7);
  CHECK(pool.IdleCount() == 0);

  // The pool bound: releasing beyond maxIdle evicts the oldest.
  closed.clear();
  pool.Release("a:1", C{10}, 100, close);
  pool.Release("b:1", C{20}, 200, close);
  pool.Release("c:1", C{30}, 300, close);   // over maxIdle=2 -> evict oldest (10)
  CHECK(pool.IdleCount() == 2);
  CHECK(closed.size() == 1 && closed[0] == 10);

  // Clear closes everything.
  closed.clear();
  pool.Clear(close);
  CHECK(pool.IdleCount() == 0);
  CHECK(closed.size() == 2);

  if (g_fail == 0) printf("test_conn_pool: ALL OK\n");
  return g_fail;
}
