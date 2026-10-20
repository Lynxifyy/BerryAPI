// Host tests for the signature scanner, build fingerprint, and the
// fail-closed SigState semantics (Phase 0.4).
//
// Covers:
//   * Signature parse/match/Resolve.
//   * SigScanner::Find / FindAll / MatchesAt against a local byte buffer.
//   * BuildLibrary::HashSection determinism + sensitivity.
//   * SigCache fail-closed gates: without a verified build fingerprint, Resolve
//     returns 0 and State() stays Missing, so a fake feature that depends on a
//     rule disables itself instead of guessing.

#include "berry/sigscan.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

int main() {
  // --- Signature parse / match ---
  {
    Signature sig("test", "48 8D 0D ? ? ? ? E8 ? ? ? ? C3", 0);
    CHECK(sig.bytes.size() == 13);
    CHECK(sig.mask.size() == 13);
    u8 buf[13] = {0x48, 0x8D, 0x0D, 0x01, 0x02, 0x03, 0x04,
                  0xE8, 0xAA, 0xBB, 0xCC, 0xDD, 0xC3};
    CHECK(sig.Match(buf));
    buf[12] = 0xC4;
    CHECK(!sig.Match(buf));
  }
  {
    Signature sig("short", "AA ? CC");   // '?' = one wildcard byte
    CHECK(sig.bytes.size() == 3);
    CHECK(sig.mask[0] == 1 && sig.mask[1] == 0 && sig.mask[2] == 1);
    u8 buf[3] = {0xAA, 0xBB, 0xCC};
    CHECK(sig.Match(buf));
    buf[2] = 0xDD;
    CHECK(!sig.Match(buf));
  }
  {
    Signature sig("endoff", "01 02 03", 2);
    u8 buf[3] = {1, 2, 3};
    CHECK(sig.Resolve((uptr)buf) == (uptr)(buf + 2));
  }

  // --- SigScanner against a local byte buffer ---
  {
    // Buffer where the pattern appears twice (offset 4 and 18).
    u8 buf[64];
    std::memset(buf, 0x90, sizeof buf);   // fill with NOP filler over a sig
    const u8 pat[5] = {0x48, 0x8D, 0x0D, 0x00, 0x00};
    std::memcpy(buf + 4, pat, 5);
    std::memcpy(buf + 18, pat, 5);

    MemRange range;
    range.base = (uptr)buf;
    range.size = sizeof buf;
    range.name = "test-buffer";
    SigScanner scanner(std::vector<MemRange>{range});

    Signature sig("pat", "48 8D 0D ?? ??", 0);
    size_t idx = 999;
    uptr first = scanner.Find(sig, &idx);
    CHECK(first == (uptr)(buf + 4));
    CHECK(idx == 0);

    std::vector<uptr> all = scanner.FindAll(sig, 32);
    CHECK(all.size() == 2);
    CHECK(all[0] == (uptr)(buf + 4));
    CHECK(all[1] == (uptr)(buf + 18));

    CHECK(scanner.MatchesAt((uptr)(buf + 4), sig));
    CHECK(!scanner.MatchesAt((uptr)(buf + 5), sig));
  }

  // --- BuildLibrary::HashSection ---
  {
    const u8 a[4] = {1, 2, 3, 4};
    const u8 b[4] = {1, 2, 3, 5};
    u64 h1 = BuildLibrary::HashSection(a, 4);
    u64 h2 = BuildLibrary::HashSection(a, 4);
    u64 h3 = BuildLibrary::HashSection(b, 4);
    CHECK(h1 == h2);            // deterministic
    CHECK(h1 != h3);            // one byte difference changes the hash
    CHECK(BuildLibrary::HashSection(nullptr, 4) == 0);
    CHECK(BuildLibrary::HashSection(a, 0) == 0);
  }

  // --- SigCache fail-closed lifecycle ---
  // Gate 1: empty expected build (no fingerprint). Every rule is Missing and
  // Resolve() returns 0 — the dependent feature disables itself.
  {
    SigCache& sc = SigCache::Instance();
    sc.SetExpectedBuild({});
    sc.Register("rule:disabled", Signature("rule:disabled", "AA ?? BB"));
    CHECK(sc.State("rule:disabled") == SigState::Missing);
    CHECK(sc.Resolve("rule:disabled") == 0);
    CHECK(sc.State("rule:disabled") == SigState::Missing);
    CHECK(!sc.BuildVerified());
  }

  // Gate 2: a claimed-verified build whose fingerprint cannot match the live
  // host module (we are not in libminecraftpe.so). Resolution fails closed.
  {
    SigCache& sc = SigCache::Instance();
    BuildLibrary expected;
    expected.name = "libminecraftpe.so";
    expected.verified = true;
    expected.base = 1;                       // never equal to the live base
    expected.textHash = 0xDEADBEEFULL;
    expected.textSize = 1;
    sc.SetExpectedBuild(expected);
    sc.Register("rule:a", Signature("rule:a", "33 44 55"));
    sc.Register("rule:b", Signature("rule:b", "66 77 88"));
    CHECK(sc.State("rule:a") == SigState::Unknown);

    // Live fingerprint (host binary, not libminecraftpe.so) cannot match a
    // base of 1 -> the feature must disable itself rather than guess.
    uptr a0 = sc.Resolve("rule:a");
    CHECK(a0 == 0);
    CHECK(sc.State("rule:a") == SigState::Missing);
    CHECK(sc.Resolve("rule:b") == 0);
    CHECK(sc.State("rule:b") == SigState::Missing);
  }

  // Gate 3: always-unverified build — same fail-closed behavior.
  {
    SigCache& sc = SigCache::Instance();
    BuildLibrary never;
    never.name = "libminecraftpe.so";
    never.verified = false;
    never.base = 1;
    sc.SetExpectedBuild(never);
    sc.Register("rule:c", Signature("rule:c", "48 8D 0D ? ? ? ? E8"));
    CHECK(sc.State("rule:c") == SigState::Unknown);
    CHECK(sc.Resolve("rule:c") == 0);
    CHECK(sc.State("rule:c") == SigState::Missing);
  }

  if (g_fail == 0) printf("test_sigscan: ALL OK\n");
  return g_fail;
}