/* Host-side dlopen smoke test for libBerryAPI.so.
 * Verifies the exported C ABI (BERRY_Inject/BERRY_Detach/BERRY_IsAttached)
 * and that the library loads cleanly as an injectable module.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "build/libBerryAPI.so";
  void* h = dlopen(path, RTLD_NOW);
  CHECK(h != NULL);
  if (!h) {
    fprintf(stderr, "dlopen: %s\n", dlerror());
    return 1;
  }

  const char* (*version)(void) = (const char* (*)(void))dlsym(h, "BERRY_Version");
  int (*isAttached)(void) = (int (*)(void))dlsym(h, "BERRY_IsAttached");
  int (*inject)(void) = (int (*)(void))dlsym(h, "BERRY_Inject");
  void (*detach)(void) = (void (*)(void))dlsym(h, "BERRY_Detach");
  CHECK(version && isAttached && inject && detach);
  if (!version || !isAttached || !inject || !detach) {
    fprintf(stderr, "dlsym: %s\n", dlerror());
    return 1;
  }

  printf("BERRY_Version() = %s\n", version());
  // libBerryAPI.so is built with the auto-attach constructor, so a dlopen
  // already injected it. Both states are valid for the inject path; the
  // important invariants are: Inject is safe/idempotent, Detach works, and
  // re-inject works.
  int wasAttached = isAttached();
  CHECK(inject() == 1);              // idempotent safe-call
  CHECK(isAttached() == 1);
  detach();
  CHECK(isAttached() == 0);
  CHECK(inject() == 1);              // re-inject after detach
  CHECK(isAttached() == 1);
  printf("(auto-attached on load: %s)\n", wasAttached ? "yes" : "no");

  if (g_fail == 0) printf("dlopen_test: ALL OK\n");
  dlclose(h);
  return g_fail;
}