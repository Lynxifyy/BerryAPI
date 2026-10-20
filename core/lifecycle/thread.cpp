#include "berry/thread.h"

namespace Berry {

const char* ThreadRoleName(ThreadRole r) {
  switch (r) {
    case ThreadRole::Unknown: return "unknown";
    case ThreadRole::GameTick: return "game-tick";
    case ThreadRole::Render: return "render";
    case ThreadRole::Scripts: return "scripts";
    case ThreadRole::Detach: return "detach";
  }
  return "unknown";
}

namespace {
thread_local ThreadRole g_role = ThreadRole::Unknown;

bool AssertionsEnabled() {
#ifdef BERRY_THREAD_ASSERT
  return true;
#else
  return false;
#endif
}
}  // namespace

ThreadModel& ThreadModel::Instance() {
  static ThreadModel tm;
  return tm;
}

ThreadRole ThreadModel::GetRole() { return g_role; }
void ThreadModel::SetRole(ThreadRole role) { g_role = role; }

void ThreadModel::Enter(ThreadRole role) {
  SetRole(role);
}

void ThreadModel::Leave() {
  SetRole(ThreadRole::Unknown);
}

ThreadRole ThreadModel::CurrentRole() const {
  return GetRole();
}

bool ThreadModel::Is(ThreadRole role) const {
  return GetRole() == role;
}

#define BERRY_ROLE_CHECK(cond, what)                                       \
  do {                                                                     \
    if (!(cond)) {                                                         \
      Log::Error("thread model violation: %s (current=%s)", what,          \
                 ThreadRoleName(GetRole()));                               \
      if (AssertionsEnabled()) __builtin_trap();                           \
    }                                                                      \
  } while (0)

void ThreadModel::AssertTick() const {
  BERRY_ROLE_CHECK(GetRole() == ThreadRole::GameTick, "expected game-tick thread");
}

void ThreadModel::AssertRender() const {
  BERRY_ROLE_CHECK(GetRole() == ThreadRole::Render, "expected render thread");
}

void ThreadModel::AssertScripts() const {
  BERRY_ROLE_CHECK(GetRole() == ThreadRole::Scripts, "expected script thread");
}

void ThreadModel::AssertNotRender() const {
  BERRY_ROLE_CHECK(GetRole() != ThreadRole::Render, "must not run on render thread");
}

#undef BERRY_ROLE_CHECK

}  // namespace Berry