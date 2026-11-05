// Host tests for the drift-corrected interval scheduler (improvement 70).
//
// Covers: a repeating timer fires once per interval; a stall catches up by
// whole intervals instead of drifting; a one-shot fires once; cancel (single
// and per-script); the cap; and a callback that cancels its own timer.

#include "berry/timers.h"

#include <cstdio>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

int main() {
  // --- a repeating timer fires once per interval ---
  {
    TimerScheduler s;
    std::vector<GameTick> fires;
    GameTick tick = 0;
    u64 id = s.Every(5, "a", [&](u64) { fires.push_back(tick); }, tick);
    CHECK(id != 0);
    CHECK(s.Count() == 1);
    for (tick = 1; tick <= 20; ++tick) s.Advance(tick);
    // Fires at 5, 10, 15, 20.
    CHECK(fires.size() == 4);
    CHECK(fires[0] == 5 && fires[3] == 20);
  }

  // --- a stall catches up, no drift ---
  {
    TimerScheduler s;
    int count = 0;
    GameTick tick = 0;
    s.Every(10, "a", [&](u64) { ++count; }, tick);
    // Jump from 0 to 35: three intervals (10, 20, 30) have elapsed.
    s.Advance(35);
    CHECK(count == 3);
    // The next fire is at 40 (a multiple of 10), not 45 (35 + 10).
    s.Advance(39);
    CHECK(count == 3);
    s.Advance(40);
    CHECK(count == 4);
  }

  // --- a one-shot fires once and is removed ---
  {
    TimerScheduler s;
    int count = 0;
    s.After(3, "a", [&](u64) { ++count; }, 0);
    for (GameTick t = 1; t <= 10; ++t) s.Advance(t);
    CHECK(count == 1);
    CHECK(s.Count() == 0);
  }

  // --- cancel ---
  {
    TimerScheduler s;
    int count = 0;
    u64 id = s.Every(2, "a", [&](u64) { ++count; }, 0);
    CHECK(s.Cancel(id));
    CHECK(!s.Cancel(id));   // idempotent
    s.Advance(100);
    CHECK(count == 0);
  }

  // --- cancel per script ---
  {
    TimerScheduler s;
    s.Every(1, "a", [](u64) {}, 0);
    s.Every(1, "a", [](u64) {}, 0);
    s.Every(1, "b", [](u64) {}, 0);
    CHECK(s.Count() == 3);
    CHECK(s.CancelScript("a") == 2);
    CHECK(s.Count() == 1);
  }

  // --- a callback that cancels its own timer stops the repetition ---
  {
    TimerScheduler s;
    int count = 0;
    u64 id = 0;
    id = s.Every(1, "a", [&](u64 self) { ++count; s.Cancel(self); }, 0);
    s.Advance(1);
    s.Advance(2);
    s.Advance(3);
    CHECK(count == 1);
    CHECK(s.Count() == 0);
    (void)id;
  }

  // --- refusals ---
  {
    TimerScheduler s;
    CHECK(s.Every(0, "a", [](u64) {}, 0) == 0);   // zero interval
    CHECK(s.Every(1, "a", nullptr, 0) == 0);      // no callback
  }

  // --- the cap ---
  {
    TimerScheduler s;
    for (size_t i = 0; i < TimerScheduler::kMaxTimers; ++i) {
      CHECK(s.Every(100, "a", [](u64) {}, 0) != 0);
    }
    CHECK(s.Every(100, "a", [](u64) {}, 0) == 0);   // over the cap
  }

  if (g_fail == 0) printf("test_timers: ALL OK\n");
  return g_fail;
}
