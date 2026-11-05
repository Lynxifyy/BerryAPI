#pragma once
#include "berry/export.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>

namespace Berry {

enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error };

class BERRY_API Log {
 public:
  static void SetLevel(LogLevel level);
  static LogLevel GetLevel();
  static void Trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
  static void Debug(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
  static void Info(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
  static void Warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
  static void Error(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
  static void LogAt(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  static std::string LastMessage();
  static void Capture(bool enabled);

  /// Suppresses the log's own output (stderr / logcat) while still recording the
  /// last message for `LastMessage()`. A host that presents the log itself — the
  /// `berry-run` CLI, which prints a test's failure from the test result rather
  /// than from a duplicated engine line — turns this on so the two do not both
  /// print. Default off; a device build never sets it.
  static void SetSilent(bool silent);

  /// Per-module level override (improvement 90). `module` is the tag the caller
  /// passes to `LogTagged` (e.g. "net", "script"). A level of `LogLevel::Trace`
  /// is the most verbose; set a module's level higher to quiet it without
  /// quieting the rest. Returns the module's effective level.
  static void SetModuleLevel(const std::string& module, LogLevel level);
  static LogLevel ModuleLevel(const std::string& module);
  static void ClearModuleLevels();

  /// Logs with a module tag, applying the module's own level (or the global
  /// one) and a per-module rate limit so a hot loop cannot flood the sink
  /// (improvement 90). `maxPerSecond` of 0 disables the limit for the call.
  static void LogTagged(LogLevel level, const char* module, int maxPerSecond,
                        const char* fmt, ...)
      __attribute__((format(printf, 4, 5)));

 private:
  static void VFormat(LogLevel level, const char* fmt, va_list args);
  static std::mutex mutex_;
  static std::string last_message_;
  static bool capture_;
  static bool silent_;
  /// Atomic: the level is read on every log call (often off the worker thread)
  /// while SetLevel writes it, so a plain int was a data race (bug 52).
  static std::atomic<int> level_;
};

}  // namespace Berry