#pragma once
#include "berry/export.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>

namespace Berry {

enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error };

// Log sink abstraction. The default sink writes to stderr; on Android it is
// replaced with __android_log_print, preserving the same line format.
class BERRY_API ILogSink {
 public:
  virtual ~ILogSink() = default;
  /// Receives a pre-formatted, null-terminated line (no trailing newline).
  virtual void write(LogLevel level, const char* line) = 0;
};

class BERRY_API Log {
 public:
  static void SetSink(ILogSink* sink);
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

 private:
  static void VFormat(LogLevel level, const char* fmt, va_list args);
  static ILogSink* sink_;
  static std::mutex mutex_;
  static std::string last_message_;
  static bool capture_;
  static LogLevel level_;
};

}  // namespace Berry