// Script test framework + runner. See include/berry/scripttest.h.
#include "berry/scripttest.h"

#include "berry/event.h"    // NowNs
#include "berry/logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Berry {

//----------------------------------------------------------------------------
// Outcome names
//----------------------------------------------------------------------------

const char* TestOutcomeName(TestOutcome o) {
  switch (o) {
    case TestOutcome::Passed: return "passed";
    case TestOutcome::Failed: return "failed";
    case TestOutcome::Skipped: return "skipped";
    case TestOutcome::ExpectedFailure: return "xfail";
    case TestOutcome::UnexpectedPass: return "unexpected-pass";
  }
  return "?";
}

//----------------------------------------------------------------------------
// Assertion surface
//----------------------------------------------------------------------------

namespace {

/// Renders a value for an assertion message. A string is quoted so an empty
/// string and a missing value are distinguishable in the output.
std::string Show(const ScriptValue& v) {
  if (v.IsString()) return "'" + v.AsString() + "'";
  return v.ToDisplay();
}

/// Fails the current test with `msg`. The VM turns the raised error into a
/// script failure, which the host's error isolation reports as a fault; the
/// runner reads the message back off the host.
void RaiseFail(ScriptContext& ctx, const std::string& msg) { ctx.RaiseError(msg); }

}  // namespace

size_t ScriptTestBindings::Install(ScriptBindings* bindings) {
  if (!bindings) return 0;
  size_t added = 0;

  // A binding that needs no capability must say so explicitly: the gate denies
  // an unset requirement, and Capability::NoCap() is that declaration. A name
  // already registered is left untouched so a second install cannot shadow it.
  auto reg = [&](const char* name, ScriptNativeFn fn) {
    if (bindings->Has(name)) return;
    bindings->Register(name, Capability::NoCap(), std::move(fn));
    ++added;
  };

  // assert(cond, message?) — the message is optional; a missing one still
  // names the failure so the output is not a bare "assertion failed".
  reg(ScriptTestBindings::Assert(),
      [](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
        const bool ok = !args.empty() && args[0].Truthy();
        if (!ok) {
          const std::string msg =
              args.size() > 1 && args[1].IsString() ? args[1].AsString()
                                                    : "assertion failed";
          RaiseFail(ctx, msg);
        }
        return ScriptValue::Nil();
      });

  reg(ScriptTestBindings::Truthy(),
      [](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
        if (args.empty() || !args[0].Truthy()) {
          RaiseFail(ctx, "expected a truthy value, got " +
                        Show(args.empty() ? ScriptValue::Nil() : args[0]));
        }
        return ScriptValue::Nil();
      });

  reg(ScriptTestBindings::Falsy(),
      [](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
        if (args.empty() || args[0].Truthy()) {
          RaiseFail(ctx, "expected a falsy value, got " +
                        Show(args.empty() ? ScriptValue::Nil() : args[0]));
        }
        return ScriptValue::Nil();
      });

  reg(ScriptTestBindings::NotNil(),
      [](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
        if (args.empty() || args[0].IsNil()) RaiseFail(ctx, "expected a value, got nil");
        return ScriptValue::Nil();
      });

  // eq(a, b, message?) — equality uses ScriptValue::operator==, which compares
  // tables by identity (like Luau) and numbers by value. So `eq(1, 1.0)` passes
  // and two distinct empty tables do not, which is the honest reading of the
  // same comparison a script's own `==` would make.
  reg(ScriptTestBindings::Eq(),
      [](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
        const ScriptValue a = args.size() > 0 ? args[0] : ScriptValue::Nil();
        const ScriptValue b = args.size() > 1 ? args[1] : ScriptValue::Nil();
        if (a == b) return ScriptValue::Nil();
        std::string msg = "expected " + Show(a) + " == " + Show(b);
        if (args.size() > 2 && args[2].IsString() && !args[2].AsString().empty()) {
          msg = args[2].AsString() + " (" + msg + ")";
        }
        RaiseFail(ctx, msg);
        return ScriptValue::Nil();
      });

  reg(ScriptTestBindings::Ne(),
      [](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
        const ScriptValue a = args.size() > 0 ? args[0] : ScriptValue::Nil();
        const ScriptValue b = args.size() > 1 ? args[1] : ScriptValue::Nil();
        if (!(a == b)) return ScriptValue::Nil();
        RaiseFail(ctx, "expected " + Show(a) + " ~= " + Show(b));
        return ScriptValue::Nil();
      });

  // near(a, b, eps?) — numeric closeness for float results, where `eq` would
  // be brittle. A non-number argument is a failure, never a silent 0.
  reg(ScriptTestBindings::Near(),
      [](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
        if (args.size() < 2 || !args[0].IsNumber() || !args[1].IsNumber()) {
          RaiseFail(ctx, "near() needs two numbers");
          return ScriptValue::Nil();
        }
        const f64 a = args[0].AsNumber();
        const f64 b = args[1].AsNumber();
        const f64 eps = args.size() > 2 && args[2].IsNumber() ? args[2].AsNumber()
                                                              : 1e-9;
        if (!std::isfinite(eps) || eps < 0) {
          RaiseFail(ctx, "near() epsilon must be a non-negative finite number");
          return ScriptValue::Nil();
        }
        if (std::fabs(a - b) <= eps) return ScriptValue::Nil();
        RaiseFail(ctx, "expected " + Show(args[0]) + " near " + Show(args[1]) +
                      " (within " + std::to_string(eps) + ")");
        return ScriptValue::Nil();
      });

  // fail(message?) — an explicit failure, for a branch that should be
  // unreachable. Distinct from `assert(false)`: it always fails.
  reg(ScriptTestBindings::Fail(),
      [](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
        const std::string msg =
            !args.empty() && args[0].IsString() ? args[0].AsString()
                                                : "explicit failure";
        RaiseFail(ctx, msg);
        return ScriptValue::Nil();
      });

  return added;
}

//----------------------------------------------------------------------------
// Discovery
//----------------------------------------------------------------------------

namespace {

const char* const kTestPrefix = "test_";
const char* const kXfailPrefix = "xfail_";
const char* const kSkipPrefix = "skip_";

/// Strips a leading `test_` when present, so both `test_foo` and a namespaced
/// `tests.test_foo` are handled the same way.
std::string StripTestPrefix(const std::string& leaf) {
  const std::string p = kTestPrefix;
  if (leaf.compare(0, p.size(), p) == 0) return leaf.substr(p.size());
  return leaf;
}

/// True when `name` matches the discovery convention. A top-level function is
/// `test_*`, `xfail_*` or `skip_*`; a namespaced function is the same leaf
/// under the `tests` table (`tests.<leaf>`). The leaf must carry a name after
/// the prefix, so a bare `test_` is not a test.
bool MatchesConvention(const std::string& name) {
  const size_t dot = name.find('.');
  std::string leaf;
  if (dot == std::string::npos) {
    leaf = name;
  } else {
    if (name.substr(0, dot) != "tests") return false;
    leaf = name.substr(dot + 1);
    if (leaf.find('.') != std::string::npos) return false;   // one level only
  }
  for (const char* p : {kTestPrefix, kXfailPrefix, kSkipPrefix}) {
    const size_t n = std::strlen(p);
    if (leaf.compare(0, n, p) == 0 && leaf.size() > n) return true;
  }
  return false;
}

}  // namespace

bool ScriptTestRunner::IsTestName(const std::string& name) {
  return MatchesConvention(name);
}

ScriptTestRunner::ScriptTestRunner(VmFactory vmFactory,
                                   const CapabilitySet& granted,
                                   ScriptBindings* bindings, ScriptLimits limits)
    : vmFactory_(std::move(vmFactory)),
      granted_(granted),
      bindings_(bindings ? bindings : &ownedBindings_),
      limits_(limits) {
  ScriptTestBindings::Install(bindings_);
}

std::vector<std::string> ScriptTestRunner::Discover(const std::string& source,
                                                    const std::string& chunkName,
                                                    std::string* error) {
  std::vector<std::string> found;
  if (!vmFactory_) {
    if (error) *error = "no VM factory configured";
    return found;
  }
  std::unique_ptr<IScriptVm> vm = vmFactory_();
  if (!vm) {
    if (error) *error = "VM factory returned null";
    return found;
  }
  ScriptError err;
  if (!vm->Load(source, chunkName, &err)) {
    if (error) *error = err.message.empty() ? "load failed" : err.message;
    return found;
  }
  for (const std::string& name : vm->FunctionNames()) {
    if (MatchesConvention(name)) found.push_back(name);
  }
  std::sort(found.begin(), found.end());
  found.erase(std::unique(found.begin(), found.end()), found.end());
  if (error) error->clear();
  return found;
}

std::vector<std::string> ScriptTestRunner::ListTests(const std::string& source,
                                                     const std::string& chunkName,
                                                     std::string* error) {
  return Discover(source, chunkName, error);
}

//----------------------------------------------------------------------------
// Execution
//----------------------------------------------------------------------------

namespace {

/// The verdict a test name declares, before it runs.
enum class Marker { Normal, Expected, Skipped };

Marker MarkerFor(const std::string& name) {
  const size_t dot = name.find('.');
  const std::string leaf = dot == std::string::npos ? name : name.substr(dot + 1);
  const std::string body = StripTestPrefix(leaf);
  if (body.compare(0, 6, "xfail_") == 0) return Marker::Expected;
  if (body.compare(0, 5, "skip_") == 0) return Marker::Skipped;
  return Marker::Normal;
}

}  // namespace

ScriptTestResult ScriptTestRunner::RunOne(const std::string& name,
                                          const std::string& source,
                                          const std::string& chunkName,
                                          const ScriptTestOptions& opts) {
  ScriptTestResult result;
  result.name = name;
  const Marker marker = MarkerFor(name);
  if (marker == Marker::Skipped) {
    result.outcome = TestOutcome::Skipped;
    return result;
  }

  const u64 start = NowNs();

  // A fresh host per test: a fault unloads the host, so a shared host would let
  // one failed assertion take the rest of the suite down. The load is
  // deterministic and cheap relative to the failure mode it removes.
  std::unique_ptr<IScriptVm> vm = vmFactory_ ? vmFactory_() : nullptr;
  ScriptHost host(name, granted_, std::move(vm));
  host.SetLimits(limits_);
  host.SetBindings(bindings_);

  ScriptError err;
  if (!host.Load(source, chunkName, &err)) {
    result.outcome = TestOutcome::Failed;
    result.message = err.message.empty() ? "load failed" : err.message;
    result.elapsedMs = (NowNs() - start) / 1000000ULL;
    return result;
  }

  // Fixtures, then the test. A fault anywhere in the chain ends the test.
  // `requireResult` is false for a fixture (a missing hook is fine) and true
  // for the test itself. A suspend at an await is a failure: the runner drives
  // no scheduler, so a test that awaits would otherwise be reported as passed
  // while it is still suspended.
  auto run = [&](const char* hook, bool requireResult) -> bool {
    const ScriptStatus st = host.RunHook(hook);
    if (st == ScriptStatus::Ok) return true;
    if (!requireResult && st == ScriptStatus::NoFunction) return true;
    if (result.message.empty()) {
      const ScriptError& e = host.lastError();
      if (st == ScriptStatus::Yielded) {
        result.message = std::string(hook) +
                         " suspended at an await (the runner pumps no scheduler)";
      } else {
        result.message = e.message.empty() ? std::string(ScriptStatusName(st))
                                           : e.message;
      }
    }
    return false;
  };

  bool ok = true;
  if (opts.runFixtures) ok = run(ScriptHooks::OnLoad(), false);
  if (ok && opts.runFixtures) ok = run(ScriptTestHooks::BeforeEach(), false);
  if (ok) ok = run(name.c_str(), true);
  // after_each runs on the same host (it observes what before_each set up) and
  // only when the host is still loaded: a fault unloaded it, so there is
  // nothing left to clean up on. It never overwrites a recorded failure.
  if (opts.runFixtures && host.loaded()) run(ScriptTestHooks::AfterEach(), false);

  result.elapsedMs = (NowNs() - start) / 1000000ULL;

  if (marker == Marker::Expected) {
    result.outcome = ok ? TestOutcome::UnexpectedPass : TestOutcome::ExpectedFailure;
    if (result.outcome == TestOutcome::UnexpectedPass) {
      result.message = "marked xfail but passed";
    }
    return result;
  }
  result.outcome = ok ? TestOutcome::Passed : TestOutcome::Failed;
  return result;
}

ScriptTestReport ScriptTestRunner::Run(const std::string& source,
                                       const std::string& chunkName,
                                       const ScriptTestOptions& opts) {
  ScriptTestReport report;
  std::string error;
  const std::vector<std::string> tests = Discover(source, chunkName, &error);
  if (!error.empty()) {
    report.discoveryOk = false;
    report.error = error;
    return report;
  }
  report.discoveryOk = true;

  size_t executed = 0;
  for (const std::string& name : tests) {
    if (!opts.filter.empty() && name.find(opts.filter) == std::string::npos) {
      continue;
    }
    if (executed >= opts.maxTests) {
      Log::Warn("ScriptTestRunner: maxTests (%zu) reached; %zu test(s) not run",
                opts.maxTests, tests.size() - executed);
      break;
    }
    ++executed;
    ScriptTestResult r = RunOne(name, source, chunkName, opts);
    switch (r.outcome) {
      case TestOutcome::Passed: ++report.passed; break;
      case TestOutcome::Failed: ++report.failed; break;
      case TestOutcome::Skipped: ++report.skipped; break;
      case TestOutcome::ExpectedFailure: ++report.expectedFailures; break;
      case TestOutcome::UnexpectedPass: ++report.failed; break;
    }
    report.results.push_back(std::move(r));
  }
  return report;
}

}  // namespace Berry
