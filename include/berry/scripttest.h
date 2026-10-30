#pragma once
// Script test framework and the `berry-run test` runner.
//
// Two halves that only make sense together:
//
//   * an assertion surface (`test.assert`, `test.eq`, `test.near`, ...) a
//     script calls. These bindings are registered directly on the
//     ScriptBindings table rather than declared in the IDL, exactly like the
//     async bindings: this is developer tooling, not a game surface, so it must
//     not appear in the generated API and must not require a capability. Each
//     raises a script error on failure, so a failed assertion travels the
//     runtime's normal fault path and carries the chunk and the line.
//
//   * a runner that discovers and executes the tests. Discovery is by naming
//     convention because a function value cannot cross the ScriptValue boundary:
//     a native binding can never receive a script function, so an
//     `it(name, fn)` API is impossible through the C++ seam. A script instead
//     EXPORTS its tests and the runner enumerates the exports (via
//     IScriptVm::FunctionNames) and calls each by name:
//
//       * `test_<name>`            a bare exported function
//       * `tests.<name>`           a function in the exported `tests` table
//       * a leaf beginning with    the test is expected to fail (`xfail_`) or
//         `xfail_` / `skip_`       is skipped (`skip_`); a leading `test_` is
//                                  stripped first, so `test_xfail_x` works
//
//     Every test runs in a FRESH host over the same source, because error
//     isolation unloads a host when a hook faults: one failed assertion would
//     otherwise take the rest of the suite down with it. A fresh host per test
//     is the honest reading of "each test is independent". The lifecycle hooks
//     `on_load`, `before_each` and `after_each` run around each test when the
//     script exports them, so fixtures need no second mechanism.
//
// The runner touches no game state and needs no device: a host test drives the
// whole thing with the reference VM or Luau.

#include "berry/export.h"
#include "berry/manifest.h"
#include "berry/script.h"
#include "berry/types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Berry {

//----------------------------------------------------------------------------
// Assertion surface.
//----------------------------------------------------------------------------

/// Native binding names the assertion surface installs. Kept as constants so
/// the runner, the CLI and the docs agree on the spelling.
namespace ScriptTestBindings {
inline const char* Assert() { return "test.assert"; }
// `test.true` / `test.false` cannot be written in Luau: `true` and `false` are
// reserved words, so `test.true(x)` is a syntax error. The truthy/falsy pair
// keeps the same meaning under names the lexer accepts.
inline const char* Truthy() { return "test.truthy"; }
inline const char* Falsy() { return "test.falsy"; }
inline const char* NotNil() { return "test.not_nil"; }
inline const char* Eq() { return "test.eq"; }
inline const char* Ne() { return "test.ne"; }
inline const char* Near() { return "test.near"; }
inline const char* Fail() { return "test.fail"; }

/// Installs every assertion binding on `bindings`, each requiring no capability
/// (a binding must say so explicitly; an unset requirement is denied). A name
/// already present is left alone, so the call is idempotent and a second caller
/// cannot shadow the first. Returns how many were added.
BERRY_API size_t Install(ScriptBindings* bindings);
}  // namespace ScriptTestBindings

/// Lifecycle hook names the runner drives around each test, when exported.
namespace ScriptTestHooks {
inline const char* BeforeEach() { return "before_each"; }
inline const char* AfterEach() { return "after_each"; }
}  // namespace ScriptTestHooks

//----------------------------------------------------------------------------
// Results.
//----------------------------------------------------------------------------

/// How one test ended.
enum class TestOutcome : u32 {
  Passed = 0,
  Failed,
  Skipped,
  /// The test was named `xfail_...` and it did fail, as expected.
  ExpectedFailure,
  /// The test was named `xfail_...` and it passed. Counted as a failure: the
  /// marker is stale and the test should be un-xfailed.
  UnexpectedPass,
};

const char* TestOutcomeName(TestOutcome o);

/// One test's result. `message` carries the assertion/ fault text on a
/// non-pass, and is empty on a clean pass.
struct BERRY_API ScriptTestResult {
  std::string name;
  TestOutcome outcome = TestOutcome::Passed;
  std::string message;
  u64 elapsedMs = 0;

  /// True for a pass or an expected failure; false for a failure, an
  /// unexpected pass, or a skip.
  bool passed() const {
    return outcome == TestOutcome::Passed ||
           outcome == TestOutcome::ExpectedFailure;
  }
};

/// The whole run. `ok()` is the single verdict a caller (or an exit code)
/// should use.
struct BERRY_API ScriptTestReport {
  std::vector<ScriptTestResult> results;
  size_t passed = 0;
  size_t failed = 0;
  size_t skipped = 0;
  size_t expectedFailures = 0;
  /// True when the source loaded and the exports were enumerated. A discovery
  /// failure leaves `results` empty and `error` set.
  bool discoveryOk = false;
  std::string error;

  size_t total() const { return results.size(); }
  bool ok() const { return discoveryOk && failed == 0; }
};

/// Knobs for a run.
struct BERRY_API ScriptTestOptions {
  /// Substring match on the discovered name; empty selects every test.
  std::string filter;
  /// Upper bound on tests executed, so a runaway export table cannot make one
  /// invocation unbounded.
  size_t maxTests = 4096;
  /// Run `on_load` / `before_each` / `after_each` around each test when the
  /// script exports them.
  bool runFixtures = true;
};

//----------------------------------------------------------------------------
// Runner.
//----------------------------------------------------------------------------

/// Discovers and executes a script's tests. Holds no host between calls: each
/// test is loaded into its own VM, so the runner is stateless and reentrant.
class BERRY_API ScriptTestRunner {
 public:
  using VmFactory = std::function<std::unique_ptr<IScriptVm>()>;

  /// `bindings` is the surface a test may call. Pass the client's table (the
  /// IDL surface plus the async bindings) so a test exercises the real API; a
  /// null pointer uses an internal table carrying only the assertion surface.
  /// The assertion bindings are installed onto whichever table is used, once.
  ScriptTestRunner(VmFactory vmFactory, const CapabilitySet& granted,
                   ScriptBindings* bindings = nullptr,
                   ScriptLimits limits = ScriptLimits());

  /// The discovered test names in `source`, sorted. On a load failure returns
  /// empty and sets `error` (when non-null).
  std::vector<std::string> ListTests(const std::string& source,
                                     const std::string& chunkName,
                                     std::string* error = nullptr);

  /// Runs the discovered tests. `chunkName` names the script in diagnostics.
  ScriptTestReport Run(const std::string& source, const std::string& chunkName,
                       const ScriptTestOptions& opts = ScriptTestOptions());

  /// True when `name` follows the discovery convention. Exposed so a caller can
  /// filter a listing without loading anything.
  static bool IsTestName(const std::string& name);

 private:
  std::vector<std::string> Discover(const std::string& source,
                                    const std::string& chunkName,
                                    std::string* error);
  ScriptTestResult RunOne(const std::string& name, const std::string& source,
                          const std::string& chunkName,
                          const ScriptTestOptions& opts);

  VmFactory vmFactory_;
  CapabilitySet granted_;
  ScriptBindings ownedBindings_;
  ScriptBindings* bindings_ = nullptr;
  ScriptLimits limits_;
};

}  // namespace Berry
