#pragma once
// Crash handling and safe mode (improvements 87, 88).
//
// A fault inside the injected process must not vanish: this installs a handler
// on an alternate stack that writes a backtrace with module-relative offsets
// and the last log lines to app-private storage, so a field crash is
// diagnosable. It also records a consecutive-startup-crash counter so the next
// boot can come up in safe mode (hooks and scripts disabled).

#include "berry/export.h"
#include "berry/types.h"

#include <string>

namespace Berry {

/// Crash-handler configuration.
struct BERRY_API CrashHandlerConfig {
  /// Where to write a crash report (a file). Empty uses a default under the
  /// temp dir. The directory must already exist and be writable.
  std::string reportPath;
  /// Also write to stderr (a host that captures it). Off by default.
  bool toStderr = false;
  /// How many frames of backtrace to capture.
  int maxFrames = 32;
};

/// Installs the SIGSEGV/SIGBUS/SIGABRT/SIGILL/SIGFPE handler on an alternate
/// signal stack. Idempotent. Returns false when the alt stack or the handler
/// could not be installed.
BERRY_API bool InstallCrashHandler(const CrashHandlerConfig& cfg = {});

/// The path the last report would be (or was) written to.
BERRY_API const std::string& CrashReportPath();

/// Records that a startup began, and returns how many consecutive starts have
/// crashed without a matching success. A nonzero result means the previous
/// start(s) did not reach `MarkStartupHealthy`, so the caller should boot in
/// safe mode (improvement 88).
BERRY_API int BeginStartup();

/// Marks the current startup as healthy (the attach and the first frames
/// survived), resetting the crash counter.
BERRY_API void MarkStartupHealthy();

/// True when safe mode has been forced on for this process (by config or by
/// the crash counter). In safe mode the engine must not install hooks or run
/// scripts.
BERRY_API bool SafeModeForced();
/// Forces safe mode for this process.
BERRY_API void ForceSafeMode(bool on);

}  // namespace Berry
