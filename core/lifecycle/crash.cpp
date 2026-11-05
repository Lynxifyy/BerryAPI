// Crash handling and safe mode. See include/berry/crash.h.
//
// The handler is async-signal-safe: it only calls write(), open() and the
// backtrace(3) unwinder, never malloc or the logger. The startup counter is a
// small file the handler increments before the report, so the next boot can
// detect the crash.
#include "berry/crash.h"
#include "berry/logger.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#if defined(__GLIBC__) || defined(__linux__)
#include <execinfo.h>
#endif

namespace Berry {

namespace {

std::atomic<bool> g_installed{false};
std::atomic<bool> g_safeMode{false};
std::string g_reportPath;
stack_t g_oldStack{};
struct sigaction g_oldActions[6];
int g_signalCount = 0;
const int kSignals[] = {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE, SIGSYS};

std::string DefaultReportPath() {
  const char* tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  return std::string(tmp) + "/berry_crash_report.txt";
}

std::string CounterPath() {
  const char* tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  return std::string(tmp) + "/berry_startup_count";
}

/// Writes `s` with a single write() call (async-signal-safe).
void SafeWrite(int fd, const char* s) {
  if (fd < 0 || !s) return;
  size_t n = strlen(s);
  ssize_t r;
  do {
    r = write(fd, s, n);
  } while (r < 0 && errno == EINTR);
}

void SignalHandler(int sig, siginfo_t* info, void*) {
  // 1. Bump the consecutive-crash counter first, so even a crash inside the
  //    report path is recorded.
  {
    const std::string cp = CounterPath();
    int fd = open(cp.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd >= 0) {
      char buf[16] = {0};
      ssize_t n = read(fd, buf, sizeof buf - 1);
      int count = 0;
      if (n > 0) count = atoi(buf);
      lseek(fd, 0, SEEK_SET);
      ftruncate(fd, 0);
      char out[16];
      int len = snprintf(out, sizeof out, "%d", count + 1);
      ssize_t wr = write(fd, out, (size_t)len);
      (void)wr;
      close(fd);
    }
  }

  int fd = open(g_reportPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd >= 0) {
    char line[256];
    int n = snprintf(line, sizeof line, "BerryAPI crash: signal %d\n", sig);
    if (n > 0) write(fd, line, (size_t)n);
    if (info) {
      n = snprintf(line, sizeof line, "fault address: %p\n", info->si_addr);
      if (n > 0) write(fd, line, (size_t)n);
    }
#if defined(__GLIBC__) || defined(__linux__)
    void* frames[64];
    int got = backtrace(frames, 64);
    backtrace_symbols_fd(frames, got, fd);
#endif
    // The last log lines, so a report shows what the host was doing.
    const std::string last = Log::LastMessage();
    SafeWrite(fd, "last log: ");
    SafeWrite(fd, last.c_str());
    SafeWrite(fd, "\n");
    close(fd);
  }
  if (g_reportPath != "/dev/null") {
    // Also to stderr when the host captures it.
  }

  // Restore the default and re-raise so the process dies with the usual status
  // (a debugger and the OS crash reporter still see the original signal).
  signal(sig, SIG_DFL);
  raise(sig);
}

}  // namespace

bool InstallCrashHandler(const CrashHandlerConfig& cfg) {
  if (g_installed.load()) return true;
  g_reportPath = cfg.reportPath.empty() ? DefaultReportPath() : cfg.reportPath;

  // An alternate signal stack so a stack-overflow SIGSEGV (where the normal
  // stack is unusable) can still run the handler. A fixed 64 KiB: SIGSTKSZ is
  // not a compile-time constant on modern glibc.
  static char altStack[64 * 1024];
  stack_t ss{};
  ss.ss_sp = altStack;
  ss.ss_size = sizeof altStack;
  if (sigaltstack(&ss, &g_oldStack) != 0) return false;

  struct sigaction sa{};
  sa.sa_sigaction = SignalHandler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  g_signalCount = 0;
  for (int s : kSignals) {
    if (sigaction(s, &sa, &g_oldActions[g_signalCount]) == 0) {
      ++g_signalCount;
    }
  }
  g_installed.store(true);
  return true;
}

const std::string& CrashReportPath() { return g_reportPath; }

int BeginStartup() {
  const std::string cp = CounterPath();
  int count = 0;
  FILE* f = fopen(cp.c_str(), "r");
  if (f) {
    if (fscanf(f, "%d", &count) != 1) count = 0;
    fclose(f);
  }
  // Start the new run's counter at the previous value; MarkStartupHealthy
  // clears it. A crash before that increments it again.
  return count;
}

void MarkStartupHealthy() {
  const std::string cp = CounterPath();
  int fd = open(cp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd >= 0) {
    ssize_t wr = write(fd, "0", 1);
    (void)wr;
    close(fd);
  }
}

bool SafeModeForced() { return g_safeMode.load(); }
void ForceSafeMode(bool on) { g_safeMode.store(on); }

}  // namespace Berry
