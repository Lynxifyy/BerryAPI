// Host tests for hook chains and RAII hook groups (improvements 8, 9, 10).
//
// The targets are ordinary functions defined in this binary, so the detour
// (the chain dispatcher, also in this binary) is within the +/-2 GiB a 5-byte
// `jmp rel32` needs.
#include "berry/hook_chain.h"
#include "berry/hooks.h"

#include <cstdio>
#include <memory>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// Targets: real functions in this image. `noinline` gives each a stable body,
// and the leading NOP pad gives the prologue at least five non-terminal bytes
// so the shim will patch it (a bare `lea; ret` is only four bytes and is
// correctly refused).
#define PROLOGUE_PAD asm volatile("nop; nop; nop; nop; nop")
__attribute__((noinline)) int AddFn(int a, int b) { PROLOGUE_PAD; return a + b; }
__attribute__((noinline)) int MulFn(int a, int b) { PROLOGUE_PAD; return a * b; }
__attribute__((noinline)) int SubFn(int a, int b) { PROLOGUE_PAD; return a - b; }
// A lone `ret`: the shim must refuse to patch it (bug 3).
__attribute__((noinline)) int RetOnlyFn(int, int) { return 0; }

int main() {
  if (!BERRY_ShimCanPatch()) {
    printf("test_hook_chain: shim unavailable; skipped\n");
    return 0;
  }
  HookManager::Instance().BackendEmulateShim();
  IHook* backend = HookManager::Instance().Backend();
  CHECK(backend && backend->CanHook());

  CHECK(AddFn(2, 3) == 5);

  // --- a chain of two handlers composes in priority order ---
  {
    HookChain<int (*)(int, int)> chain(backend, (void*)&AddFn, "add");
    CHECK(chain.installed());

    HookHandlerOptions lo; lo.priority = 0; lo.name = "plus100";
    CHECK(chain.Add("plus100",
                    [](HookChain<int (*)(int, int)>::Next next, int a, int b) {
                      return next(a, b) + 100;
                    },
                    lo));
    HookHandlerOptions hi; hi.priority = 10; hi.name = "double";
    CHECK(chain.Add("double",
                    [](HookChain<int (*)(int, int)>::Next next, int a, int b) {
                      return next(a * 2, b * 2);
                    },
                    hi));
    CHECK(chain.handlerCount() == 2);
    // double runs first: (4 + 6) = 10, then + 100 = 110.
    CHECK(AddFn(2, 3) == 110);
    CHECK(chain.callOriginal(2, 3) == 5);

    // Removing a handler drops it from the chain.
    CHECK(chain.Remove("plus100"));
    CHECK(AddFn(2, 3) == 10);
    chain.Remove();
    CHECK(AddFn(2, 3) == 5);
  }

  // --- a handler that short-circuits never calls the original ---
  {
    HookChain<int (*)(int, int)> chain(backend, (void*)&MulFn, "short");
    CHECK(chain.installed());
    CHECK(chain.Add("deny", [](HookChain<int (*)(int, int)>::Next, int, int) {
      return -1;   // never calls next
    }));
    CHECK(MulFn(2, 3) == -1);
    chain.Remove();
    CHECK(MulFn(2, 3) == 6);
  }

  // --- ScopedHook installs and releases ---
  {
    auto detour = +[](int, int) -> int { return 42; };
    {
      ScopedHook<int (*)(int, int)> sh(backend, (void*)&AddFn, detour);
      CHECK(sh.installed());
      int av = AddFn(2,3);
      CHECK(av == 42);
      sh.Release();
      CHECK(!sh.installed());
      CHECK(AddFn(2, 3) == 5);
    }
  }

  // --- HookGroup rolls back on a partial failure ---
  {
    auto detour = +[](int, int) -> int { return 7; };
    HookGroup<int (*)(int, int)> g;
    // The second target is a lone `ret` (four bytes of padding after it), which
    // the shim refuses because a terminal instruction cannot be patched; the
    // group must roll the first install back.
    std::vector<std::pair<void*, int (*)(int, int)>> bad = {
        {(void*)&AddFn, detour}, {(void*)&RetOnlyFn, detour}};
    size_t failed = 999;
    CHECK(!g.InstallAll(backend, bad, &failed));
    CHECK(failed == 1);
    CHECK(g.count() == 0);
    CHECK(AddFn(2, 3) == 5);   // rolled back

    std::vector<std::pair<void*, int (*)(int, int)>> good = {
        {(void*)&AddFn, detour}, {(void*)&MulFn, detour}};
    CHECK(g.InstallAll(backend, good));
    CHECK(g.count() == 2);
    CHECK(AddFn(2, 3) == 7 && MulFn(2, 3) == 7);
    g.RemoveAll();
    CHECK(AddFn(2, 3) == 5 && MulFn(2, 3) == 6);
  }

  // --- the chain cap is enforced ---
  {
    std::vector<std::unique_ptr<HookChain<int (*)(int, int)>>> chains;
    for (size_t i = 0; i < detail::kMaxHookChains + 4; ++i) {
      auto c = std::make_unique<HookChain<int (*)(int, int)>>(
          backend, (void*)&SubFn, "cap");
      if (c->installed()) chains.push_back(std::move(c));
    }
    // Only one chain can own the single backend slot for SubFn; the rest fail.
    CHECK(chains.size() <= 1);
  }

  if (g_fail == 0) printf("test_hook_chain: ALL OK\n");
  return g_fail;
}
