// Host tests for the BerryAPI threading model (Phase 0.5): role enter/leave
// routing and the assertion helpers. Assertions in this suite are compiled
// with BERRY_THREAD_ASSERT, but a violated assertion __builtin_trap()s, so we
// only test the *passing* paths here plus CurrentRole routing. The fail path
// is covered indirectly by engine wiring in other suites.
#include "berry/thread.h"
#include <cstdio>
#include <thread>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

int main() {
  ThreadModel& tm = ThreadModel::Instance();

  // Default state.
  CHECK(tm.CurrentRole() == ThreadRole::Unknown);
  CHECK(ThreadRoleName(ThreadRole::Render) != nullptr);

  // GameTick route.
  tm.Enter(ThreadRole::GameTick);
  CHECK(tm.Is(ThreadRole::GameTick));
  tm.AssertTick();          // must pass
  CHECK(!tm.Is(ThreadRole::Render));
  tm.Leave();
  CHECK(tm.CurrentRole() == ThreadRole::Unknown);

  // Render route.
  tm.Enter(ThreadRole::Render);
  CHECK(tm.Is(ThreadRole::Render));
  tm.AssertRender();
  tm.Leave();

  // Script route (a worker thread — state is thread-local).
  std::thread worker([&]() {
    tm.Enter(ThreadRole::Scripts);
    CHECK(tm.Is(ThreadRole::Scripts));
    tm.AssertScripts();
    tm.AssertNotRender();   // scripts are never on the render thread
    tm.Leave();
    CHECK(tm.CurrentRole() == ThreadRole::Unknown);
  });
  worker.join();

  // The main thread is still detached.
  CHECK(tm.CurrentRole() == ThreadRole::Unknown);

  if (g_fail == 0) printf("test_thread: ALL OK\n");
  return g_fail;
}