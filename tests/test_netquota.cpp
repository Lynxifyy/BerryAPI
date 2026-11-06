// Host tests for per-script network quotas (improvement 53).
#include "berry/netquota.h"
#include <cstdio>
using namespace Berry;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
int main() {
  NetQuotaTracker q;
  // No limits -> always allowed.
  CHECK(q.AllowConnect("s", 0) == NetQuotaResult::Ok);

  NetQuotaLimits lim;
  lim.maxConcurrentSockets = 2;
  lim.requestsPerSecond = 3;
  lim.bytesPerSecond = 100;
  q.SetLimits("s", lim);
  CHECK(q.Limits("s").maxConcurrentSockets == 2);

  // Concurrent socket cap.
  CHECK(q.AllowConnect("s", 0) == NetQuotaResult::Ok); q.SocketOpened("s");
  CHECK(q.AllowConnect("s", 0) == NetQuotaResult::Ok); q.SocketOpened("s");
  CHECK(q.OpenSockets("s") == 2);
  CHECK(q.AllowConnect("s", 0) == NetQuotaResult::TooManySockets);
  q.SocketClosed("s");
  CHECK(q.OpenSockets("s") == 1);

  // Request rate cap within a second window.
  NetQuotaTracker r;
  NetQuotaLimits rl; rl.requestsPerSecond = 2;
  r.SetLimits("a", rl);
  CHECK(r.AllowConnect("a", 100) == NetQuotaResult::Ok);
  CHECK(r.AllowConnect("a", 100) == NetQuotaResult::Ok);
  CHECK(r.AllowConnect("a", 100) == NetQuotaResult::RateLimited);
  // A new second resets the window.
  CHECK(r.AllowConnect("a", 1200) == NetQuotaResult::Ok);

  // Byte budget.
  NetQuotaTracker b;
  NetQuotaLimits bl; bl.bytesPerSecond = 100;
  b.SetLimits("b", bl);
  CHECK(b.ChargeBytes("b", 60, 0) == NetQuotaResult::Ok);
  CHECK(b.BytesRemaining("b", 0) == 40);
  CHECK(b.ChargeBytes("b", 50, 0) == NetQuotaResult::ByteBudgetExhausted);
  CHECK(b.ChargeBytes("b", 40, 0) == NetQuotaResult::Ok);
  CHECK(b.BytesRemaining("b", 0) == 0);
  // A new second resets the byte budget.
  CHECK(b.ChargeBytes("b", 100, 1000) == NetQuotaResult::Ok);

  // Forget and Clear.
  q.Forget("s");
  CHECK(q.OpenSockets("s") == 0);
  CHECK(q.Limits("s").maxConcurrentSockets == 0);
  b.Clear();
  CHECK(b.ChargeBytes("b", 1000000, 0) == NetQuotaResult::Ok);   // no limits now

  if (g_fail == 0) printf("test_netquota: ALL OK\n");
  return g_fail;
}
