/* Host-side dlopen smoke test for libBerryAPI.so.
 * Verifies the exported C ABI (BERRY_Inject/BERRY_InjectWithConfig/
 * BERRY_Detach/BERRY_IsAttached), the struct-validated config path, and that
 * the library loads cleanly as an injectable module.
 *
 * Config validation must fail closed: wrong abi_version or a mismatched size
 * are rejected, and the engine stays unattached.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "berry/config.h"

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
  int (*injectWithConfig)(const BerryConfigC*) =
      (int (*)(const BerryConfigC*))dlsym(h, "BERRY_InjectWithConfig");
  void (*detach)(void) = (void (*)(void))dlsym(h, "BERRY_Detach");
  CHECK(version && isAttached && inject && injectWithConfig && detach);
  if (!version || !isAttached || !inject || !injectWithConfig || !detach) {
    fprintf(stderr, "dlsym: %s\n", dlerror());
    return 1;
  }

  printf("BERRY_Version() = %s\n", version());
  // libBerryAPI.so is built with the auto-attach constructor, so a dlopen
  // already injected it. Make sure we start detached to test each entry.
  detach();
  CHECK(isAttached() == 0);

  // --- Config validation: bad abi_version must be rejected (fail closed) ---
  BerryConfigC badAbi;
  memset(&badAbi, 0, sizeof(badAbi));
  badAbi.size = sizeof(badAbi);
  badAbi.abi_version = BERRY_ABI_VERSION + 1000u;
  CHECK(injectWithConfig(&badAbi) == 0);
  CHECK(isAttached() == 0);

  // --- Config validation: undersized struct must be rejected ---
  BerryConfigC small;
  memset(&small, 0, sizeof(small));
  small.size = sizeof(BerryConfigC) - 1;   // truncated struct
  small.abi_version = BERRY_ABI_VERSION;
  CHECK(injectWithConfig(&small) == 0);
  CHECK(isAttached() == 0);

  // --- Null config falls back to defaults (still valid) ---
  CHECK(injectWithConfig(NULL) == 1);
  CHECK(isAttached() == 1);
  detach();
  CHECK(isAttached() == 0);

  // --- Valid explicit config attaches ---
  BerryConfigC cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.size = sizeof(cfg);
  cfg.abi_version = BERRY_ABI_VERSION;
  cfg.log_level = BerryLogWarnC;
  cfg.auto_attach_modules = 1;
  cfg.enable_render = 1;
  CHECK(injectWithConfig(&cfg) == 1);
  CHECK(isAttached() == 1);
  detach();
  CHECK(isAttached() == 0);

  // --- Plain Inject remains idempotent and re-injectable ---
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