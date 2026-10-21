// Coroutine-native async layer. See include/berry/async.h.
//
// Design: an `await` suspends the running script instead of blocking a thread.
// The binding that implements the await registers a completion predicate with
// the TaskScheduler and returns; the VM stops at that boundary with status
// Yielded. The host then polls the scheduler on the script worker and resumes
// the VM (IScriptVm::Resume) with the awaited value once a predicate fires.
// Every resume therefore runs on the script thread and in ascending task-id
// order, which is what makes script async deterministic and replay-safe.
#include "berry/async.h"
#include "berry/logger.h"
#include "berry/module.h"
#include "berry/net.h"

#include <algorithm>
#include <cstdlib>

namespace Berry {

//----------------------------------------------------------------------------
// TaskState
//----------------------------------------------------------------------------

const char* TaskStateName(TaskState s) {
  switch (s) {
    case TaskState::Pending: return "pending";
    case TaskState::Ready: return "ready";
    case TaskState::Resumed: return "resumed";
    case TaskState::Cancelled: return "cancelled";
    case TaskState::Failed: return "failed";
  }
  return "?";
}

//----------------------------------------------------------------------------
// TaskScheduler
//----------------------------------------------------------------------------

TaskScheduler& TaskScheduler::Instance() {
  static TaskScheduler* inst = new TaskScheduler();
  return *inst;
}

u64 TaskScheduler::Suspend(const std::string& script, const std::string& reason,
                           std::function<bool(ScriptValue*)> ready, u64 nowMs,
                           GameTick timeoutTick, u64 timeoutMs) {
  std::lock_guard<std::mutex> lk(mutex_);
  AsyncTask t;
  t.id = nextId_++;
  t.script = script;
  t.reason = reason;
  t.ready = std::move(ready);
  t.state = TaskState::Pending;
  t.timeoutTick = timeoutTick;
  t.timeoutMs = timeoutMs;
  t.createdAtMs = nowMs;
  tasks_.push_back(std::move(t));
  return tasks_.back().id;
}

std::vector<AsyncTask> TaskScheduler::Poll(GameTick currentTick, u64 nowMs) {
  // Snapshot the pending set under the lock, then evaluate predicates WITHOUT
  // it: a predicate may call Suspend()/Cancel() (which take the same lock) or
  // otherwise re-enter the scheduler. Holding the lock across a predicate
  // would deadlock and would let a registration invalidate our iteration.
  struct Candidate {
    u64 id;
    std::string script;
    std::string reason;
    std::function<bool(ScriptValue*)> ready;
    GameTick timeoutTick;
    u64 timeoutMs;
    u64 createdAtMs;
  };
  std::vector<Candidate> candidates;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    candidates.reserve(tasks_.size());
    for (const auto& t : tasks_) {
      if (t.state != TaskState::Pending) continue;
      candidates.push_back({t.id, t.script, t.reason, t.ready, t.timeoutTick,
                            t.timeoutMs, t.createdAtMs});
    }
  }

  // Evaluate predicates and deadlines WITHOUT the lock, recording outcomes
  // keyed by id. Candidates run in ascending id order, so resume order is
  // deterministic. A task suspended by a predicate is not a candidate (it was
  // not in the snapshot) and is therefore only eligible on the next pass.
  struct Outcome {
    u64 id;
    bool timedOut;
    bool fired;
    ScriptValue value;
  };
  std::vector<Outcome> outcomes;
  outcomes.reserve(candidates.size());
  for (const auto& c : candidates) {
    const bool tickExpired = c.timeoutTick != 0 && currentTick >= c.timeoutTick;
    const bool msExpired =
        c.timeoutMs != 0 && nowMs >= c.createdAtMs + c.timeoutMs;
    if (tickExpired || msExpired) {
      outcomes.push_back({c.id, true, false, ScriptValue::Nil()});
      continue;
    }
    ScriptValue out;
    const bool fired = c.ready && c.ready(&out);
    outcomes.push_back({c.id, false, fired, out});
  }

  // Fold the outcomes back in under the lock and build the ready list.
  std::vector<AsyncTask> ready;
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& o : outcomes) {
    AsyncTask* t = nullptr;
    for (auto& candidate : tasks_) {
      if (candidate.id == o.id) { t = &candidate; break; }
    }
    if (!t || t->state != TaskState::Pending) continue;
    if (o.timedOut) {
      t->state = TaskState::Failed;
      t->error = ScriptError::Make("async task timed out: " + t->reason,
                                   t->script, 0);
      ready.push_back(*t);
    } else if (o.fired) {
      t->state = TaskState::Ready;
      t->result = o.value;
      t->hasResult = true;
      ready.push_back(*t);
    }
  }
  return ready;
}

bool TaskScheduler::Complete(u64 id) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto it = tasks_.begin(); it != tasks_.end(); ++it) {
    if (it->id == id) {
      it->state = TaskState::Resumed;
      tasks_.erase(it);
      return true;
    }
  }
  return false;
}

bool TaskScheduler::Cancel(u64 id) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto it = tasks_.begin(); it != tasks_.end(); ++it) {
    if (it->id == id) {
      it->state = TaskState::Cancelled;
      tasks_.erase(it);
      return true;
    }
  }
  return false;
}

size_t TaskScheduler::CancelScript(const std::string& script) {
  std::lock_guard<std::mutex> lk(mutex_);
  size_t n = 0;
  for (auto it = tasks_.begin(); it != tasks_.end();) {
    if (it->script == script) {
      it = tasks_.erase(it);
      ++n;
    } else {
      ++it;
    }
  }
  return n;
}

bool TaskScheduler::Find(u64 id, AsyncTask* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& t : tasks_) {
    if (t.id == id) {
      if (out) *out = t;
      return true;
    }
  }
  return false;
}

size_t TaskScheduler::Pending() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return tasks_.size();
}

size_t TaskScheduler::CountInState(TaskState state) const {
  std::lock_guard<std::mutex> lk(mutex_);
  size_t n = 0;
  for (const auto& t : tasks_) {
    if (t.state == state) ++n;
  }
  return n;
}

void TaskScheduler::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  tasks_.clear();
  nextId_ = 1;
}

//----------------------------------------------------------------------------
// AsyncRuntime
//----------------------------------------------------------------------------

AsyncRuntime::AsyncRuntime(std::function<GameTick()> currentTick,
                           std::function<u64()> nowMs)
    : currentTick_(std::move(currentTick)), nowMs_(std::move(nowMs)) {}

bool AsyncRuntime::Await(ScriptContext& ctx, const std::string& reason,
                         std::function<bool(ScriptValue*)> ready,
                         GameTick timeoutTicks, u64 timeoutMs) {
  const u64 now = nowMs_ ? nowMs_() : NowMs();
  GameTick deadlineTick = 0;
  if (timeoutTicks != 0 && currentTick_) {
    deadlineTick = currentTick_() + timeoutTicks;
  }
  u64 id = scheduler().Suspend(ctx.scriptName(), reason, std::move(ready), now,
                               deadlineTick, timeoutMs);
  {
    std::lock_guard<std::mutex> lk(mutex_);
    pendingTaskId_ = id;
    pendingScript_ = ctx.scriptName();
  }
  // Ask the VM to suspend at this binding boundary. The host resumes it with
  // the awaited value once the scheduler reports the task ready.
  ctx.RequestYield(reason);
  return true;
}

std::string AsyncRuntime::suspendedScript() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return pendingScript_;
}

void AsyncRuntime::InstallBindings(ScriptBindings* bindings) const {
  if (!bindings) return;
  // The bindings close over `this`, so the runtime must outlive the bindings.
  AsyncRuntime* self = const_cast<AsyncRuntime*>(this);

  Capability none;   // async.wait needs no capability: pure scheduling
  bindings->Register(AsyncBindings::Wait(), none,
                     [self](ScriptContext& ctx,
                            const std::vector<ScriptValue>& args) {
                       const GameTick ticks =
                           args.empty() ? 0 : (GameTick)args[0].AsNumber();
                       // Capture the absolute deadline at await time so a long
                       // poll gap cannot shorten the wait.
                       auto target = std::make_shared<GameTick>(
                           self->currentTick_ ? self->currentTick_() + ticks : 0);
                       self->Await(
                           ctx, "wait(" + std::to_string((long long)ticks) + ")",
                           [target, self](ScriptValue* out) {
                             if (!self->currentTick_) {
                               if (out) *out = ScriptValue::Nil();
                               return true;   // no clock: complete immediately
                             }
                             if (self->currentTick_() >= *target) {
                               if (out) *out = ScriptValue::Nil();
                               return true;
                             }
                             return false;
                           });
                       return ScriptValue::Nil();
                     });

  Capability eventsCap;
  Capability::Parse("events", &eventsCap);
  bindings->Register(AsyncBindings::EventsOnce(), eventsCap,
                     [self](ScriptContext& ctx,
                            const std::vector<ScriptValue>& args) {
                       const std::string channel =
                           args.empty() ? std::string() : args[0].ToDisplay();
                       auto fired = std::make_shared<std::atomic<bool>>(false);
                       auto payload = std::make_shared<std::string>();
                       // The listener owns a copy of its id so it can remove
                       // itself exactly once, whichever path fires first.
                       auto id = std::make_shared<EventBus::ListenerId>(0);
                       EventBus::ListenerId reg = EventBus::Instance().On(
                           channel.c_str(),
                           [fired, payload](const EventContext& e) {
                             if (e.data) *payload = *(const char**)e.data;
                             fired->store(true);
                             return EventResult::Continue;
                           });
                       *id = reg;
                       self->Await(ctx, "events.once(" + channel + ")",
                                   [fired, payload, channel, id](ScriptValue* out) {
                                     if (!fired->load()) return false;
                                     EventBus::Instance().Off(channel.c_str(), *id);
                                     if (out) *out = ScriptValue::Str(*payload);
                                     return true;
                                   });
                       return ScriptValue::Nil();
                     });

  Capability netCap;
  Capability::Parse("network", &netCap);
  bindings->Register(AsyncBindings::HttpGet(), netCap,
                     [self](ScriptContext& ctx,
                            const std::vector<ScriptValue>& args) {
                       const std::string url =
                           args.empty() ? std::string() : args[0].ToDisplay();
                       const long timeout =
                           args.size() >= 2 ? (long)args[1].AsNumber() : 8000;
                       // Blocking HTTP on the script worker is intended: this
                       // thread exists for exactly this and is never the
                       // render thread.
                       ScriptValue body = HttpGetBlocking(ctx, url, timeout);
                       (void)self;
                       return body;
                     });
}

size_t AsyncRuntime::Pump(ScriptHost& host, GameTick currentTick,
                          const std::function<u64()>& nowMs) {
  if (!host.loaded()) return 0;
  const u64 now = nowMs ? nowMs() : NowMs();
  std::vector<AsyncTask> ready = scheduler().Poll(currentTick, now);
  size_t resumed = 0;
  for (const auto& task : ready) {
    if (task.state == TaskState::Failed) {
      // A timeout faults the script exactly like a hook error, so a stuck
      // await cannot pin a script forever.
      host.Fault(task.error);
      scheduler().Complete(task.id);
      continue;
    }
    if (!host.suspended()) {
      // The script is no longer waiting (unloaded or reloaded): drop the task.
      scheduler().Complete(task.id);
      continue;
    }
    ScriptStatus st = host.ResumeFromYield(task.result);
    scheduler().Complete(task.id);
    ++resumed;
    if (st == ScriptStatus::Ok || st == ScriptStatus::Yielded) continue;
    // A fault during resume unloads the host; cancel any siblings.
    scheduler().CancelScript(task.script);
    break;
  }
  return resumed;
}

//----------------------------------------------------------------------------
// HttpGetBlocking
//----------------------------------------------------------------------------

ScriptValue HttpGetBlocking(ScriptContext& ctx, const std::string& url,
                            long timeoutMs) {
  HttpRequest req(url.c_str());
  req.Method("GET").Timeout((int)timeoutMs);
  HttpResponse resp = req.Do();
  if (resp.status == 0) {
    ctx.RaiseError("http request failed: " + url);
    return ScriptValue::Nil();
  }
  if (!resp.ok()) {
    ctx.RaiseError("http " + std::to_string(resp.status) + " for " + url);
    return ScriptValue::Nil();
  }
  return ScriptValue::Str(resp.body);
}

}  // namespace Berry
