// Host tests for the scripting runtime (Phase 2).
//
// Covers:
//   * ScriptValue semantics (truthiness, coercion, display, equality).
//   * Reference VM assembly, execution, arithmetic/compare/branch/loop.
//   * Instruction budget enforcement (infinite loop stops).
//   * Memory limit enforcement.
//   * Wall-clock timeout via ScopedScriptTimeslice + host guard.
//   * Capability-gated native bindings (denied vs allowed).
//   * Error isolation: a faulting script unloads; the host stays usable.
//   * State serialize/restore round-trip.
//   * ScriptWorker routing (script thread role, deterministic drain).
#include "berry/script.h"
#include "berry/thread.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
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

static void TestValues() {
  CHECK(ScriptValue::Nil().IsNil());
  CHECK(!ScriptValue::Nil().Truthy());
  CHECK(!ScriptValue::Bool(false).Truthy());
  CHECK(ScriptValue::Bool(true).Truthy());
  CHECK(!ScriptValue::Num(0).Truthy());
  CHECK(ScriptValue::Num(3).Truthy());
  CHECK(!ScriptValue::Str("").Truthy());
  CHECK(ScriptValue::Str("x").Truthy());

  CHECK(ScriptValue::Num(42).ToDisplay() == "42");
  CHECK(ScriptValue::Num(1.5).ToDisplay() == "1.5");
  CHECK(ScriptValue::Bool(true).ToDisplay() == "true");
  CHECK(ScriptValue::Nil().ToDisplay() == "nil");
  CHECK(ScriptValue::Str("hi").ToDisplay() == "hi");

  CHECK(ScriptValue::Num(2) == ScriptValue::Num(2));
  CHECK(ScriptValue::Num(2) != ScriptValue::Num(3));
  CHECK(ScriptValue::Str("a") != ScriptValue::Num(1));
  CHECK(ScriptValue::Num(1) == ScriptValue::Bool(true));   // numeric coercion
  CHECK(ScriptValue::Str("12").AsNumber() == 12.0);
  CHECK(ScriptValue::Str("abc").AsNumber() == 0.0);
}

static void TestVmBasics() {
  std::unique_ptr<IScriptVm> vm(CreateReferenceVm());
  CHECK(vm->Name() != nullptr);
  ScriptError err;
  const char* src =
      "func add a b\n"
      "  load a\n"
      "  load b\n"
      "  add\n"
      "  ret 1\n"
      "end\n"
      "func loop_sum n\n"
      "  push 0\n"
      "  store acc\n"
      "  push 0\n"
      "  store i\n"
      "loop:\n"
      "  load i\n"
      "  load n\n"
      "  lt\n"
      "  jz done\n"
      "  load acc\n"
      "  load i\n"
      "  add\n"
      "  store acc\n"
      "  load i\n"
      "  push 1\n"
      "  add\n"
      "  store i\n"
      "  jmp loop\n"
      "done:\n"
      "  load acc\n"
      "  ret 1\n"
      "end\n";
  CHECK(vm->Load(src, "test", &err));
  CHECK(vm->loaded());
  CHECK(vm->HasFunction("add"));
  CHECK(vm->HasFunction("loop_sum"));
  CHECK(!vm->HasFunction("missing"));

  std::vector<ScriptValue> ret;
  CHECK(vm->Call("add", {ScriptValue::Num(2), ScriptValue::Num(40)}, &ret, &err) ==
        ScriptStatus::Ok);
  CHECK(ret.size() == 1);
  CHECK(ret[0].AsNumber() == 42);

  CHECK(vm->Call("loop_sum", {ScriptValue::Num(10)}, &ret, &err) ==
        ScriptStatus::Ok);
  CHECK(ret[0].AsNumber() == 45);   // 0+1+...+9

  // String concatenation.
  const char* cat =
      "func greet\n"
      "  push \"hi \"\n"
      "  push \"there\"\n"
      "  add\n"
      "  ret 1\n"
      "end\n";
  CHECK(vm->Load(cat, "cat", &err));
  CHECK(vm->Call("greet", {}, &ret, &err) == ScriptStatus::Ok);
  CHECK(ret[0].IsString() && ret[0].string == "hi there");

  // No such function is reported, not a crash.
  CHECK(vm->Call("nope", {}, &ret, &err) == ScriptStatus::NoFunction);

  // Division by zero is a script error, not a crash.
  const char* div = "func boom\n  push 1\n  push 0\n  div\n  ret 1\nend\n";
  CHECK(vm->Load(div, "div", &err));
  CHECK(vm->Call("boom", {}, &ret, &err) == ScriptStatus::Error);
  CHECK(err.occurred);
}

static void TestAssemblerErrors() {
  std::unique_ptr<IScriptVm> vm(CreateReferenceVm());
  ScriptError err;
  // Unknown opcode.
  CHECK(!vm->Load("func f\n  frobnicate\nend\n", "bad", &err));
  CHECK(err.occurred);
  // Undefined label.
  CHECK(!vm->Load("func f\n  jmp nowhere\nend\n", "bad2", &err));
  CHECK(err.occurred);
  // Unterminated function.
  CHECK(!vm->Load("func f\n  push 1\n", "bad3", &err));
  CHECK(err.occurred);
  // Duplicate function.
  CHECK(!vm->Load("func f\nend\nfunc f\nend\n", "bad4", &err));
  CHECK(err.occurred);
  // Bad number literal.
  CHECK(!vm->Load("func f\n  push 1x\nend\n", "bad5", &err));
  CHECK(err.occurred);
  // A failed load leaves the VM unloaded (fail closed).
  CHECK(!vm->loaded());
}

static void TestInstructionBudget() {
  std::unique_ptr<IScriptVm> vm(CreateReferenceVm());
  ScriptError err;
  const char* spin =
      "func spin\n"
      "loop:\n"
      "  jmp loop\n"
      "end\n";
  CHECK(vm->Load(spin, "spin", &err));
  ScriptLimits lim;
  lim.instructionBudget = 5000;
  vm->SetLimits(lim);

  std::vector<ScriptValue> ret;
  ScriptStatus st = vm->Call("spin", {}, &ret, &err);
  CHECK(st == ScriptStatus::BudgetExceeded);
  CHECK(err.occurred);
  CHECK(vm->InstructionsExecuted() >= 5000);
}

static void TestMemoryLimit() {
  std::unique_ptr<IScriptVm> vm(CreateReferenceVm());
  ScriptError err;
  // Build a large string by repeated concatenation.
  const char* grow =
      "func grow\n"
      "  push \"\"\n"
      "  store s\n"
      "  push 0\n"
      "  store i\n"
      "loop:\n"
      "  load i\n"
      "  push 1000\n"
      "  lt\n"
      "  jz done\n"
      "  load s\n"
      "  push \"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"\n"
      "  add\n"
      "  store s\n"
      "  load i\n"
      "  push 1\n"
      "  add\n"
      "  store i\n"
      "  jmp loop\n"
      "done:\n"
      "  load s\n"
      "  ret 1\n"
      "end\n";
  CHECK(vm->Load(grow, "grow", &err));
  ScriptLimits lim;
  lim.instructionBudget = 10000000;   // generous: memory must trip first
  lim.memoryLimitBytes = 4096;
  vm->SetLimits(lim);
  std::vector<ScriptValue> ret;
  ScriptStatus st = vm->Call("grow", {}, &ret, &err);
  CHECK(st == ScriptStatus::MemoryExceeded);
  CHECK(err.occurred);
}

static void TestTimeslice() {
  // ScopedScriptTimeslice must be created on the script thread; run it there.
  std::atomic<bool> ok{false};
  std::atomic<bool> expired{false};
  std::thread worker([&]() {
    ThreadModel::Instance().Enter(ThreadRole::Scripts);
    std::unique_ptr<IScriptVm> vm(CreateReferenceVm());
    {
      ScopedScriptTimeslice ts(vm.get(), 5);
      ok.store(!ts.expired());
      // Busy-wait past the deadline.
      while (!ts.expired()) {
      }
      expired.store(true);
      // Expiry must have armed the VM interrupt so the interpreter stops.
      CHECK(vm->interrupted());
      CHECK(ts.elapsedMs() >= 5);
    }
    // The guard clears the interrupt on scope exit so the next callback starts
    // from a clean slate.
    CHECK(!vm->interrupted());
    ThreadModel::Instance().Leave();
  });
  worker.join();
  CHECK(ok.load());
  CHECK(expired.load());
}

static void TestBindingsGate() {
  ScriptBindings bindings;
  Capability renderCap;
  CHECK(Capability::Parse("render", &renderCap));
  bindings.Register("draw", renderCap,
                    [](ScriptContext&, const std::vector<ScriptValue>& args) {
                      return ScriptValue::Num(args.empty() ? 0 : args[0].AsNumber());
                    });
  Capability netCap;
  CHECK(Capability::Parse("network:domain=api.example.com", &netCap));
  bindings.Register("fetch", netCap,
                    [](ScriptContext&, const std::vector<ScriptValue>&) {
                      return ScriptValue::Str("data");
                    });

  CHECK(bindings.Count() == 2);
  CHECK(bindings.Has("draw"));
  CHECK(bindings.Required("draw").kind == CapKind::Render);
  CHECK(bindings.Required("missing").kind == CapKind::Unknown);

  // Script granted only render.
  ScriptContext ctx("cps", Grant({"render"}));
  bool ok = false, denied = false;
  ScriptValue v = bindings.Invoke(ctx, "draw", {ScriptValue::Num(7)}, &ok, &denied);
  CHECK(ok && !denied && v.AsNumber() == 7);

  // Network binding is denied without the network grant (fail closed).
  ok = false; denied = false;
  v = bindings.Invoke(ctx, "fetch", {}, &ok, &denied);
  CHECK(!ok && denied);

  // Unknown binding: not a denial, just absent.
  ok = false; denied = false;
  bindings.Invoke(ctx, "ghost", {}, &ok, &denied);
  CHECK(!ok && !denied);
}

// The gate must fail CLOSED when a binding's capability is unset. An unset
// requirement (a default-constructed Capability, kind == Unknown) means "the
// author forgot to declare one", not "no capability needed": it must be denied,
// and only an explicit Capability::NoCap may run without a grant.
static void TestGateDeniesUnsetCapability() {
  ScriptBindings bindings;
  // A binding registered the way a careless caller would: no capability named.
  Capability unset;
  CHECK(unset.kind == CapKind::Unknown);
  bindings.Register("forgot", unset,
                    [](ScriptContext&, const std::vector<ScriptValue>&) {
                      return ScriptValue::Bool(true);
                    });
  // A binding that names NoCap on purpose: genuinely capability-free.
  bindings.Register("scheduler", Capability::NoCap(),
                    [](ScriptContext&, const std::vector<ScriptValue>&) {
                      return ScriptValue::Bool(true);
                    });

  // Even a generously granted script cannot reach the unset binding.
  ScriptContext rich("rich", Grant({"render", "network", "world", "combat",
                                    "input", "hud", "perf", "events", "audio",
                                    "cosmetic", "fileio"}));
  bool ok = true, denied = false;
  bindings.Invoke(rich, "forgot", {}, &ok, &denied);
  CHECK(!ok && denied);

  // And of course not a script with no grants either.
  ScriptContext bare("bare", Grant({}));
  ok = true; denied = false;
  bindings.Invoke(bare, "forgot", {}, &ok, &denied);
  CHECK(!ok && denied);

  // NoCap runs with no grants at all.
  ok = false; denied = true;
  ScriptValue v = bindings.Invoke(bare, "scheduler", {}, &ok, &denied);
  CHECK(ok && !denied && v.Truthy());
}

// NoCap is a binding requirement, never something a manifest can grant, and it
// must never cover anything when it appears as a grant side.
static void TestNoCapIsNotGrantable() {
  CapabilitySet s;
  CHECK(!s.Add("nocap"));                    // not a manifest capability
  CHECK(!s.Add(Capability::NoCap()));        // not addable as a grant
  CHECK(s.empty());
  // A NoCap "grant" covers nothing, including another NoCap requirement.
  CHECK(!Capability::NoCap().Covers(Capability::NoCap()));
  // An unset capability covers nothing either.
  Capability unset;
  Capability render;
  CHECK(Capability::Parse("render", &render));
  CHECK(!unset.Covers(render));
  CHECK(!unset.Covers(unset));
}

static void TestHostBindingsThroughVm() {
  auto vm = std::unique_ptr<IScriptVm>(CreateReferenceVm());
  ScriptBindings bindings;
  Capability renderCap;
  CHECK(Capability::Parse("render", &renderCap));
  int draws = 0;
  bindings.Register("draw", renderCap,
                    [&](ScriptContext&, const std::vector<ScriptValue>& args) {
                      ++draws;
                      return ScriptValue::Num(args.empty() ? 0 : args[0].AsNumber());
                    });
  Capability netCap;
  CHECK(Capability::Parse("network:*", &netCap));
  bindings.Register("fetch", netCap,
                    [](ScriptContext&, const std::vector<ScriptValue>&) {
                      return ScriptValue::Str("payload");
                    });

  // Host granted render only.
  ScriptHost host("hud", Grant({"render"}), std::move(vm));
  host.SetBindings(&bindings);
  ScriptError err;
  const char* src =
      "func use_render\n"
      "  push 5\n"
      "  native draw 1\n"
      "  ret 1\n"
      "end\n"
      "func use_net\n"
      "  native fetch 0\n"
      "  ret 1\n"
      "end\n";
  CHECK(host.Load(src, "bind", &err));

  std::vector<ScriptValue> ret;
  CHECK(host.RunHook("use_render", {}, &ret) == ScriptStatus::Ok);
  CHECK(draws == 1);
  CHECK(ret.size() == 1 && ret[0].AsNumber() == 5);

  // Using the network binding without the grant faults the host.
  CHECK(host.RunHook("use_net", {}, &ret) == ScriptStatus::CapabilityDenied);
  CHECK(host.state() == ScriptHostState::Unloaded);
  CHECK(!host.loaded());
}

static void TestErrorIsolation() {
  ScriptHost host("faulty", Grant({"render"}), nullptr);
  ScriptError err;
  const char* src =
      "func boom\n"
      "  abort \"kaboom\"\n"
      "end\n";
  CHECK(host.Load(src, "fault", &err));
  CHECK(host.loaded());
  CHECK(host.RunHook("boom") == ScriptStatus::Error);
  CHECK(host.lastError().occurred);
  CHECK(host.lastError().message == "kaboom");
  // Faulted: further hooks refuse to run.
  CHECK(host.state() == ScriptHostState::Unloaded);
  CHECK(host.RunHook("boom") == ScriptStatus::NotLoaded);
  CHECK(host.Faults() == 1);

  // A missing hook is not a fault.
  ScriptHost quiet("quiet", Grant({"render"}), nullptr);
  CHECK(quiet.Load("func on_tick\n  ret 0\nend\n", "quiet", &err));
  CHECK(quiet.RunHook("on_frame") == ScriptStatus::NoFunction);
  CHECK(quiet.state() == ScriptHostState::Loaded);
  CHECK(quiet.Faults() == 0);

  // Reloading recovers a faulted host.
  CHECK(host.Load(src, "fault", &err));
  CHECK(host.loaded());
}

static void TestStateRoundTrip() {
  ScriptHost host("stateful", Grant({"render"}), nullptr);
  ScriptError err;
  const char* src =
      "func on_load\n"
      "  push 7\n"
      "  store counter\n"
      "  push \"hello\"\n"
      "  store label\n"
      "  ret 0\n"
      "end\n"
      "func get\n"
      "  load counter\n"
      "  ret 1\n"
      "end\n";
  CHECK(host.Load(src, "state", &err));
  CHECK(host.RunHook("on_load") == ScriptStatus::Ok);
  std::vector<ScriptValue> ret;
  CHECK(host.RunHook("get", {}, &ret) == ScriptStatus::Ok);
  CHECK(ret[0].AsNumber() == 7);

  std::string snapshot = host.SerializeState();
  CHECK(!snapshot.empty());
  CHECK(snapshot.find("counter") != std::string::npos);

  // Mutate, then restore and confirm the state comes back.
  const char* bump =
      "func get\n  load counter\n  ret 1\nend\n"
      "func bump\n  push 99\n  store counter\n  ret 0\nend\n";
  CHECK(host.Load(bump, "state", &err));
  CHECK(host.RunHook("bump") == ScriptStatus::Ok);
  CHECK(host.RunHook("get", {}, &ret) == ScriptStatus::Ok);
  CHECK(ret[0].AsNumber() == 99);

  CHECK(host.RestoreState(snapshot, &err));
  CHECK(host.RunHook("get", {}, &ret) == ScriptStatus::Ok);
  CHECK(ret[0].AsNumber() == 7);

  // Restoring garbage fails closed.
  CHECK(!host.RestoreState("not a state", &err));
  CHECK(err.occurred);
}

static void TestWorker() {
  ScriptWorker worker;
  CHECK(!worker.running());
  // Posting before Start fails closed.
  CHECK(!worker.Post([]() {}));

  worker.Start();
  CHECK(worker.running());

  std::atomic<int> count{0};
  for (int i = 0; i < 5; ++i) {
    CHECK(worker.Post([&count]() { count.fetch_add(1); }));
  }
  // Wait for the worker to drain.
  for (int i = 0; i < 200 && count.load() < 5; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(count.load() == 5);
  CHECK(worker.Executed() >= 5);

  worker.Stop();
  CHECK(!worker.running());

  // Stop() must drain queued work rather than dropping it.
  ScriptWorker w2;
  std::atomic<int> n2{0};
  w2.Start();
  w2.Post([&n2]() { n2.fetch_add(1); });
  w2.Post([&n2]() { n2.fetch_add(1); });
  w2.Stop();
  CHECK(n2.load() == 2);
}

static void TestWorkerScriptRole() {
  // A task run on the worker must see ThreadRole::Scripts.
  ScriptWorker worker;
  worker.Start();
  std::atomic<bool> sawScripts{false};
  worker.Post([&]() {
    sawScripts.store(ThreadModel::Instance().Is(ThreadRole::Scripts));
  });
  for (int i = 0; i < 200 && !sawScripts.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(sawScripts.load());
  worker.Stop();
  // The main thread is unaffected.
  CHECK(ThreadModel::Instance().CurrentRole() == ThreadRole::Unknown);
}

int main() {
  TestValues();
  TestVmBasics();
  TestAssemblerErrors();
  TestInstructionBudget();
  TestMemoryLimit();
  TestTimeslice();
  TestBindingsGate();
  TestGateDeniesUnsetCapability();
  TestNoCapIsNotGrantable();
  TestHostBindingsThroughVm();
  TestErrorIsolation();
  TestStateRoundTrip();
  TestWorker();
  TestWorkerScriptRole();
  if (g_fail == 0) printf("test_script: ALL OK\n");
  return g_fail;
}
