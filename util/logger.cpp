#include "berry/logger.h"
#include <mutex>
#include <cstdarg>
#include <cstring>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace Berry {

std::mutex Log::mutex_;
std::string Log::last_message_;
bool Log::capture_ = false;
LogLevel Log::level_ = LogLevel::Info;

void Log::SetLevel(LogLevel level) { level_ = level; }
LogLevel Log::GetLevel() { return level_; }

std::string Log::LastMessage() {
  std::lock_guard<std::mutex> lk(mutex_);
  return last_message_;
}

void Log::Capture(bool enabled) {
  std::lock_guard<std::mutex> lk(mutex_);
  capture_ = enabled;
}

static const char* LevelName(LogLevel lv) {
  switch (lv) {
    case LogLevel::Trace: return "T";
    case LogLevel::Debug: return "D";
    case LogLevel::Info:  return "I";
    case LogLevel::Warn:  return "W";
    case LogLevel::Error: return "E";
  }
  return "?";
}

void Log::VFormat(LogLevel level, const char* fmt, va_list args) {
  std::lock_guard<std::mutex> lk(mutex_);
  char buf[1024];
  {
    va_list cp;
    va_copy(cp, args);
    vsnprintf(buf, sizeof(buf), fmt, cp);
    va_end(cp);
  }
#ifdef __ANDROID__
  android_LogPriority prio = ANDROID_LOG_INFO;
  switch (level) {
    case LogLevel::Trace: prio = ANDROID_LOG_VERBOSE; break;
    case LogLevel::Debug: prio = ANDROID_LOG_DEBUG; break;
    case LogLevel::Warn:  prio = ANDROID_LOG_WARN; break;
    case LogLevel::Error: prio = ANDROID_LOG_ERROR; break;
    default: break;
  }
  __android_log_print(prio, "Berry", "%s", buf);
#else
  fprintf(stderr, "[Berry][%s] %s\n", LevelName(level), buf);
#endif
  if (capture_) last_message_ = buf;
}

void Log::LogAt(LogLevel level, const char* fmt, ...) {
  if (level < Log::GetLevel()) return;
  va_list args;
  va_start(args, fmt);
  VFormat(level, fmt, args);
  va_end(args);
}

void Log::Trace(const char* fmt, ...) {
  if (LogLevel::Trace < Log::GetLevel()) return;
  va_list args; va_start(args, fmt); VFormat(LogLevel::Trace, fmt, args); va_end(args);
}
void Log::Debug(const char* fmt, ...) {
  if (LogLevel::Debug < Log::GetLevel()) return;
  va_list args; va_start(args, fmt); VFormat(LogLevel::Debug, fmt, args); va_end(args);
}
void Log::Info(const char* fmt, ...) {
  if (LogLevel::Info < Log::GetLevel()) return;
  va_list args; va_start(args, fmt); VFormat(LogLevel::Info, fmt, args); va_end(args);
}
void Log::Warn(const char* fmt, ...) {
  if (LogLevel::Warn < Log::GetLevel()) return;
  va_list args; va_start(args, fmt); VFormat(LogLevel::Warn, fmt, args); va_end(args);
}
void Log::Error(const char* fmt, ...) {
  if (LogLevel::Error < Log::GetLevel()) return;
  va_list args; va_start(args, fmt); VFormat(LogLevel::Error, fmt, args); va_end(args);
}

}  // namespace Berry