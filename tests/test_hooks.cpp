// Host tests for the x86-64 shim hotpatch backend.
//
// The shim overwrites the first >=5 bytes of a function with `jmp rel32` and
// relocates the overwritten whole instructions into a trampoline. The failure
// modes that matter are pinned here with hand-assembled code (so the test does
// not depend on what the compiler emits):
//
//   * A prologue whose rel32 field is not the last thing in its instruction
//     (`cmp dword [rip+x], imm8`) must be rebased against the instruction end,
//     not the field end, or the trampoline reads the wrong address (bug 1).
//   * The prologue walk must stop at a terminal instruction (bug 3), or a patch
//     overruns a short function into its neighbour.
//   * A branch from inside the overwritten span into its middle is refused
//     (bug 4).
//   * Unpatch must not clobber a prologue that no longer holds our jump (bug 9).
//
// The target, the detour and the RIP-referenced global all live in one RWX page
// so every `jmp rel32` and every RIP displacement fits in 32 bits — exactly the
// layout the shim is designed for (target and detour in the same module).

#include "berry/hooks.h"
#include "berry/types.h"

#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

constexpr size_t kFnOffset = 0;        // the function under test
constexpr size_t kDetourOffset = 128;  // `mov eax,42; ret`
constexpr size_t kGlobalOffset = 192;  // the RIP-referenced int

/// A read/write/execute scratch page holding the function, its detour and the
/// global the RIP-relative test reads.
struct CodePage {
  u8* mem = nullptr;
  size_t cap = 0;

  CodePage() {
    cap = (size_t)sysconf(_SC_PAGESIZE);
    mem = (u8*)mmap(nullptr, cap, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) mem = nullptr;
    if (mem) {
      // Detour: mov eax, 42 ; ret
      const u8 detour[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
      memcpy(mem + kDetourOffset, detour, sizeof detour);
    }
  }
  ~CodePage() {
    if (mem) munmap(mem, cap);
  }
  bool ok() const { return mem != nullptr; }

  u8* fn() const { return mem + kFnOffset; }
  void* detour() const { return mem + kDetourOffset; }
  int* global() const { return (int*)(mem + kGlobalOffset); }

  u8* Emit(const std::vector<u8>& bytes) {
    memcpy(fn(), bytes.data(), bytes.size());
    return fn();
  }
};

}  // namespace

int main() {
  if (!BERRY_ShimCanPatch()) {
    printf("test_hooks: shim cannot patch on this host; skipping\n");
    return 0;
  }

  // --- bug 3: a one-byte `ret` function must not be patched --------------
  {
    CodePage page;
    CHECK(page.ok());
    u8* fn = page.Emit({0xC3});   // ret
    CHECK(BERRY_ShimPatchInPlace(fn, page.detour()) == nullptr);
  }

  // --- bug 3: a terminal before five bytes is refused --------------------
  {
    CodePage page;
    CHECK(page.ok());
    // nop; ret -> only two bytes of fall-through; refuse rather than read the
    // next function's bytes.
    u8* fn = page.Emit({0x90, 0xC3});
    CHECK(BERRY_ShimPatchInPlace(fn, page.detour()) == nullptr);
  }

  // --- bug 4: a branch from the span into its own middle is refused ------
  {
    CodePage page;
    CHECK(page.ok());
    // jmp rel32 with disp = -4: from offset 5 back to offset 1, which is inside
    // the five-byte span the patch would overwrite.
    u8* fn = page.Emit({0xE9, 0xFC, 0xFF, 0xFF, 0xFF, 0x90, 0x90});
    CHECK(BERRY_ShimPatchInPlace(fn, page.detour()) == nullptr);
  }

  // --- a normal prologue installs, the detour fires, the trampoline calls
  //     the original ------------------------------------------------------
  {
    CodePage page;
    CHECK(page.ok());
    // mov eax, 1  (B8 01 00 00 00) ; ret (C3)
    u8* fn = page.Emit({0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3});
    int (*orig)() = (int (*)())fn;
    CHECK(orig() == 1);

    BERRY_PatchedFn tramp = BERRY_ShimPatchInPlace(fn, page.detour());
    CHECK(tramp != nullptr);
    int (*patched)() = (int (*)())fn;
    CHECK(patched() == 42);                       // detour is live
    CHECK(((int (*)())tramp)() == 1);             // trampoline runs the original

    // Diagnostics: the hook is listed.
    BERRY_ShimHookInfo info[4];
    int n = BERRY_ShimListHooks(info, 4);
    CHECK(n == 1);
    CHECK(n == 1 && info[0].target == (void*)fn);
    CHECK(n == 1 && info[0].trampoline == (void*)tramp);

    CHECK(BERRY_ShimUnpatch(fn) == 0);
    CHECK(orig() == 1);                           // restored
    CHECK(BERRY_ShimListHooks(info, 4) == 0);     // nothing left
    // A re-install works; a second install on a live target is refused; a
    // second unpatch reports "not hooked".
    CHECK(BERRY_ShimPatchInPlace(fn, page.detour()) != nullptr);
    CHECK(BERRY_ShimPatchInPlace(fn, page.detour()) == nullptr);
    CHECK(BERRY_ShimUnpatch(fn) == 0);
    CHECK(BERRY_ShimUnpatch(fn) == -1);
  }

  // --- bug 9: unpatch refuses to clobber a foreign prologue --------------
  {
    CodePage page;
    CHECK(page.ok());
    u8* fn = page.Emit({0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3});
    CHECK(BERRY_ShimPatchInPlace(fn, page.detour()) != nullptr);
    // Simulate another hook replacing our jump: byte 0 is no longer 0xE9.
    fn[0] = 0xCC;
    CHECK(BERRY_ShimUnpatch(fn) == -1);   // refuses to restore over it
    fn[0] = 0xE9;                          // put our jump back for cleanup
    CHECK(BERRY_ShimUnpatch(fn) == 0);
  }

  // --- bug 1: a RIP-relative operand whose rel32 field is not last -------
  {
    CodePage page;
    CHECK(page.ok());
    *page.global() = 7;
    // cmp dword ptr [rip+disp], 7   (83 3D <disp32> 07)  -- 7 bytes
    // sete al                       (0F 94 C0)            -- 3 bytes
    // movzx eax, al                 (0F B6 C0)            -- 3 bytes
    // ret                           (C3)                  -- 1 byte
    // Returns 1 when the global == 7, else 0.
    const size_t kCmp = 0;
    u8* fn = page.Emit({0x83, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x07,
                        0x0F, 0x94, 0xC0,
                        0x0F, 0xB6, 0xC0,
                        0xC3});
    // The displacement is relative to the end of the cmp instruction (kCmp+7).
    i64 disp = (i64)(const u8*)page.global() - (i64)(fn + kCmp + 7);
    i32 d32 = (i32)disp;
    memcpy(fn + kCmp + 2, &d32, 4);

    int (*orig)() = (int (*)())fn;
    CHECK(orig() == 1);

    BERRY_PatchedFn tramp = BERRY_ShimPatchInPlace(fn, page.detour());
    CHECK(tramp != nullptr);
    int (*patched)() = (int (*)())fn;
    CHECK(patched() == 42);
    // The trampoline must still read the same global: a wrong rebase base reads
    // a neighbouring byte and returns 0.
    CHECK(((int (*)())tramp)() == 1);
    // And it must follow the global's value, proving it reads that address.
    *page.global() = 9;
    CHECK(((int (*)())tramp)() == 0);
    *page.global() = 7;
    CHECK(((int (*)())tramp)() == 1);
    CHECK(BERRY_ShimUnpatch(fn) == 0);
    CHECK(orig() == 1);
  }

  if (g_fail == 0) printf("test_hooks: ALL OK\n");
  return g_fail;
}
