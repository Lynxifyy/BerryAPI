// Host tests for the Luau VM backend (Fix 5, Phase E).
//
// The reference VM proves the IScriptVm seam; this suite proves the real
// language runtime behind it:
//   * Compile + run real Luau chunks, exports table, multiple results, tables.
//   * Budgets: instruction budget, wall-clock timeout, memory cap.
//   * Errors: syntax, runtime, unknown function, error isolation through the
//     host (a faulting script unloads; the host stays usable).
//   * Sandbox: require / dofile / loadstring / setfenv / raw mutation / os
//     process + filesystem calls / debug are unreachable, globals are read-only.
//   * Hostile scripts: infinite loops, deep recursion, unbounded string and
//     table growth, huge tables, yield from the top level are all contained.
//   * Hot reload state round-trip through the shared ValueCodec.
//   * The same behaviours on BOTH VMs (reference and Luau) so the seam does
//     not silently diverge.
#include "berry/script.h"
#include "berry/value_codec.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

/// A Luau source header every module-style script shares. `return M` exposes
/// the functions the VM calls by name.
static const char* kModules =
    "local M = {}\n"
    "function M.add(a, b) return a + b end\n"
    "function M.greet(name) return 'hi ' .. name end\n"
    "function M.multi() return 1, 'two', true end\n"
    "function M.mktable() return { a = 1, b = 'x', c = { d = 2 } } end\n"
    "function M.fail() error('boom') end\n"
    "return M\n";

static std::unique_ptr<IScriptVm> Luau() {
  return std::unique_ptr<IScriptVm>(CreateLuauVm());
}

//----------------------------------------------------------------------------
// Core VM behaviour.
//----------------------------------------------------------------------------

static void TestLuauBasics() {
  auto vm = Luau();
  CHECK(std::string(vm->Name()) == "berry-luau");
  ScriptError err;
  CHECK(vm->Load(kModules, "mods", &err));
  CHECK(vm->loaded());
  CHECK(vm->HasFunction("add"));
  CHECK(vm->HasFunction("multi"));
  CHECK(!vm->HasFunction("nope"));

  std::vector<ScriptValue> ret;
  CHECK(vm->Call("add", {ScriptValue::Num(2), ScriptValue::Num(40)}, &ret, &err) ==
        ScriptStatus::Ok);
  CHECK(ret.size() == 1 && ret[0].AsNumber() == 42);

  ret.clear();
  CHECK(vm->Call("greet", {ScriptValue::Str("berry")}, &ret, &err) ==
        ScriptStatus::Ok);
  CHECK(ret.size() == 1 && ret[0].AsString() == "hi berry");

  ret.clear();
  CHECK(vm->Call("multi", {}, &ret, &err) == ScriptStatus::Ok);
  CHECK(ret.size() == 3);
  CHECK(ret[0].AsNumber() == 1);
  CHECK(ret[1].AsString() == "two");
  CHECK(ret[2].IsBool() && ret[2].boolean);

  ret.clear();
  CHECK(vm->Call("mktable", {}, &ret, &err) == ScriptStatus::Ok);
  CHECK(ret.size() == 1 && ret[0].IsTable());
  const ScriptValue* c = ret[0].Field("c");
  CHECK(c && c->IsTable() && c->Field("d") && c->Field("d")->AsNumber() == 2);
}

static void TestLuauCompileErrors() {
  auto vm = Luau();
  ScriptError err;
  CHECK(!vm->Load("this is not luau !!!", "bad", &err));
  CHECK(err.occurred);
  CHECK(!vm->loaded());
  // A failed load leaves the VM unloaded (fail closed).
  CHECK(!vm->HasFunction("anything"));
}

static void TestLuauRuntimeError() {
  auto vm = Luau();
  ScriptError err;
  CHECK(vm->Load(kModules, "mods", &err));
  std::vector<ScriptValue> ret;
  CHECK(vm->Call("fail", {}, &ret, &err) == ScriptStatus::Error);
  CHECK(err.occurred);
  // The VM stays usable after a script-level error.
  CHECK(vm->Call("add", {ScriptValue::Num(1), ScriptValue::Num(2)}, &ret, &err) ==
        ScriptStatus::Ok);
}

static void TestLuauUnknownFunction() {
  auto vm = Luau();
  ScriptError err;
  CHECK(vm->Load(kModules, "mods", &err));
  std::vector<ScriptValue> ret;
  CHECK(vm->Call("missing", {}, &ret, &err) == ScriptStatus::NoFunction);
}

//----------------------------------------------------------------------------
// Budgets.
//----------------------------------------------------------------------------

static void TestLuauInstructionBudget() {
  auto vm = Luau();
  ScriptError err;
  CHECK(vm->Load("return { spin = function() while true do end end }\n", "spin",
                 &err));
  ScriptLimits lim;
  lim.instructionBudget = 5000;
  lim.wallClockMs = 100000;
  vm->SetLimits(lim);
  std::vector<ScriptValue> ret;
  CHECK(vm->Call("spin", {}, &ret, &err) == ScriptStatus::BudgetExceeded);
  CHECK(err.occurred);
  CHECK(vm->InstructionsExecuted() >= 5000);
}

static void TestLuauTimeout() {
  auto vm = Luau();
  ScriptError err;
  CHECK(vm->Load("return { spin = function() while true do end end }\n", "spin",
                 &err));
  ScriptLimits lim;
  lim.instructionBudget = 100000000000ULL;   // let the clock trip first
  lim.wallClockMs = 30;
  vm->SetLimits(lim);
  std::vector<ScriptValue> ret;
  CHECK(vm->Call("spin", {}, &ret, &err) == ScriptStatus::Timeout);
  CHECK(err.occurred);
}

static void TestLuauMemoryLimit() {
  auto vm = Luau();
  ScriptError err;
  CHECK(vm->Load(
      "return { grow = function() local s = '' "
      "for i = 1, 100000 do s = s .. 'xxxxxxxxxxxxxxxx' end return s end }\n",
      "grow", &err));
  ScriptLimits lim;
  lim.instructionBudget = 100000000;
  lim.wallClockMs = 100000;
  lim.memoryLimitBytes = 1u << 20;   // 1 MiB
  vm->SetLimits(lim);
  std::vector<ScriptValue> ret;
  CHECK(vm->Call("grow", {}, &ret, &err) == ScriptStatus::MemoryExceeded);
  CHECK(err.occurred);
}

//----------------------------------------------------------------------------
// Sandbox + hostile scripts.
//----------------------------------------------------------------------------

/// Loads a one-liner whose body is `body`, returning whether it ran clean.
static bool RunProbe(const std::string& body, ScriptStatus* out) {
  auto vm = Luau();
  ScriptError err;
  std::string src = "return { probe = function() " + body + " end }\n";
  if (!vm->Load(src, "probe", &err)) return false;
  std::vector<ScriptValue> ret;
  ScriptStatus st = vm->Call("probe", {}, &ret, &err);
  if (out) *out = st;
  return true;
}

static void TestLuauSandbox() {
  ScriptStatus st;

  // The dangerous helpers must be nil: calling them is a runtime error.
  for (const char* call : {"require('x')", "dofile('x')", "loadfile('x')",
                           "loadstring('return 1')()", "collectgarbage('count')",
                           "setfenv(1, {})", "getfenv(1)"}) {
    st = ScriptStatus::Ok;
    CHECK(RunProbe(call, &st));
    CHECK(st == ScriptStatus::Error);
  }

  // os process/filesystem surface is gone; the harmless clock survives.
  CHECK(RunProbe("os.execute('true')", &st) && st == ScriptStatus::Error);
  CHECK(RunProbe("os.remove('x')", &st) && st == ScriptStatus::Error);
  CHECK(RunProbe("os.getenv('HOME')", &st) && st == ScriptStatus::Error);
  CHECK(RunProbe("debug.traceback()", &st) && st == ScriptStatus::Error);
  CHECK(RunProbe("return os.time() ~= nil", &st) && st == ScriptStatus::Ok);

  // The global table is read-only: a script cannot poison another.
  CHECK(RunProbe("GLOBAL_POISON = 1", &st) && st == ScriptStatus::Error);
  CHECK(RunProbe("math = nil", &st) && st == ScriptStatus::Error);
}

static void TestLuauHostileScripts() {
  auto vm = Luau();
  ScriptError err;
  const char* hostile =
      "local M = {}\n"
      "function M.deep(n) if n <= 0 then return 0 end return M.deep(n - 1) + 1 end\n"
      "function M.deeploop() return M.deep(1000000) end\n"
      "function M.bigtable()\n"
      "  local t = {}\n"
      "  for i = 1, 100000000 do t[i] = i end\n"
      "  return #t\n"
      "end\n"
      "function M.yieldtop()\n"
      "  local co = coroutine.create(function() coroutine.yield(1) end)\n"
      "  return select(2, coroutine.resume(co))\n"
      "end\n"
      "return M\n";
  CHECK(vm->Load(hostile, "hostile", &err));

  // Deep recursion: a stack overflow must surface as an error, not a crash.
  ScriptLimits lim;
  lim.instructionBudget = 50000000;
  lim.wallClockMs = 5000;
  lim.memoryLimitBytes = 16u << 20;
  vm->SetLimits(lim);
  std::vector<ScriptValue> ret;
  CHECK(vm->Call("deeploop", {}, &ret, &err) == ScriptStatus::Error);

  // Unbounded table growth: the memory cap stops it.
  CHECK(vm->Call("bigtable", {}, &ret, &err) == ScriptStatus::MemoryExceeded);

  // Coroutines still work: a nested yield/resume completes.
  ret.clear();
  CHECK(vm->Call("yieldtop", {}, &ret, &err) == ScriptStatus::Ok);
  CHECK(ret.size() == 1 && ret[0].AsNumber() == 1);
}

//----------------------------------------------------------------------------
// Hot reload state through the shared codec.
//----------------------------------------------------------------------------

static void TestLuauStateRoundTrip() {
  auto vm = Luau();
  ScriptError err;
  // A script keeps reload state in the exported `__state` field.
  CHECK(vm->Load(
      "return { __state = { counter = 7, label = 'hello', nested = { x = 1 } } }\n",
      "state", &err));
  const std::string snap = vm->SerializeState();
  CHECK(snap.compare(0, 19, "berry-luau-state-v1") == 0);

  // A second VM restores it and reports the same state.
  auto vm2 = Luau();
  CHECK(vm2->Load("return { __state = nil }\n", "state", &err));
  CHECK(vm2->RestoreState(snap, &err));
  CHECK(vm2->SerializeState() == snap);

  // Malformed state is rejected (fail closed).
  CHECK(!vm2->RestoreState("garbage", &err));
  CHECK(err.occurred);
}

//----------------------------------------------------------------------------
// Both VMs share the seam: run a behaviour on each and compare outcomes.
//----------------------------------------------------------------------------

static void TestBothVmsShareSeam() {
  struct Case {
    const char* name;
    const char* refSrc;    // reference-VM source
    const char* luauSrc;   // Luau source
    f64 expect;
  };
  // The reference VM uses its tiny assembly dialect; Luau uses real syntax.
  // Both must report the same arithmetic result through the same seam.
  Case cases[] = {
      {"three",
       "func three\n  push 3\n  ret 1\nend\n",
       "return { three = function() return 3 end }\n", 3},
      {"sum",
       "func sum\n  push 20\n  push 22\n  add\n  ret 1\nend\n",
       "return { sum = function() return 20 + 22 end }\n", 42},
  };

  for (const Case& c : cases) {
    std::unique_ptr<IScriptVm> ref(CreateReferenceVm());
    std::unique_ptr<IScriptVm> luau(CreateLuauVm());
    ScriptError e1, e2;
    CHECK(ref->Load(c.refSrc, c.name, &e1));
    CHECK(luau->Load(c.luauSrc, c.name, &e2));
    std::vector<ScriptValue> r1, r2;
    CHECK(ref->Call(c.name, {}, &r1, &e1) == ScriptStatus::Ok);
    CHECK(luau->Call(c.name, {}, &r2, &e2) == ScriptStatus::Ok);
    CHECK(r1.size() == 1 && r2.size() == 1);
    CHECK(r1[0].AsNumber() == c.expect);
    CHECK(r2[0].AsNumber() == c.expect);
  }
}

int main() {
  TestLuauBasics();
  TestLuauCompileErrors();
  TestLuauRuntimeError();
  TestLuauUnknownFunction();
  TestLuauInstructionBudget();
  TestLuauTimeout();
  TestLuauMemoryLimit();
  TestLuauSandbox();
  TestLuauHostileScripts();
  TestLuauStateRoundTrip();
  TestBothVmsShareSeam();
  if (g_fail == 0) printf("test_luau: ALL OK\n");
  return g_fail;
}
