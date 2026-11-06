// Host tests for the metrics registry (improvement 91).
//
// Covers counters (monotonic add), gauges (up and down), histograms (count,
// mean, min/max, bucket placement), the snapshot views, and Clear.

#include "berry/metrics.h"

#include <cstdio>
#include <string>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

int main() {
  MetricsRegistry& reg = MetricsRegistry::Instance();
  reg.Clear();

  // --- counters ---
  {
    Counter* c = reg.CounterFor("combat.hits");
    CHECK(c != nullptr);
    CHECK(c->Value() == 0);
    c->Add();
    c->Add(4);
    CHECK(c->Value() == 5);
    // The same name returns the same handle.
    CHECK(reg.CounterFor("combat.hits") == c);
  }

  // --- gauges ---
  {
    Gauge* g = reg.GaugeFor("world.entities");
    g->Set(10);
    CHECK(g->Value() == 10);
    g->Add(-3);
    CHECK(g->Value() == 7);
    g->Set(-1);   // a gauge may legitimately go negative
    CHECK(g->Value() == -1);
  }

  // --- histograms ---
  {
    Histogram* h = reg.HistogramFor("frame.time_ms");
    CHECK(h->Count() == 0);
    CHECK(h->Mean() == 0.0);
    h->Observe(1.0);
    h->Observe(3.0);
    h->Observe(9.0);
    CHECK(h->Count() == 3);
    CHECK(h->Min() == 1.0);
    CHECK(h->Max() == 9.0);
    CHECK(h->Mean() > 4.3 && h->Mean() < 4.4);   // (1+3+9)/3 = 4.333
    std::vector<u64> buckets = h->Buckets();
    CHECK(buckets.size() == Histogram::BoundCount() + 1);
    u64 total = 0;
    for (u64 b : buckets) total += b;
    CHECK(total == 3);   // every sample landed in exactly one bucket
  }

  // --- snapshots ---
  {
    auto snap = reg.Snapshot();
    bool sawCounter = false, sawGauge = false;
    for (const auto& kv : snap) {
      if (kv.first == "combat.hits") { sawCounter = true; CHECK(kv.second == 5); }
      if (kv.first == "world.entities") { sawGauge = true; CHECK(kv.second == -1); }
    }
    CHECK(sawCounter && sawGauge);

    auto hs = reg.HistogramSnapshot();
    bool sawHist = false;
    for (const auto& s : hs) {
      if (s.name == "frame.time_ms") {
        sawHist = true;
        CHECK(s.count == 3);
        CHECK(s.max == 9.0);
      }
    }
    CHECK(sawHist);
  }

  CHECK(reg.Count() == 3);

  // --- clear ---
  reg.Clear();
  CHECK(reg.Count() == 0);
  // A cleared registry hands out fresh, zeroed metrics.
  CHECK(reg.CounterFor("combat.hits")->Value() == 0);

  if (g_fail == 0) printf("test_metrics: ALL OK\n");
  return g_fail;
}
