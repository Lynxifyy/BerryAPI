// Host tests for crash handling + safe mode (improvements 87, 88).
//
// The handler itself is exercised in a subprocess (a real SIGSEGV), so the
// parent test can assert the report was written and the counter bumped without
// dying. The safe-mode counter logic is tested directly.

#include "berry/crash.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

int main() {
  const std::string report = "/tmp/berry_test_crash_report.txt";
  const std::string counter = "/tmp/berry_startup_count";
  remove(report.c_str());
  remove(counter.c_str());

  // --- safe mode flag ---
  {
    CHECK(!SafeModeForced());
    ForceSafeMode(true);
    CHECK(SafeModeForced());
    ForceSafeMode(false);
    CHECK(!SafeModeForced());
  }

  // --- startup counter: a crash before healthy increments it ---
  {
    // First start: no counter file yet -> 0 previous crashes.
    CHECK(BeginStartup() == 0);
    // Marking healthy resets it.
    MarkStartupHealthy();
    CHECK(BeginStartup() == 0);
  }

  // --- the handler writes a report on a real fault (subprocess) ---
  {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      // Child: install the handler and take a real SIGSEGV. `raise` delivers
      // the signal to this process (exercising the handler end to end) without
      // an actual invalid access, so UBSan does not flag the test itself.
      CrashHandlerConfig cfg;
      cfg.reportPath = report;
      InstallCrashHandler(cfg);
      raise(SIGSEGV);
      _exit(0);  // not reached: the handler re-raises and the process dies
    }
    int status = 0;
    waitpid(pid, &status, 0);
    // The child died from SIGSEGV (the handler re-raises it).
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGSEGV);

    // The report exists and names the signal.
    FILE* f = fopen(report.c_str(), "r");
    CHECK(f != nullptr);
    if (f) {
      char buf[256] = {0};
      size_t n = fread(buf, 1, sizeof buf - 1, f);
      (void)n;
      fclose(f);
      CHECK(std::string(buf).find("SIGSEGV") != std::string::npos ||
            std::string(buf).find("signal") != std::string::npos);
    }

    // The crash counter is now nonzero, so the next boot should enter safe mode.
    CHECK(BeginStartup() >= 1);
  }

  remove(report.c_str());
  remove(counter.c_str());
  if (g_fail == 0) printf("test_crash: ALL OK\n");
  return g_fail;
}
