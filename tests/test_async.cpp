// Host tests for the coroutine-native async layer (Phase 4).
//
// Covers:
//   * await wait(ticks): suspends, then resumes on the script thread.
//   * await Events.once(channel): resumes with the event payload.
//   * Deterministic resume order (ascending task id).
//   * A task suspended during a poll pass is not collected until the next pass.
//   * Tick and wall-clock timeouts, including faulting the host.
//   * Cancellation (single task and per script).
//   * HTTP await binding is capability-gated (denied without network).
#include "berry/async.h"
#include "berry/event.h"
#include "berry/module.h"
#include "berry/script.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static CapabilitySet Grant(const std::vector<std::string>& caps) {
  CapabilitySet s;
  for (const auto& c : caps) s.Add(c);
  return s;
}

static void TestSchedulerBasics() {
  auto& sched = TaskScheduler::Instance();
  sched.Clear();

  bool ready1 = false;
  u64 t1 = sched.Suspend("a", "wait(1)",
                         [&ready1](ScriptValue* out) {
                           if (ready1 && out) *out = ScriptValue::Num(1);
                           return ready1;
                         },
                         1000);
  CHECK(t1 == 1);
  CHECK(sched.Pending() == 1);
  CHECK(sched.CountInState(TaskState::Pending) == 1);

  // Not ready yet.
  auto r = sched.Poll(0, 1000);
  CHECK(r.empty());

  // Becomes ready.
  ready1 = true;
  r = sched.Poll(1, 1010);
  CHECK(r.size() == 1);
  CHECK(r[0].id == t1);
  CHECK(r[0].state == TaskState::Ready);
  CHECK(r[0].hasResult && r[0].result.AsNumber() == 1);

  // Complete removes it.
  CHECK(sched.Complete(t1));
  CHECK(sched.Pending() == 0);
  CHECK(!sched.Complete(t1));

  // Find / cancel.
  u64 t2 = sched.Suspend("a", "wait(2)", [](ScriptValue*) { return false; }, 0);
  AsyncTask found;
  CHECK(sched.Find(t2, &found));
  CHECK(found.reason == "wait(2)");
  CHECK(sched.Cancel(t2));
  CHECK(sched.Pending() == 0);
  CHECK(!sched.Cancel(t2));

  sched.Clear();
}

static void TestSchedulerTimeout() {
  auto& sched = TaskScheduler::Instance();
  sched.Clear();

  // Tick timeout.
  u64 t1 = sched.Suspend("a", "wait(10)", [](ScriptValue*) { return false; }, 0,
                         /*timeoutTick=*/5, 0);
  auto r = sched.Poll(4, 0);
  CHECK(r.empty());
  r = sched.Poll(5, 0);
  CHECK(r.size() == 1);
  CHECK(r[0].state == TaskState::Failed);
  CHECK(r[0].error.occurred);
  sched.Complete(t1);

  // Wall-clock timeout.
  u64 t2 = sched.Suspend("a", "http", [](ScriptValue*) { return false; }, 1000,
                         0, /*timeoutMs=*/250);
  CHECK(sched.Poll(0, 1200).empty());
  r = sched.Poll(0, 1250);
  CHECK(r.size() == 1);
  CHECK(r[0].state == TaskState::Failed);
  sched.Complete(t2);

  sched.Clear();
}

static void TestDeterministicOrder() {
  auto& sched = TaskScheduler::Instance();
  sched.Clear();

  // Three always-ready tasks must be reported in ascending id order.
  u64 a = sched.Suspend("a", "x", [](ScriptValue* o) { *o = ScriptValue::Num(1); return true; }, 0);
  u64 b = sched.Suspend("b", "y", [](ScriptValue* o) { *o = ScriptValue::Num(2); return true; }, 0);
  u64 c = sched.Suspend("c", "z", [](ScriptValue* o) { *o = ScriptValue::Num(3); return true; }, 0);
  auto r = sched.Poll(0, 0);
  CHECK(r.size() == 3);
  CHECK(r[0].id == a && r[1].id == b && r[2].id == c);
  CHECK(r[0].result.AsNumber() == 1);
  CHECK(r[2].result.AsNumber() == 3);
  sched.Clear();
}

static void TestPassBoundary() {
  auto& sched = TaskScheduler::Instance();
  sched.Clear();

  // A task suspended while a pass is running must not be collected in that
  // same pass; it is only eligible on the next one. This is what keeps a
  // given tick's resume set stable.
  u64 spawned = 0;
  sched.Suspend("a", "spawner",
                [&spawned](ScriptValue* o) {
                  spawned = TaskScheduler::Instance().Suspend(
                      "a", "child", [](ScriptValue*) { return true; }, 0);
                  *o = ScriptValue::Nil();
                  return true;
                },
                0);
  auto r = sched.Poll(0, 0);
  CHECK(r.size() == 1);            // only the spawner
  CHECK(spawned != 0);
  // Next pass collects the child.
  r = sched.Poll(0, 0);
  CHECK(r.size() == 1);
  CHECK(r[0].id == spawned);
  sched.Clear();
}

static void TestCancelScript() {
  auto& sched = TaskScheduler::Instance();
  sched.Clear();
  sched.Suspend("a", "1", [](ScriptValue*) { return false; }, 0);
  sched.Suspend("b", "2", [](ScriptValue*) { return false; }, 0);
  sched.Suspend("a", "3", [](ScriptValue*) { return false; }, 0);
  CHECK(sched.Pending() == 3);
  CHECK(sched.CancelScript("a") == 2);
  CHECK(sched.Pending() == 1);
  CHECK(sched.CancelScript("nobody") == 0);
  sched.Clear();
}

static void TestAwaitWaitResumes() {
  TaskScheduler::Instance().Clear();
  GameTick tick = 0;
  u64 now = 5000;
  AsyncRuntime rt([&tick]() { return tick; }, [&now]() { return now; });

  ScriptBindings bindings;
  rt.InstallBindings(&bindings);

  auto vm = std::unique_ptr<IScriptVm>(CreateReferenceVm());
  ScriptHost host("waiter", Grant({}), std::move(vm));
  host.SetBindings(&bindings);

  ScriptError err;
  const char* src =
      "func on_tick\n"
      "  push 5\n"
      "  native async.wait 1\n"
      "  pop\n"
      "  push 1\n"
      "  store resumed\n"
      "  ret 0\n"
      "end\n";
  CHECK(host.Load(src, "wait", &err));

  // The hook suspends at the await.
  CHECK(host.RunHook("on_tick") == ScriptStatus::Yielded);
  CHECK(host.suspended());
  CHECK(host.loaded());                     // a yield is not a fault
  CHECK(TaskScheduler::Instance().Pending() == 1);

  // Before the deadline: no resume.
  CHECK(rt.Pump(host, 0) == 0);
  CHECK(host.suspended());
  CHECK(rt.Pump(host, 4) == 0);

  // At the deadline: resume and finish.
  tick = 5;
  CHECK(rt.Pump(host, 5) == 1);
  CHECK(!host.suspended());
  CHECK(host.loaded());
  CHECK(TaskScheduler::Instance().Pending() == 0);

  // The script ran past the await.
  std::vector<ScriptValue> ret;
  CHECK(host.RunHook("on_tick", {}, &ret) == ScriptStatus::Yielded);   // awaits again
  TaskScheduler::Instance().Clear();
}

static void TestAwaitEventResumes() {
  TaskScheduler::Instance().Clear();
  GameTick tick = 1;
  u64 now = 1;
  AsyncRuntime rt([&tick]() { return tick; }, [&now]() { return now; });

  ScriptBindings bindings;
  rt.InstallBindings(&bindings);

  auto vm = std::unique_ptr<IScriptVm>(CreateReferenceVm());
  ScriptHost host("listener", Grant({"events"}), std::move(vm));
  host.SetBindings(&bindings);

  ScriptError err;
  const char* src =
      "func on_event\n"
      "  push \"hit\"\n"
      "  native async.events_once 1\n"
      "  store payload\n"
      "  ret 0\n"
      "end\n"
      "func got\n"
      "  load payload\n"
      "  ret 1\n"
      "end\n";
  CHECK(host.Load(src, "evt", &err));
  CHECK(host.RunHook("on_event") == ScriptStatus::Yielded);
  CHECK(host.suspended());
  CHECK(TaskScheduler::Instance().Pending() == 1);

  // No event yet: no resume.
  CHECK(rt.Pump(host, tick) == 0);

  // Fire the channel; the next pump resumes with the payload.
  const char* msg = "kaboom";
  EventBus::Instance().Emit("hit", (void*)&msg);
  CHECK(rt.Pump(host, tick) == 1);
  CHECK(!host.suspended());
  // The awaited payload was delivered as the binding's return value.
  std::vector<ScriptValue> ret;
  CHECK(host.RunHook("got", {}, &ret) == ScriptStatus::Ok);
  CHECK(ret.size() == 1 && ret[0].IsString() && ret[0].string == "kaboom");
  // The listener unregistered itself (one-shot).
  CHECK(!EventBus::Instance().HasListeners("hit"));
  TaskScheduler::Instance().Clear();
}

static void TestAwaitTimeoutFaultsHost() {
  TaskScheduler::Instance().Clear();
  GameTick tick = 0;
  u64 now = 0;
  AsyncRuntime rt([&tick]() { return tick; }, [&now]() { return now; });

  ScriptBindings bindings;
  rt.InstallBindings(&bindings);

  auto vm = std::unique_ptr<IScriptVm>(CreateReferenceVm());
  ScriptHost host("timeout", Grant({}), std::move(vm));
  host.SetBindings(&bindings);
  CHECK(host.Load("func on_tick\n  push 3\n  native async.wait 1\n  ret 0\nend\n",
                  "t", nullptr));

  CHECK(host.RunHook("on_tick") == ScriptStatus::Yielded);
  CHECK(TaskScheduler::Instance().Pending() == 1);
  // The wait completes at tick 3, so pump at tick 3: the task fires and is
  // resumed normally (the timeout path is covered directly in
  // TestSchedulerTimeout, since a wait predicate is driven by the tick clock).
  tick = 3;
  CHECK(rt.Pump(host, tick) == 1);
  CHECK(host.loaded());
  CHECK(!host.suspended());
  CHECK(host.Faults() == 0);

  // Now prove a stuck await faults the script instead of pinning it: suspend
  // the host again, then replace its task with one that never completes and
  // has a tick deadline.
  CHECK(host.RunHook("on_tick") == ScriptStatus::Yielded);
  TaskScheduler::Instance().CancelScript("timeout");
  TaskScheduler::Instance().Suspend("timeout", "stuck",
                                    [](ScriptValue*) { return false; }, now,
                                    /*timeoutTick=*/10, 0);
  rt.Pump(host, 10);
  CHECK(!host.loaded());
  CHECK(!host.suspended());
  CHECK(host.Faults() >= 1);
  CHECK(TaskScheduler::Instance().Pending() == 0);
  TaskScheduler::Instance().Clear();
}

static void TestHttpBindingGated() {
  TaskScheduler::Instance().Clear();
  AsyncRuntime rt([]() { return (GameTick)0; }, []() { return (u64)0; });
  ScriptBindings bindings;
  rt.InstallBindings(&bindings);

  // The async bindings are registered under their canonical names.
  CHECK(bindings.Has("async.wait"));
  CHECK(bindings.Has("async.events_once"));
  CHECK(bindings.Has("async.http_get"));
  CHECK(bindings.Required("async.events_once").kind == CapKind::Events);
  CHECK(bindings.Required("async.http_get").kind == CapKind::Network);
  CHECK(bindings.Required("async.wait").kind == CapKind::Unknown);

  // A script without the network grant cannot use Http.get.
  ScriptContext ctx("no-net", Grant({"events"}));
  bool ok = true, denied = false;
  bindings.Invoke(ctx, "async.http_get", {ScriptValue::Str("http://example.com")},
                  &ok, &denied);
  CHECK(!ok && denied);
}

static void TestWaitWithoutClock() {
  // With no tick source the wait completes immediately on the first pump,
  // which is what a headless/replay context wants.
  TaskScheduler::Instance().Clear();
  AsyncRuntime rt(nullptr, nullptr);
  ScriptBindings bindings;
  rt.InstallBindings(&bindings);

  auto vm = std::unique_ptr<IScriptVm>(CreateReferenceVm());
  ScriptHost host("noclk", Grant({}), std::move(vm));
  host.SetBindings(&bindings);
  CHECK(host.Load("func on_tick\n  push 1\n  native async.wait 1\n  ret 0\nend\n",
                  "n", nullptr));
  CHECK(host.RunHook("on_tick") == ScriptStatus::Yielded);
  CHECK(rt.Pump(host, 0) == 1);
  CHECK(host.loaded());
  CHECK(!host.suspended());
  TaskScheduler::Instance().Clear();
}

int main() {
  TestSchedulerBasics();
  TestSchedulerTimeout();
  TestDeterministicOrder();
  TestPassBoundary();
  TestCancelScript();
  TestAwaitWaitResumes();
  TestAwaitEventResumes();
  TestAwaitTimeoutFaultsHost();
  TestHttpBindingGated();
  TestWaitWithoutClock();
  if (g_fail == 0) printf("test_async: ALL OK\n");
  return g_fail;
}
