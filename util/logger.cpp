#include "berry/logger.h"
#include "berry/types.h"
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace Berry {

std::mutex Log::mutex_;
std::string Log::last_message_;
bool Log::capture_ = false;
bool Log::silent_ = false;
std::atomic<int> Log::level_{(int)LogLevel::Info};

namespace {

// Per-module level overrides and rate-limit state (improvement 90). Guarded by
// Log's mutex so a concurrent SetModuleLevel cannot race a read.
struct ModuleState {
  int level = -1;                 // -1 = no override
  u64 windowStartMs = 0;
  u32 emittedThisSecond = 0;
};
std::map<std::string, ModuleState> g_moduleState;

u64 SteadyMs() {
  using namespace std::chrono;
  return (u64)duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

void Log::SetModuleLevel(const std::string& module, LogLevel level) {
  std::lock_guard<std::mutex> lk(mutex_);
  g_moduleState[module].level = (int)level;
}

LogLevel Log::ModuleLevel(const std::string& module) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = g_moduleState.find(module);
  if (it != g_moduleState.end() && it->second.level >= 0) {
    return (LogLevel)it->second.level;
  }
  return GetLevel();
}

void Log::ClearModuleLevels() {
  std::lock_guard<std::mutex> lk(mutex_);
  g_moduleState.clear();
}

void Log::LogTagged(LogLevel level, const char* module, int maxPerSecond,
                    const char* fmt, ...) {
  const std::string mod = module ? module : "";
  // Resolve the effective level and, if a rate limit applies, decide whether
  // this call is allowed. All under the lock; the sink itself runs outside it.
  {
    std::lock_guard<std::mutex> lk(mutex_);
    auto& st = g_moduleState[mod];
    const int effective = st.level >= 0 ? st.level : level_.load();
    if ((int)level < effective) return;
    if (maxPerSecond > 0) {
      const u64 now = SteadyMs();
      if (now - st.windowStartMs >= 1000) {
        st.windowStartMs = now;
        st.emittedThisSecond = 0;
      }
      if (st.emittedThisSecond >= (u32)maxPerSecond) return;   // dropped
      ++st.emittedThisSecond;
    }
  }
  va_list args;
  va_start(args, fmt);
  VFormat(level, fmt, args);
  va_end(args);
}

void Log::SetLevel(LogLevel level) { level_.store((int)level); }
LogLevel Log::GetLevel() { return (LogLevel)level_.load(); }

std::string Log::LastMessage() {
  std::lock_guard<std::mutex> lk(mutex_);
  return last_message_;
}

void Log::Capture(bool enabled) {
  std::lock_guard<std::mutex> lk(mutex_);
  capture_ = enabled;
}

void Log::SetSilent(bool silent) {
  std::lock_guard<std::mutex> lk(mutex_);
  silent_ = silent;
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
  // Format first, WITHOUT the lock. The message is bounded but not truncated at
  // a fixed 1 KiB: a longer line is retried with a bigger buffer, so a long
  // path or a large value is logged whole rather than silently cut (bug 52).
  std::vector<char> buf(1024);
  for (;;) {
    va_list cp;
    va_copy(cp, args);
    const int n = vsnprintf(buf.data(), buf.size(), fmt, cp);
    va_end(cp);
    if (n < 0) {
      buf[0] = '\0';
      break;
    }
    if ((size_t)n < buf.size()) break;   // it fit
    buf.resize((size_t)n + 1);           // retry with room for the whole line
  }
  const char* msg = buf.data();

  // Publish the line and read the output flags under the lock, then do the
  // actual I/O OUTSIDE it: a slow sink must not block every other logger (bug
  // 52).
  bool silent = false;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    silent = silent_;
    if (capture_) last_message_ = msg;
  }
  if (silent) return;

#ifdef __ANDROID__
  android_LogPriority prio = ANDROID_LOG_INFO;
  switch (level) {
    case LogLevel::Trace: prio = ANDROID_LOG_VERBOSE; break;
    case LogLevel::Debug: prio = ANDROID_LOG_DEBUG; break;
    case LogLevel::Warn:  prio = ANDROID_LOG_WARN; break;
    case LogLevel::Error: prio = ANDROID_LOG_ERROR; break;
    default: break;
  }
  __android_log_print(prio, "Berry", "%s", msg);
#else
  fprintf(stderr, "[Berry][%s] %s\n", LevelName(level), msg);
#endif
}

void Log::LogAt(LogLevel level, const char* fmt, ...) {
  if ((int)level < level_.load()) return;
  va_list args;
  va_start(args, fmt);
  VFormat(level, fmt, args);
  va_end(args);
}

void Log::Trace(const char* fmt, ...) {
  if ((int)LogLevel::Trace < level_.load()) return;
  va_list args; va_start(args, fmt); VFormat(LogLevel::Trace, fmt, args); va_end(args);
}
void Log::Debug(const char* fmt, ...) {
  if ((int)LogLevel::Debug < level_.load()) return;
  va_list args; va_start(args, fmt); VFormat(LogLevel::Debug, fmt, args); va_end(args);
}
void Log::Info(const char* fmt, ...) {
  if ((int)LogLevel::Info < level_.load()) return;
  va_list args; va_start(args, fmt); VFormat(LogLevel::Info, fmt, args); va_end(args);
}
void Log::Warn(const char* fmt, ...) {
  if ((int)LogLevel::Warn < level_.load()) return;
  va_list args; va_start(args, fmt); VFormat(LogLevel::Warn, fmt, args); va_end(args);
}
void Log::Error(const char* fmt, ...) {
  if ((int)LogLevel::Error < level_.load()) return;
  va_list args; va_start(args, fmt); VFormat(LogLevel::Error, fmt, args); va_end(args);
}

}  // namespace Berry
