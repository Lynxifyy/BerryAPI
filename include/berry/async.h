#pragma once
// BerryAPI coroutine-native async layer (Phase 4).
//
// Scripts express async work with `await`:
//
//   await wait(ticks(20))        -- resume after 20 game ticks
//   await Events.once("hit")     -- resume when a channel fires
//   await Http.get(url)          -- resume when a request completes
//
// The runtime models each `await` as a SUSPENSION of the running script rather
// than a blocking call. A suspension registers a completion predicate with the
// TaskScheduler; the scheduler polls predicates on the script worker and
// resumes the VM with the awaited value when a predicate is satisfied. Every
// resume therefore happens on the script thread, in a well-defined order, and
// never inside the render thread or the game tick.
//
// Ordering is deterministic: predicates are polled in ascending id order, and
// a predicate that fires mid-poll does not affect the current pass (the task
// is resumed on the next pass). This keeps replay and tests reproducible.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/event.h"
#include "berry/script.h"
#include "berry/url.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Berry {

class HttpRequest;
struct HttpResponse;

//----------------------------------------------------------------------------
// Tick/time helpers used by script timeouts.
//----------------------------------------------------------------------------

namespace async_time {
/// A duration measured in game ticks (20/s default).
struct BERRY_API Ticks {
  GameTick count = 0;
  explicit Ticks(GameTick n) : count(n) {}
};
inline Ticks ticks(GameTick n) { return Ticks(n); }

/// A duration measured in milliseconds (wall clock).
struct BERRY_API Millis {
  u64 ms = 0;
  explicit Millis(u64 n) : ms(n) {}
};
inline Millis millis(u64 n) { return Millis(n); }
}  // namespace async_time

//----------------------------------------------------------------------------
// Task + scheduler.
//----------------------------------------------------------------------------

enum class TaskState : u32 {
  Pending = 0,   // waiting on its predicate
  Ready,         // predicate fired; awaiting resume
  Resumed,       // the VM was resumed
  Cancelled,
  Failed,
};

const char* TaskStateName(TaskState s);

/// One suspended script execution. The predicate is polled on the script
/// worker; `timeoutTick`/`timeoutMs` bound how long the task may wait (0 = no
/// timeout).
struct BERRY_API AsyncTask {
  u64 id = 0;
  std::string script;
  std::string reason;                 // "wait(20)", "events.once(hit)", …
  std::function<bool(ScriptValue* out)> ready;  // returns true when complete
  ScriptValue result;
  bool hasResult = false;
  TaskState state = TaskState::Pending;
  GameTick timeoutTick = 0;           // 0 = none
  u64 timeoutMs = 0;                  // 0 = none
  u64 createdAtMs = 0;
  ScriptError error;                  // populated on Failed
};

/// Owns the pending-task table. Thread-safe. The scheduler never resumes a VM
/// itself; it reports which tasks are Ready and the host performs the resume,
/// so VM access stays single-threaded.
class BERRY_API TaskScheduler {
 public:
  static TaskScheduler& Instance();

  /// Registers a suspended task; returns its id. `nowMs` is injected so tests
  /// are deterministic.
  u64 Suspend(const std::string& script, const std::string& reason,
              std::function<bool(ScriptValue*)> ready, u64 nowMs,
              GameTick timeoutTick = 0, u64 timeoutMs = 0);

  /// Polls every pending task in ascending id order. Tasks whose predicate
  /// fires move to Ready (or Failed on timeout) and are returned. A task that
  /// becomes ready during this pass is NOT returned until the next pass.
  std::vector<AsyncTask> Poll(GameTick currentTick, u64 nowMs);

  /// Marks a ready task as resumed and removes it from the table.
  bool Complete(u64 id);

  /// Cancels a task (the script was unloaded / detached).
  bool Cancel(u64 id);
  /// Cancels every task for a script. Returns how many were cancelled.
  size_t CancelScript(const std::string& script);

  bool Find(u64 id, AsyncTask* out) const;
  size_t Pending() const;
  /// Count of tasks in a given state (diagnostics).
  size_t CountInState(TaskState state) const;
  void Clear();

 private:
  TaskScheduler() = default;
  mutable std::mutex mutex_;
  std::vector<AsyncTask> tasks_;
  u64 nextId_ = 1;
};

//----------------------------------------------------------------------------
// Script-side async facade (the object `async` / `Events` / `Http` resolve to).
//----------------------------------------------------------------------------

/// Native binding names the async layer installs. Kept as constants so the IDL
/// and docs stay in sync with the implementation.
namespace AsyncBindings {
inline const char* Wait() { return "async.wait"; }
inline const char* EventsOnce() { return "async.events_once"; }
inline const char* HttpGet() { return "async.http_get"; }
inline const char* All() { return "async.all"; }
}  // namespace AsyncBindings

/// Bridges awaited operations to the scheduler. One per ScriptHost; installed
/// as bindings so a script can call them directly, and used by the host's
/// resume loop.
class BERRY_API AsyncRuntime {
 public:
  /// `currentTick` supplies the game tick used for tick-based waits.
  AsyncRuntime(std::function<GameTick()> currentTick,
               std::function<u64()> nowMs);

  /// Registers the async bindings on `bindings`, each requiring `cap`.
  /// - async.wait:        requires no capability (declared via Capability::NoCap).
  /// - async.events_once: requires kCapEvents.
  /// - async.http_get:    requires a kCapNetwork grant scoped to the URL host.
  ///
  /// `policy` is the egress policy applied to the destination and re-applied
  /// to every redirect hop. The default is the safe policy (https only, no IP
  /// literals, no private ranges); the host harness relaxes it to reach its
  /// loopback server.
  void InstallBindings(ScriptBindings* bindings,
                       const UrlPolicy& policy = UrlPolicy()) const;

  /// Suspends the current binding invocation until `ready` fires, returning
  /// true when the script should be suspended (the caller returns from the
  /// binding). The awaited value is delivered by the host when it resumes the
  /// VM, so the binding runs exactly once per await.
  bool Await(ScriptContext& ctx, const std::string& reason,
             std::function<bool(ScriptValue*)> ready, GameTick timeoutTicks = 0,
             u64 timeoutMs = 0);

  /// The script currently suspended by this runtime (empty when none).
  std::string suspendedScript() const;

  /// Drives the scheduler and resumes the host for each ready task. Called on
  /// the script worker each tick/frame. Returns how many awaits were resumed.
  size_t Pump(ScriptHost& host, GameTick currentTick,
              const std::function<u64()>& nowMs = nullptr);

  TaskScheduler& scheduler() { return TaskScheduler::Instance(); }

 private:
  std::function<GameTick()> currentTick_;
  std::function<u64()> nowMs_;
  UrlPolicy policy_;
  mutable std::mutex mutex_;
  u64 pendingTaskId_ = 0;      // task created by the most recent Await
  std::string pendingScript_;
};

//----------------------------------------------------------------------------
// HTTP await helper (uses the Network module's client on the script thread).
//----------------------------------------------------------------------------

/// Issues a GET and returns the body, or raises a script error on failure.
/// Runs on the script worker, so blocking IO here is expected and allowed.
///
/// Before the first request — and again after every redirect — the destination
/// is checked against `policy` and against the host the script was granted. A
/// redirect to a host outside the grant, or to a private address, fails the
/// call instead of being followed. `timeoutMs` is clamped to a safe range.
BERRY_API ScriptValue HttpGetBlocking(ScriptContext& ctx, const std::string& url,
                                      long timeoutMs = 8000,
                                      const UrlPolicy& policy = UrlPolicy());

}  // namespace Berry
