#pragma once
// A drift-corrected interval scheduler (improvement 70).
//
// A timer fires every `intervalTicks` game ticks. A naive scheduler adds the
// interval to the last fire, so a late tick (a stall) makes every subsequent
// fire late by the same amount and the drift accumulates. This one tracks the
// ideal next fire time and advances it by whole intervals until it is in the
// future, so a skipped interval is caught up rather than compounding.
//
// It owns no clock: `Advance(currentTick)` is the whole input, and the caller
// (the ScriptApi frame/tick hook) drives it. That keeps it deterministic and
// testable.

#include "berry/export.h"
#include "berry/types.h"

#include <functional>
#include <string>
#include <vector>

namespace Berry {

class BERRY_API TimerScheduler {
 public:
  using Callback = std::function<void(u64 timerId)>;

  /// Registers a repeating timer firing every `intervalTicks`, starting one
  /// interval from `nowTick`. Returns a nonzero id, or 0 when refused (a
  /// non-positive interval, or the cap reached).
  u64 Every(u64 intervalTicks, const std::string& script, Callback cb,
            GameTick nowTick);
  /// A one-shot timer firing once after `delayTicks`.
  u64 After(u64 delayTicks, const std::string& script, Callback cb,
            GameTick nowTick);
  /// Cancels a timer. Returns true when it existed.
  bool Cancel(u64 id);
  /// Cancels every timer owned by `script`. Returns how many were removed.
  size_t CancelScript(const std::string& script);

  /// Advances to `currentTick`, firing every timer whose deadline has passed
  /// (a repeating timer may fire more than once in one Advance when the caller
  /// was stalled, catching up rather than drifting). Returns how many callbacks
  /// ran. A callback that cancels its own timer stops its repetition.
  size_t Advance(GameTick currentTick);

  size_t Count() const;
  void Clear();

  static constexpr size_t kMaxTimers = 256;
  /// The most catch-up fires one Advance may deliver for a single repeating
  /// timer, so a very long stall cannot burst an unbounded number of callbacks.
  static constexpr u64 kMaxCatchUp = 16;

 private:
  struct Timer {
    u64 id = 0;
    u64 interval = 0;    // 0 = one-shot
    GameTick next = 0;   // ideal next fire tick
    std::string script;
    Callback cb;
  };
  std::vector<Timer> timers_;
  u64 nextId_ = 1;
};

}  // namespace Berry
