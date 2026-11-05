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

  // --- parse styles + errors (bugs 14, 15) ---
  {
    // Half-nibble wildcards: "A?" is one byte (high nibble fixed), not two.
    Signature h("h", "A? 10");
    CHECK(h.bytes.size() == 2);
    CHECK(h.bytes[0] == 0xA0 && h.wild[0] == 0xF0);
    CHECK(h.bytes[1] == 0x10 && h.wild[1] == 0xFF);
    u8 ok1[2] = {0xAF, 0x10};
    CHECK(h.Match(ok1));
    u8 bad1[2] = {0xB0, 0x10};
    CHECK(!h.Match(bad1));

    // "?B" fixes the low nibble.
    Signature l("l", "?B");
    CHECK(l.bytes.size() == 1 && l.wild[0] == 0x0F);
    u8 ok2[1] = {0x3B};
    CHECK(l.Match(ok2));

    // IDA-style "??" is a whole wildcard byte.
    Signature ida("ida", "48 ?? 0D");
    CHECK(ida.bytes.size() == 3 && ida.wild[1] == 0x00);

    // C-escaped style.
    Signature esc("esc", "\\x48\\x8D\\x0D");
    CHECK(esc.bytes.size() == 3 && esc.bytes[0] == 0x48 && esc.bytes[2] == 0x0D);

    // A single hex digit is an error, not 0xF0.
    std::string err;
    Signature bad("bad", "F");
    CHECK(!bad.Parse("F", &err));
    CHECK(bad.bytes.empty());
    CHECK(!err.empty());

    // A half-nibble wildcard in the classic style no longer shifts later bytes.
    Signature mixed("mixed", "AA ? BB CC");
    CHECK(mixed.bytes.size() == 4);
  }

  // --- unique + closest-match diagnostics (improvements 24, 25) ---
  {
    u8 buf[64];
    std::memset(buf, 0x90, sizeof buf);
    const u8 pat[4] = {0x11, 0x22, 0x33, 0x44};
    std::memcpy(buf + 8, pat, 4);
    MemRange range{(uptr)buf, sizeof buf, "diag", "", 0, false};
    SigScanner scanner(std::vector<MemRange>{range});

    Signature sig("uniq", "11 22 33 44");
    size_t matches = 999;
    CHECK(scanner.FindUnique(sig, &matches) == (uptr)(buf + 8));
    CHECK(matches == 1);

    // A second copy makes it ambiguous: FindUnique fails and reports 2.
    std::memcpy(buf + 32, pat, 4);
    CHECK(scanner.FindUnique(sig, &matches) == 0);
    CHECK(matches == 2);

    // ClosestMatch finds a near miss and reports the differing bit count.
    Signature near("near", "11 22 33 45");   // last byte off by one bit
    size_t diff = 999;
    uptr cm = scanner.ClosestMatch(near, &diff);
    CHECK(cm != 0);
    CHECK(diff == 1);   // 0x44 ^ 0x45 = one bit
  }

  // --- multi-pattern single-pass scan (improvement 16) ---
  {
    u8 buf[128];
    std::memset(buf, 0x90, sizeof buf);
    const u8 p1[4] = {0x11, 0x22, 0x33, 0x44};
    const u8 p2[3] = {0xAA, 0xBB, 0xCC};
    const u8 p3[2] = {0xDE, 0xAD};
    std::memcpy(buf + 10, p1, 4);
    std::memcpy(buf + 40, p2, 3);
    std::memcpy(buf + 70, p3, 2);
    MemRange range{(uptr)buf, sizeof buf, "multi", "", 0, false};
    SigScanner scanner(std::vector<MemRange>{range});

    std::vector<Signature> sigs = {
        Signature("a", "11 22 33 44"),
        Signature("b", "AA BB CC"),
        Signature("c", "DE AD"),
        Signature("missing", "77 88 99"),
        Signature("wild", "?? ?? ??"),   // wildcard-only: matches at offset 0
    };
    std::vector<uptr> hits;
    scanner.FindAllMulti(sigs, &hits);
    CHECK(hits.size() == 5);
    CHECK(hits[0] == (uptr)(buf + 10));
    CHECK(hits[1] == (uptr)(buf + 40));
    CHECK(hits[2] == (uptr)(buf + 70));
    CHECK(hits[3] == 0);                 // not present
    CHECK(hits[4] == (uptr)buf);         // wildcard matches the first position

    // Cross-check against the single-signature scanner: same answers.
    for (size_t i = 0; i < 4; ++i) {
      CHECK(scanner.Find(sigs[i]) == hits[i]);
    }
  }

  // --- Horspool path: a half-nibble/wildcard pattern still finds matches
  //     (improvement 17) ---
  {
    u8 buf[128];
    std::memset(buf, 0x00, sizeof buf);
    // Pattern with no fully-fixed anchor byte: half-nibble + wildcards.
    buf[20] = 0xA5;
    buf[21] = 0x00;
    buf[22] = 0x3C;
    buf[23] = 0x00;
    MemRange range{(uptr)buf, sizeof buf, "horspool", "", 0, false};
    SigScanner scanner(std::vector<MemRange>{range});
    Signature sig("h", "A? ?? 3? ??");   // only half-fixed bytes
    CHECK(scanner.Find(sig) == (uptr)(buf + 20));
    CHECK(scanner.FindAll(sig, 8).size() == 1);
    // A pattern that does not match returns 0.
    Signature miss("m", "B? ?? 4? ??");
    CHECK(scanner.Find(miss) == 0);
  }

  // --- the resolution cache is build-id keyed and ASLR-stable (impr 19) ---
  {
    SigCache& sc = SigCache::Instance();
    BuildLibrary b;
    b.name = "libminecraftpe.so";
    b.verified = true;
    b.base = 0x1000;
    b.buildId = "deadbeef";
    b.inode = 42;
    sc.SetExpectedBuild(b);
    sc.Register("rule:cache", Signature("rule:cache", "AA BB"));
    // A cache for another build is ignored.
    CHECK(sc.LoadOffsetCache(R"({"build_id":"other","offsets":{"rule:cache":16}})") == 0);
    // A cache with no build-id on the expected side is ignored.
    BuildLibrary nob;
    nob.name = "libminecraftpe.so";
    nob.verified = true;
    nob.inode = 42;
    sc.SetExpectedBuild(nob);
    CHECK(sc.LoadOffsetCache(R"({"build_id":"deadbeef","offsets":{"rule:cache":16}})") == 0);
    // The save side emits the expected build-id.
    sc.SetExpectedBuild(b);
    const std::string json = sc.SaveOffsetCache();
    CHECK(json.find("\"build_id\":\"deadbeef\"") != std::string::npos);
  }

  // --- FindGuarded never crashes on an unmapped range (bug 20) ---
  {
    MemRange bad{(uptr)0x1000, 0x1000, "unmapped", "", 0, false};
    SigScanner scanner(std::vector<MemRange>{bad});
    Signature sig("g", "DE AD BE EF");
    CHECK(scanner.FindGuarded(sig) == 0);   // fault caught, no crash
  }

  // --- arm64 decode helpers (bugs 27, 28, 29) ---
  {
    // adrp x0, #0x1000 at pc 0x2000: op=1, immlo=1 -> 0xB0000000 form, which
    // the old (insn>>24)==0x90 test would have missed.
    u32 adrp = 0xB0000000u;   // op=1, immlo=1, immhi=0, Rd=0
    CHECK(arm64::IsAdrp(adrp));           // immlo=1 form (old test missed it)
    CHECK(arm64::IsAdrp(0x90000000u));    // immlo=0 form
    CHECK(!arm64::IsAdrp(0x10000000u));   // not an adrp
    CHECK(arm64::DecodeAdrp(adrp, 0x2000) == 0x3000);

    // add x0, x0, #1, lsl #12 -> 4096, not 2 (bug 28).
    u32 addShift = 0x91400000u | (1u << 10) | 0u;   // sh=1, imm12=1, Rd=0, Rn=0
    CHECK(arm64::DecodeAddImm(addShift) == 4096);
    // add x0, x0, #1 -> 1.
    u32 addPlain = 0x91000000u | (1u << 10);
    CHECK(arm64::DecodeAddImm(addPlain) == 1);

    // A page with no matching add resolves to 0, not the bare page (bug 29).
    // The look-ahead is bounded by maxInsns so it cannot read past the buffer.
    std::vector<u32> code = {0x90000000u, 0x00000000u};   // adrp; nop
    CHECK(arm64::ResolveAdrpAdd((uptr)code.data(), 0, code.size()) == 0);
    // A real adrp; add pair resolves; the add is `add x0, x0, #0x123`.
    std::vector<u32> pair = {0x90000000u, 0x91000000u | (0x123u << 10)};
    CHECK(arm64::ResolveAdrpAdd((uptr)pair.data(), 0, pair.size()) ==
          (arm64::DecodeAdrp(0x90000000u, (uptr)pair.data()) + 0x123));

    // FindAdrpXrefs on a tiny buffer must not read past the end.
    u32 tiny[1] = {0x90000000u};
    CHECK(arm64::FindAdrpXrefs((uptr)tiny, sizeof tiny, 0x0).empty());
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

  // --- version-tagged rules (improvement 27) ---
  {
    CHECK(SigCache::VersionInRange("1.20.3", "1.20.0", "1.21.0"));
    CHECK(SigCache::VersionInRange("1.20.0", "1.20.0", "1.21.0"));   // inclusive
    CHECK(SigCache::VersionInRange("1.21.0", "1.20.0", "1.21.0"));
    CHECK(!SigCache::VersionInRange("1.19.9", "1.20.0", "1.21.0"));
    CHECK(!SigCache::VersionInRange("1.21.1", "1.20.0", "1.21.0"));
    CHECK(SigCache::VersionInRange("9.9.9", "1.0.0", ""));    // unbounded top
    CHECK(SigCache::VersionInRange("0.1.0", "", "1.0.0"));    // unbounded bottom
    CHECK(SigCache::VersionInRange("2.0.0", "", ""));         // fully unbounded
    // No version selected: only an unbounded rule is eligible (fail closed).
    CHECK(!SigCache::VersionInRange("", "1.0.0", ""));
    CHECK(SigCache::VersionInRange("", "", ""));
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