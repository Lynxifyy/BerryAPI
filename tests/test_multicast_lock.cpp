// Host tests for the Android multicast lock seam.
//
// On the host there is no Wi-Fi power management to defeat, so the seam must be
// a fail-closed no-op: `available()` is false, `Acquire` returns false and does
// not hold, and `Release` is safe to call without a lock. This proves the host
// build links the seam and that a transport's best-effort acquire cannot fail
// the open on a platform without the lock.
#include "../core/net/multicast_lock.h"

#include <cstdio>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

int main() {
#if defined(BERRY_PLATFORM_ANDROID)
  // On a device the seam reports available; the acquire itself needs a live JVM,
  // which a host test cannot provide, so only the availability is asserted here.
  CHECK(MulticastLock::available());
#else
  CHECK(!MulticastLock::available());
  MulticastLock lock;
  CHECK(!lock.Acquire("test"));
  CHECK(!lock.held());
  CHECK(!lock.usable());
  CHECK(!lock.lastError().empty());
  // Release without a lock is a no-op, not a crash.
  lock.Release();
  CHECK(!lock.held());
  // A second acquire is still refused, and still leaves the lock unheld.
  CHECK(!lock.Acquire("test"));
  CHECK(!lock.held());
#endif

  if (g_fail == 0) {
    printf("test_multicast_lock: ALL OK\n");
    return 0;
  }
  printf("test_multicast_lock: %d FAILURES\n", g_fail);
  return 1;
}
