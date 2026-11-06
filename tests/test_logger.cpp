// Host tests for the logger ring buffer, JSON lines and per-module levels
// (bug 52, improvement 90).
#include "berry/logger.h"
#include <cstdio>
#include <string>
using namespace Berry;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
int main() {
  Log::SetSilent(true);      // keep stderr quiet in the test
  Log::SetLevel(LogLevel::Trace);
  Log::ClearRecent();

  // The ring captures recent lines with their level.
  Log::Info("hello %d", 1);
  Log::Warn("careful");
  Log::Error("boom");
  auto entries = Log::RecentEntries(10);
  CHECK(entries.size() == 3);
  CHECK(entries[0].level == LogLevel::Info);
  CHECK(entries[0].text == "hello 1");
  CHECK(entries[1].level == LogLevel::Warn);
  CHECK(entries[2].level == LogLevel::Error);
  CHECK(entries[2].text == "boom");

  // Recent() with a cap returns only the newest.
  auto last2 = Log::Recent(2);
  CHECK(last2.size() == 2);
  CHECK(last2[1] == "boom");

  // The ring is bounded.
  for (int i = 0; i < 1000; ++i) Log::Info("line %d", i);
  CHECK(Log::RecentEntries(100000).size() <= 512);

  // A long line is not truncated (bug 52).
  Log::ClearRecent();
  std::string big(3000, 'x');
  Log::Info("%s", big.c_str());
  auto e2 = Log::Recent(1);
  CHECK(e2.size() == 1);
  CHECK(e2[0].size() == 3000);

  // Per-module levels quiet one module without the rest.
  Log::ClearModuleLevels();
  Log::SetLevel(LogLevel::Info);
  Log::SetModuleLevel("noisy", LogLevel::Error);
  CHECK(Log::ModuleLevel("noisy") == LogLevel::Error);
  CHECK(Log::ModuleLevel("other") == LogLevel::Info);
  Log::ClearRecent();
  // A warn from the quiet module is dropped; from another it is kept.
  Log::LogTagged(LogLevel::Warn, "noisy", 0, "should be dropped");
  Log::LogTagged(LogLevel::Warn, "other", 0, "should be kept");
  auto e3 = Log::RecentEntries(10);
  CHECK(e3.size() == 1);
  CHECK(e3[0].text == "should be kept");

  // Rate limiting drops beyond the cap in a second window.
  Log::ClearModuleLevels();
  Log::SetLevel(LogLevel::Trace);
  Log::ClearRecent();
  for (int i = 0; i < 100; ++i) Log::LogTagged(LogLevel::Info, "hot", 5, "x%d", i);
  CHECK(Log::RecentEntries(1000).size() <= 5);

  Log::ClearModuleLevels();
  Log::ClearRecent();
  if (g_fail == 0) printf("test_logger: ALL OK\n");
  return g_fail;
}
