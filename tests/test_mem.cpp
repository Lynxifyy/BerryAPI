// Host tests for the page-protection write window.
//
// `mem::WithWritable` is what every patch path goes through. It must (a) restore
// each page's *own* protection, even when the span crosses mappings with
// different permissions (bug 12), and (b) fail closed rather than change a page
// whose protection it could not read. Both are pinned here.

#include "berry/mem.h"
#include "berry/types.h"

#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

int main() {
  const long psz = sysconf(_SC_PAGESIZE);

  // --- a two-page span with different protections ------------------------
  // Page 0 is r-x, page 1 is r--. After a write window both must return to
  // their own protection, not both to whichever page was read first.
  {
    u8* two = (u8*)mmap(nullptr, (size_t)psz * 2, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(two && two != MAP_FAILED);
    CHECK(mprotect(two, (size_t)psz, PROT_READ | PROT_EXEC) == 0);
    CHECK(mprotect(two + psz, (size_t)psz, PROT_READ) == 0);

    CHECK(mem::ProtectionOf(two) == (PROT_READ | PROT_EXEC));
    CHECK(mem::ProtectionOf(two + psz) == PROT_READ);

    bool wrote = mem::WithWritable(two, (size_t)psz + 8, [&]() {
      // Both pages are writable inside the window.
      two[0] = 0x90;
      two[psz] = 0x90;
    });
    CHECK(wrote);

    // Each page restored to its own protection.
    CHECK(mem::ProtectionOf(two) == (PROT_READ | PROT_EXEC));
    CHECK(mem::ProtectionOf(two + psz) == PROT_READ);

    munmap(two, (size_t)psz * 2);
  }

  // --- Protect reports the previous protection ---------------------------
  {
    u8* one = (u8*)mmap(nullptr, (size_t)psz, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(one && one != MAP_FAILED);
    int old = 0;
    CHECK(mem::Protect((page_t)one, (size_t)psz, PROT_READ, &old));
    CHECK(old == (PROT_READ | PROT_WRITE));
    CHECK(mem::ProtectionOf(one) == PROT_READ);
    munmap(one, (size_t)psz);
  }

  // --- ProtectionOf on an unmapped address fails closed -------------------
  {
    // A page that is definitely not mapped in this process.
    CHECK(mem::ProtectionOf((void*)0x10) == -1);
  }

  // --- WithWritable on an unmapped region refuses rather than guessing ----
  {
    bool ran = false;
    bool ok = mem::WithWritable((void*)0x10, 8, [&]() { ran = true; });
    CHECK(!ok);
    CHECK(!ran);
  }

  if (g_fail == 0) printf("test_mem: ALL OK\n");
  return g_fail;
}
