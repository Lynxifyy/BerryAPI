#pragma once
#include "berry/export.h"
#include "berry/types.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {

/// Monotonic nanoseconds (CLOCK_MONOTONIC-backed; not wall-clock).
inline u64 NowNs() {
  return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline u64 NowMs() { return NowNs() / 1000000ULL; }

//----------------------------------------------------------------------------
// Event bus — thread-safe, ordered, cancellable.
//
// Modules attach listeners to named channels. A listener may cancel the event
// (prevents later listeners and stops propagation), or stop propagation only.
// Listener order is stable (registration order).
//----------------------------------------------------------------------------

enum class EventResult { Continue, StopPropagation, Cancel };

struct BERRY_API EventContext {
  /// Unique event id; listeners can cancel / modify it.
  u64 id = 0;
  /// Optional concrete payload (module-specific).
  void* data = nullptr;
  /// Execution watchdog: set to true when handling is being aborted.
  bool cancelled = false;
};

using EventListener = std::function<EventResult(const EventContext&)>;

class BERRY_API EventBus {
 public:
  static EventBus& Instance();

  using ListenerId = u64;

  /// Registers `fn` on channel `name`, returns a handle for removal.
  ListenerId On(const char* channel, EventListener fn);
  /// Removes a listener by handle.
  bool Off(const char* channel, ListenerId id);
  /// Removes all listeners on a channel.
  void Clear(const char* channel);

  /// Publishes the event. Returns true unless a listener cancelled it.
  bool Emit(const char* channel, void* data, EventContext* out = nullptr);

  /// True if any listener is registered on a channel.
  bool HasListeners(const char* channel) const;

  size_t ListenerCount() const;

 private:
  struct Entry {
    ListenerId id;
    EventListener fn;
  };
  std::unordered_map<std::string, std::vector<Entry>> channels_;
  mutable std::mutex mutex_;
  ListenerId nextId_ = 1;
};

//----------------------------------------------------------------------------
// Scheduler — deferral & tick-driven execution.
//----------------------------------------------------------------------------

class BERRY_API Scheduler {
 public:
  struct Delayed {
    GameTick runAt;
    u64 id;
  };

  /// Sets the engine's current absolute tick (called by Engine each tick).
  void SetCurrentTick(GameTick tick);
  /// Runs `fn` after `delayTicks` game ticks from now.
  void After(GameTick delayTicks, std::function<void()> fn, const char* tag = nullptr);
  /// Runs `fn` every `intervalTicks` ticks (first run after `intervalTicks`).
  u64 Every(GameTick intervalTicks, std::function<void(GameTick)> fn);
  bool Cancel(u64 handle);
  /// Advances the scheduler; runs due tasks.
  void Tick(GameTick currentTick);

  size_t Pending() const;

 private:
  struct Task {
    u64 id;
    GameTick runAt;
    GameTick interval;
    bool repeat = false;
    bool cancelled = false;
    std::function<void(GameTick)> fn;
  };
  std::vector<Task> tasks_;
  mutable std::mutex mutex_;
  u64 nextId_ = 1;
  GameTick currentTick_ = 0;
};

//----------------------------------------------------------------------------
// Profiler — frame timing histograms + span scopes.
//----------------------------------------------------------------------------

class BERRY_API Profiler {
 public:
  struct SpanStats {
    f64 totalMs = 0;
    u64 count = 0;
    f64 maxMs = 0;
  };

  /// One node of the aggregated call tree. `selfMs` excludes children;
  /// `totalMs` is `selfMs` plus every descendant, so a parent's total is the
  /// time actually spent inside it.
  struct FlameNode {
    std::string name;
    f64 selfMs = 0;
    f64 totalMs = 0;
    u64 count = 0;
    std::vector<FlameNode> children;
  };

  static Profiler& Instance();

  /// Start a named span; returns a token to end it. Spans nest: a span opened
  /// while another is open on the same thread becomes its child, so the tree
  /// reflects the real call structure.
  u64 Begin(const char* name);
  /// Ends a span; records milliseconds and pushes to the ring for rendering.
  void End(u64 token);
  /// Returns cumulative stats for a name (for the UI / diagnostics).
  SpanStats Stats(const char* name) const;
  /// Current frame histogram (per-frame ms per named span). Updated on End.
  void FrameClear();
  const std::unordered_map<std::string, f64>& LastFrame() const { return lastFrame_; }

  /// The aggregated call tree since the last FlameReset. Children are sorted by
  /// total time descending, so the expensive branch reads first.
  FlameNode Flame() const;
  /// Every named span's cumulative stats, highest total first. The flat view of
  /// the same data, for a list-style display.
  std::vector<std::pair<std::string, SpanStats>> Spans() const;
  /// Drops the flame tree, the span stats and the last-frame ring, starting the
  /// profiler over.
  void FlameReset();

  void SetEnabled(bool e) { enabled_ = e; }
  bool enabled() const { return enabled_; }

 private:
  Profiler() = default;
  struct Span { std::string name; u64 startNs; void* node; };
  /// Aggregation node with stable storage: children live behind unique_ptr so a
  /// pointer taken while a span is open stays valid as siblings are added.
  struct Agg {
    std::string name;
    f64 selfMs = 0;
    u64 count = 0;
    std::vector<std::unique_ptr<Agg>> children;
  };

  /// The per-thread open-span stack, so Begin knows a span's parent.
  static std::vector<Agg*>& TlsStack();
  static Agg* ChildFor(Agg* parent, const std::string& name);
  static void CopyFlame(const Agg& src, FlameNode* out);

  std::unordered_map<u64, Span> active_;
  std::unordered_map<std::string, SpanStats> stats_;
  std::unordered_map<std::string, f64> lastFrame_;
  Agg root_;
  mutable std::mutex mutex_;
  u64 nextId_ = 1;
  bool enabled_ = true;
};

// RAII profiler scope.
class BERRY_API ProfilerScope {
 public:
  explicit ProfilerScope(const char* name);
  ~ProfilerScope();
 private:
  u64 token_ = 0;
};

}  // namespace Berry