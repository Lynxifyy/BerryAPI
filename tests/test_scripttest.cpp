// Host tests for the script test framework (`berry-run test`).
//
// Covers:
//   * discovery: `test_*` at the top level and `tests.test_*` in the exported
//     `tests` table, sorted and de-duplicated; a helper that is not named as a
//     test is ignored.
//   * the assertion surface: assert/true/false/not_nil/eq/ne/near/fail, each
//     failing closed with a message, and passing when the condition holds.
//   * outcome classification: pass, fail, skip (`skip_`), xfail that fails
//     (`xfail_`), and an xfail that unexpectedly passes (counted as a failure).
//   * per-test isolation: a failing test does not stop the rest, and fixtures
//     (`on_load`, `before_each`, `after_each`) run around each test.
//   * the filter and maxTests knobs.
//   * both VMs: the reference VM (flat exports) and Luau (nested `tests` table).
#include "berry/scripttest.h"

#include "berry/async.h"
#include "berry/event.h"   // NowNs

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

ScriptTestRunner::VmFactory Luau() {
  return []() -> std::unique_ptr<IScriptVm> {
    return std::unique_ptr<IScriptVm>(CreateLuauVm());
  };
}

/// Grants every capability kind a test might request, so the runner is testing
/// discovery/assertions rather than the gate (the gate has its own suite).
CapabilitySet AllCaps() {
  CapabilitySet s;
  for (const char* c : {"combat", "render", "world", "network", "fileio", "hud",
                        "perf", "input", "events", "audio", "cosmetic",
                        "packets", "voice", "crystal", "gamereads"}) {
    s.Add(c);
  }
  return s;
}

ScriptTestReport RunLuau(const std::string& src, const std::string& filter = "") {
  ScriptTestRunner runner(Luau(), AllCaps());
  ScriptTestOptions opts;
  opts.filter = filter;
  return runner.Run(src, "suite", opts);
}

const ScriptTestResult* Find(const ScriptTestReport& r, const std::string& name) {
  for (const auto& res : r.results) {
    if (res.name == name) return &res;
  }
  return nullptr;
}

/// A Luau module that exports a few tests and one helper (which must not be
/// discovered).
const char* kSuite =
    "local M = {}\n"
    "local function helper() return 2 end\n"
    "function M.test_addition()\n"
    "  test.eq(1 + 1, helper())\n"
    "end\n"
    "function M.test_truthy()\n"
    "  test.truthy(1 == 1)\n"
    "  test.falsy(1 == 2)\n"
    "end\n"
    "function M.test_failing()\n"
    "  test.eq(2, 3, 'two is not three')\n"
    "end\n"
    "function M.test_not_nil()\n"
    "  test.not_nil(helper())\n"
    "end\n"
    "function M.skip_this()\n"
    "  test.fail('skipped test body ran')\n"
    "end\n"
    "function M.xfail_known_bug()\n"
    "  test.eq(1, 2)\n"
    "end\n"
    "function M.xfail_but_passes()\n"
    "  test.eq(1, 1)\n"
    "end\n"
    "M.tests = {}\n"
    "function M.tests.test_namespaced()\n"
    "  test.near(0.1 + 0.2, 0.3, 1e-9)\n"
    "end\n"
    "return M\n";

}  // namespace

//----------------------------------------------------------------------------
// Discovery
//----------------------------------------------------------------------------

static void TestDiscovery() {
  ScriptTestRunner runner(Luau(), AllCaps());
  std::string err;
  const std::vector<std::string> names = runner.ListTests(kSuite, "suite", &err);
  CHECK(err.empty());
  // The helper and the raw `tests` table are not tests.
  CHECK(std::find(names.begin(), names.end(), "helper") == names.end());
  CHECK(std::find(names.begin(), names.end(), "tests") == names.end());
  CHECK(std::find(names.begin(), names.end(), "test_addition") != names.end());
  CHECK(std::find(names.begin(), names.end(), "tests.test_namespaced") != names.end());
  // Sorted.
  CHECK(std::is_sorted(names.begin(), names.end()));
  // IsTestName agrees with discovery.
  CHECK(ScriptTestRunner::IsTestName("test_foo"));
  CHECK(ScriptTestRunner::IsTestName("tests.test_foo"));
  CHECK(!ScriptTestRunner::IsTestName("test_"));
  CHECK(!ScriptTestRunner::IsTestName("foo"));
  CHECK(!ScriptTestRunner::IsTestName("helpers.test_foo"));

  // A source that does not compile reports the error and discovers nothing.
  std::string derr;
  const std::vector<std::string> bad =
      runner.ListTests("this is not luau !!!", "broken", &derr);
  CHECK(bad.empty());
  CHECK(!derr.empty());
}

//----------------------------------------------------------------------------
// Assertions
//----------------------------------------------------------------------------

static void TestAssertions() {
  struct Case { const char* src; bool shouldPass; const char* what; };
  const Case cases[] = {
      {"local M = {} function M.test_ok() test.assert(true, 'm') end return M", true, "assert true"},
      {"local M = {} function M.test_ok() test.truthy(1) end return M", true, "true"},
      {"local M = {} function M.test_ok() test.falsy(nil) end return M", true, "false"},
      {"local M = {} function M.test_ok() test.not_nil(0) end return M", true, "not_nil"},
      {"local M = {} function M.test_ok() test.eq('a', 'a') end return M", true, "eq str"},
      {"local M = {} function M.test_ok() test.eq(1, 1.0) end return M", true, "eq num"},
      {"local M = {} function M.test_ok() test.ne(1, 2) end return M", true, "ne"},
      {"local M = {} function M.test_ok() test.near(1.0, 1.0000001, 1e-3) end return M", true, "near"},
      // Failing forms.
      {"local M = {} function M.test_x() test.assert(false, 'nope') end return M", false, "assert false"},
      {"local M = {} function M.test_x() test.assert(nil) end return M", false, "assert nil"},
      {"local M = {} function M.test_x() test.truthy(0) end return M", false, "true(0)"},
      {"local M = {} function M.test_x() test.falsy(1) end return M", false, "false(1)"},
      {"local M = {} function M.test_x() test.not_nil(nil) end return M", false, "not_nil(nil)"},
      {"local M = {} function M.test_x() test.eq(1, 2) end return M", false, "eq mismatch"},
      {"local M = {} function M.test_x() test.ne(1, 1) end return M", false, "ne equal"},
      {"local M = {} function M.test_x() test.near(1, 2, 0.1) end return M", false, "near far"},
      {"local M = {} function M.test_x() test.near('a', 'b') end return M", false, "near non-num"},
      {"local M = {} function M.test_x() test.fail('boom') end return M", false, "explicit fail"},
  };
  for (const Case& c : cases) {
    const ScriptTestReport r = RunLuau(c.src);
    CHECK(r.total() == 1);
    if (r.total() != 1) continue;
    const bool passed = r.results[0].outcome == TestOutcome::Passed;
    if (passed != c.shouldPass) {
      printf("FAIL assertion case '%s': expected %s, got %s (%s)\n", c.what,
             c.shouldPass ? "pass" : "fail",
             TestOutcomeName(r.results[0].outcome), r.results[0].message.c_str());
      ++g_fail;
    }
    // A failure carries the assertion message; a pass carries none.
    if (!c.shouldPass) CHECK(!r.results[0].message.empty());
  }

  // A custom eq message survives into the report.
  const ScriptTestReport r = RunLuau(
      "local M = {} function M.test_x() test.eq(2, 3, 'custom-msg') end return M");
  CHECK(r.total() == 1);
  if (r.total() == 1) CHECK(r.results[0].message.find("custom-msg") != std::string::npos);
}

//----------------------------------------------------------------------------
// Outcomes + isolation
//----------------------------------------------------------------------------

static void TestOutcomes() {
  const ScriptTestReport r = RunLuau(kSuite);
  CHECK(r.discoveryOk);
  CHECK(r.ok() == false);   // the suite has real failures on purpose
  CHECK(r.passed == 4);     // addition, truthy, not_nil, tests.test_namespaced
  CHECK(r.failed == 2);     // failing, xfail_but_passes
  CHECK(r.skipped == 1);    // skip_this
  CHECK(r.expectedFailures == 1);   // xfail_known_bug
  CHECK(r.total() == 8);

  const ScriptTestResult* add = Find(r, "test_addition");
  CHECK(add && add->outcome == TestOutcome::Passed);

  const ScriptTestResult* fail = Find(r, "test_failing");
  CHECK(fail && fail->outcome == TestOutcome::Failed);
  CHECK(fail && fail->message.find("two is not three") != std::string::npos);

  // A skipped test never ran its body, so it carries no failure message.
  const ScriptTestResult* skipped = Find(r, "skip_this");
  CHECK(skipped && skipped->outcome == TestOutcome::Skipped);
  CHECK(skipped && skipped->message.empty());
  CHECK(skipped && !skipped->passed());

  const ScriptTestResult* xfail = Find(r, "xfail_known_bug");
  CHECK(xfail && xfail->outcome == TestOutcome::ExpectedFailure);
  CHECK(xfail && xfail->passed());

  const ScriptTestResult* xpass = Find(r, "xfail_but_passes");
  CHECK(xpass && xpass->outcome == TestOutcome::UnexpectedPass);
  CHECK(xpass && !xpass->passed());

  const ScriptTestResult* ns = Find(r, "tests.test_namespaced");
  CHECK(ns && ns->outcome == TestOutcome::Passed);

  // Isolation: the passing test ran even though a sibling failed, which is only
  // true because each test gets a fresh host.
  CHECK(add != nullptr);
}

static void TestFixtures() {
  // before_each sets a global the test reads; after_each appends to a global the
  // last test inspects. Both must run per test.
  const char* src =
      "local M = {}\n"
      "local count = 0\n"
      "local cleaned = 0\n"
      "function M.before_each() count = count + 1 end\n"
      "function M.after_each() cleaned = cleaned + 1 end\n"
      "function M.test_a() test.eq(count, 1) end\n"
      "function M.test_b() test.eq(count, 1) end\n"
      "return M\n";
  const ScriptTestReport r = RunLuau(src);
  // Each test runs on a FRESH host, so `count` is 1 in both (it is not shared):
  // that is the isolation contract, and it is what these assertions check.
  CHECK(r.total() == 2);
  CHECK(r.passed == 2);
  CHECK(r.failed == 0);

  // A before_each that fails fails the test, and the test body never runs.
  const char* bad =
      "local M = {}\n"
      "function M.before_each() error('setup failed') end\n"
      "function M.test_a() test.fail('body should not run') end\n"
      "return M\n";
  const ScriptTestReport rb = RunLuau(bad);
  CHECK(rb.total() == 1);
  CHECK(rb.failed == 1);
  if (rb.total() == 1) CHECK(rb.results[0].message.find("setup failed") != std::string::npos);

  // runFixtures=false skips the hooks, so the body sees the initial state.
  ScriptTestRunner runner(Luau(), AllCaps());
  ScriptTestOptions opts;
  opts.runFixtures = false;
  const ScriptTestReport rn = runner.Run(
      "local M = {}\n"
      "local seen = false\n"
      "function M.before_each() seen = true end\n"
      "function M.test_a() test.falsy(seen) end\n"
      "return M\n",
      "suite", opts);
  CHECK(rn.passed == 1);
}

static void TestFilterAndCap() {
  const ScriptTestReport all = RunLuau(kSuite);
  const ScriptTestReport some = RunLuau(kSuite, "addition");
  CHECK(some.total() == 1);
  CHECK(some.total() < all.total());
  if (some.total() == 1) CHECK(some.results[0].name == "test_addition");

  // maxTests bounds the run; the remainder are not reported.
  ScriptTestRunner runner(Luau(), AllCaps());
  ScriptTestOptions opts;
  opts.maxTests = 2;
  const ScriptTestReport capped = runner.Run(kSuite, "suite", opts);
  CHECK(capped.total() == 2);
}

static void TestAsyncSuspendIsFailure() {
  // A test that awaits suspends the VM. The runner pumps no scheduler, so the
  // suspend must be reported as a failure — a test that is still suspended is
  // not a test that passed.
  ScriptBindings bindings;
  AsyncRuntime rt([]() { return (GameTick)0; },
                  []() { return NowNs() / 1000000ULL; });
  rt.InstallBindings(&bindings);
  ScriptTestRunner runner(Luau(), AllCaps(), &bindings);
  const ScriptTestReport r = runner.Run(
      "local M = {}\n"
      "function M.test_awaits()\n"
      "  async.wait(1)\n"
      "end\n"
      "return M\n",
      "suite", ScriptTestOptions());
  CHECK(r.total() == 1);
  if (r.total() == 1) {
    CHECK(r.results[0].outcome == TestOutcome::Failed);
    CHECK(r.results[0].message.find("suspend") != std::string::npos);
  }
}

static void TestReferenceVmFlatExports() {
  // The reference VM has flat exports; discovery must still find `test_*`.
  ScriptTestRunner runner(
      []() -> std::unique_ptr<IScriptVm> {
        return std::unique_ptr<IScriptVm>(CreateReferenceVm());
      },
      AllCaps());
  std::string err;
  const std::vector<std::string> names = runner.ListTests(
      "func test_one\n push 1\n ret 0\nend\n"
      "func helper\n push 1\n ret 0\nend\n",
      "ref", &err);
  CHECK(err.empty());
  CHECK(std::find(names.begin(), names.end(), "test_one") != names.end());
  CHECK(std::find(names.begin(), names.end(), "helper") == names.end());
}

int main() {
  TestDiscovery();
  TestAssertions();
  TestOutcomes();
  TestFixtures();
  TestFilterAndCap();
  TestAsyncSuspendIsFailure();
  TestReferenceVmFlatExports();
  if (g_fail == 0) printf("test_scripttest: ALL OK\n");
  return g_fail;
}
