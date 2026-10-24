#pragma once
#include "berry/export.h"
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

 private:
  static void VFormat(LogLevel level, const char* fmt, va_list args);
  static std::mutex mutex_;
  static std::string last_message_;
  static bool capture_;
  static LogLevel level_;
};

}  // namespace Berry