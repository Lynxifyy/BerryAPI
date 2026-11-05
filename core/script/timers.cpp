// Drift-corrected interval scheduler. See include/berry/timers.h.
#include "berry/timers.h"

#include <algorithm>

namespace Berry {

u64 TimerScheduler::Every(u64 intervalTicks, const std::string& script,
                          Callback cb, GameTick nowTick) {
  if (intervalTicks == 0 || !cb) return 0;
  if (timers_.size() >= kMaxTimers) return 0;
  Timer t;
  t.id = nextId_++;
  t.interval = intervalTicks;
  t.next = nowTick + (GameTick)intervalTicks;
  t.script = script;
  t.cb = std::move(cb);
  const u64 id = t.id;
  timers_.push_back(std::move(t));
  return id;
}

u64 TimerScheduler::After(u64 delayTicks, const std::string& script,
                          Callback cb, GameTick nowTick) {
  if (!cb) return 0;
  if (timers_.size() >= kMaxTimers) return 0;
  Timer t;
  t.id = nextId_++;
  t.interval = 0;   // one-shot
  t.next = nowTick + (GameTick)delayTicks;
  t.script = script;
  t.cb = std::move(cb);
  const u64 id = t.id;
  timers_.push_back(std::move(t));
  return id;
}

bool TimerScheduler::Cancel(u64 id) {
  for (auto it = timers_.begin(); it != timers_.end(); ++it) {
    if (it->id == id) {
      timers_.erase(it);
      return true;
    }
  }
  return false;
}

size_t TimerScheduler::CancelScript(const std::string& script) {
  const size_t before = timers_.size();
  timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                               [&](const Timer& t) { return t.script == script; }),
                timers_.end());
  return before - timers_.size();
}

size_t TimerScheduler::Advance(GameTick currentTick) {
  size_t fired = 0;
  // Iterate by index and re-check the timer still exists: a callback may cancel
  // itself or another timer, which invalidates any iterator we hold.
  for (size_t i = 0; i < timers_.size();) {
    if (timers_[i].next > currentTick) {
      ++i;
      continue;
    }
    const u64 id = timers_[i].id;
    const bool repeating = timers_[i].interval > 0;
    const u64 interval = timers_[i].interval;

    if (!repeating) {
      // One-shot: remove before firing so a re-entrant Advance cannot see it.
      Callback cb = timers_[i].cb;
      timers_.erase(timers_.begin() + (long)i);
      cb(id);
      ++fired;
      continue;   // the erase already advanced the index
    }

    // Repeating: how many whole intervals have elapsed, capped so a long stall
    // cannot burst thousands of callbacks in one Advance. The deadline is then
    // set to the ideal next tick (a multiple of the interval from the original
    // schedule), so the timer never drifts.
    u64 missed = (u64)((currentTick - timers_[i].next) / (GameTick)interval) + 1;
    if (missed > kMaxCatchUp) missed = kMaxCatchUp;
    GameTick next = timers_[i].next + (GameTick)(missed * interval);
    timers_[i].next = next;

    for (u64 k = 0; k < missed; ++k) {
      // Re-check the timer still exists: a callback may have cancelled it.
      size_t idx = timers_.size();
      for (size_t j = i; j < timers_.size(); ++j) {
        if (timers_[j].id == id) { idx = j; break; }
      }
      if (idx == timers_.size()) break;   // cancelled; stop
      Callback cb = timers_[idx].cb;
      cb(id);
      ++fired;
    }
    ++i;
  }
  return fired;
}

size_t TimerScheduler::Count() const { return timers_.size(); }
void TimerScheduler::Clear() { timers_.clear(); }

}  // namespace Berry
