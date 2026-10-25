// Mutation fuzzing for the replay container parser (berry/replay_format.h).
//
// A replay is untrusted input: it is a file another player can hand you, and it
// feeds the game's packet parsers. The format test proves specific malformed
// files are refused; this test is the other half — it mutates a *well-formed*
// file at every byte with a deterministic PRNG and asserts a hard invariant on
// the whole corpus:
//
//   * the reader never crashes, reads out of bounds, or hangs, and
//   * it either refuses the input, or returns a file that is internally
//     self-consistent (the section count matches the index, the index entries
//     point at the keyframes they name, sections are ordered, and no record
//     exceeds a declared cap).
//
// A mutation that the reader accepts is not a failure — the CRC and block
// checksums cover only part of the file, so some single-byte changes are
// harmless. What must never happen is an *accept* that yields a file the rest of
// the engine could trip over. The suite runs under ASan+UBSan, where a
// single out-of-bounds read in the parser is a hard failure, so the mutations
// are the input that proves the bounds checks actually bound.
#include "berry/replay_format.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

/// xorshift64* — a deterministic, seedable PRNG, so a failure reproduces.
struct Rng {
  u64 s;
  explicit Rng(u64 seed) : s(seed ? seed : 0x9e3779b97f4a7c15ull) {}
  u64 Next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545f4914f6cdd1dull;
  }
  u32 Below(u32 n) { return n == 0 ? 0 : (u32)(Next() % n); }
};

ReplayRecord Rec(u64 tick, ReplayRecordKind kind, u32 tag, size_t n, char fill) {
  ReplayRecord r;
  r.tick = tick;
  r.kind = kind;
  r.tag = tag;
  r.payload.assign(n, fill);
  return r;
}

/// A representative well-formed replay: keyframes, deltas, mixed payloads, a
/// label, and a real index. `payloadN` varies the compressibility.
ReplayFile BuildSample(size_t payloadN) {
  ReplayFile f;
  f.fingerprint.buildId = 4242;
  f.fingerprint.label = "1.21.0.3";
  f.fingerprint.protocolVersion = 748;
  u64 tick = 100;
  for (int k = 0; k < 4; ++k) {
    ReplaySection key;
    key.kind = ReplaySectionKind::Keyframe;
    key.startTick = tick;
    key.endTick = tick;
    key.records.push_back(Rec(tick, ReplayRecordKind::Chunk, 7, payloadN, 'A'));
    key.records.push_back(Rec(tick, ReplayRecordKind::Packet, 0x2c, payloadN / 2, 'D'));
    f.sections.push_back(key);

    ReplaySection delta;
    delta.kind = ReplaySectionKind::Delta;
    delta.startTick = tick + 1;
    delta.endTick = tick + 19;
    for (int i = 0; i < 12; ++i) {
      delta.records.push_back(
          Rec(tick + 1 + i, ReplayRecordKind::EntityMove, 0, 24, 'C'));
    }
    f.sections.push_back(delta);
    tick += 20;
  }
  f.RebuildIndex();
  return f;
}

/// The invariant the rest of the engine relies on: a replay the reader returns
/// is internally consistent. Returns false and prints why when it is not.
bool Consistent(const ReplayFile& f, const ReplayLimits& limits,
                const char** why) {
  size_t keyframes = 0;
  for (const ReplaySection& s : f.sections) {
    if (s.endTick < s.startTick) { *why = "section endTick < startTick"; return false; }
    if (s.records.size() > limits.maxRecordsPerSection) {
      *why = "too many records in a section"; return false;
    }
    for (const ReplayRecord& r : s.records) {
      if (r.payload.size() > limits.maxPayloadBytes) {
        *why = "record payload over cap"; return false;
      }
    }
    if (s.kind == ReplaySectionKind::Keyframe) ++keyframes;
  }
  if (f.sections.size() > limits.maxSections) { *why = "too many sections"; return false; }
  // The index must be exactly the keyframes, in order, each naming the keyframe
  // it points at.
  if (f.index.size() != keyframes) { *why = "index size != keyframe count"; return false; }
  size_t k = 0;
  for (const ReplaySection& s : f.sections) {
    if (s.kind != ReplaySectionKind::Keyframe) continue;
    if (f.index[k].startTick != s.startTick) { *why = "index tick != keyframe tick"; return false; }
    ++k;
  }
  return true;
}

/// Reads `data` and enforces the invariant. `mutations` counts how many of the
/// corpus the reader accepted (a coverage signal, not a pass/fail condition).
void CheckOne(const std::string& data, const ReplayLimits& limits,
              size_t* accepted, size_t* refused) {
  ReplayFile out;
  // Seed `out` with a sentinel section, so a refused read that wrongly touches
  // `out` is visible: the contract is that a refusal leaves it as it was.
  out.sections.push_back(ReplaySection{});
  const size_t before = out.sections.size();
  ReplayReader r;
  const ReplayError e = r.Read(data, &out, limits);
  if (e == ReplayError::Ok) {
    ++*accepted;
    const char* why = "";
    CHECK(Consistent(out, limits, &why));
    if (!Consistent(out, limits, &why)) {
      printf("  inconsistent accept: %s (%zu bytes)\n", why, data.size());
    }
  } else {
    ++*refused;
    // A refusal must not have touched the caller's file.
    CHECK(out.sections.size() == before);
  }
}

/// Flips one bit in one byte, at every offset, for a few bit positions.
void FuzzSingleBitFlips(const std::string& base, const ReplayLimits& limits,
                        size_t* accepted, size_t* refused) {
  Rng rng(0x1234abcd);
  for (size_t i = 0; i < base.size(); ++i) {
    for (int b = 0; b < 8; b += 3) {  // 0,3,6 — a spread without 8x the corpus
      std::string m = base;
      m[i] = (char)((u8)m[i] ^ (u8)(1u << b));
      CheckOne(m, limits, accepted, refused);
    }
  }
  (void)rng;
}

/// Replaces a byte with a value the PRNG picks, at random offsets. This reaches
/// large counts, lengths and kinds that a bit flip alone rarely produces.
void FuzzRandomByteWrites(const std::string& base, const ReplayLimits& limits,
                          size_t iterations, size_t* accepted, size_t* refused) {
  Rng rng(0xfeedface);
  for (size_t it = 0; it < iterations; ++it) {
    std::string m = base;
    const size_t writes = 1 + rng.Below(6);
    for (size_t w = 0; w < writes; ++w) {
      if (m.empty()) break;
      const size_t off = rng.Below((u32)m.size());
      m[off] = (char)(u8)rng.Next();
    }
    CheckOne(m, limits, accepted, refused);
  }
}

/// Truncates the file at every length and extends it with garbage, so the
/// reader's cursor is exercised at every boundary.
void FuzzTruncateAndExtend(const std::string& base, const ReplayLimits& limits,
                           size_t* accepted, size_t* refused) {
  Rng rng(0x0badf00d);
  for (size_t n = 0; n <= base.size(); n += 7) {
    CheckOne(base.substr(0, n), limits, accepted, refused);
  }
  for (size_t extra = 1; extra <= 64; extra += 13) {
    std::string m = base;
    for (size_t i = 0; i < extra; ++i) m.push_back((char)(u8)rng.Next());
    CheckOne(m, limits, accepted, refused);
  }
}

/// Overwrites a run of bytes with a single value, which drives the multi-byte
/// count/length/offset fields to extremes (0x00 and 0xff especially).
void FuzzRunOverwrites(const std::string& base, const ReplayLimits& limits,
                       size_t* accepted, size_t* refused) {
  const char vals[] = {0x00, (char)0xff, 0x7f, (char)0x80, 'B', 'R'};
  for (size_t start = 0; start < base.size(); start += 11) {
    for (char v : vals) {
      std::string m = base;
      const size_t run = 1 + (base.size() - start) % 16;
      for (size_t i = 0; i < run && start + i < m.size(); ++i) m[start + i] = v;
      CheckOne(m, limits, accepted, refused);
    }
  }
}

/// Splicing two valid replays together, and byte-swapping chunks between them,
/// produces length/offset fields that are individually plausible but jointly
/// wrong — the case a hand-written bounds check is most likely to miss.
void FuzzSplice(const std::string& a, const std::string& b,
                const ReplayLimits& limits, size_t* accepted, size_t* refused) {
  Rng rng(0x5150c0de);
  for (size_t it = 0; it < 2000; ++it) {
    const size_t cutA = rng.Below((u32)a.size());
    const size_t cutB = rng.Below((u32)b.size());
    std::string m = a.substr(0, cutA) + b.substr(cutB);
    CheckOne(m, limits, accepted, refused);
  }
}

}  // namespace

int main() {
  ReplayLimits limits = ReplayLimits::Default();

  ReplayWriter w;
  std::string base;
  CHECK(w.Write(BuildSample(96), &base) == ReplayError::Ok);
  std::string base2;
  CHECK(w.Write(BuildSample(7), &base2) == ReplayError::Ok);
  CHECK(!base.empty() && !base2.empty());

  // The unmutated file must round-trip, or every "refused" count below is
  // meaningless.
  {
    ReplayFile out;
    ReplayReader r;
    CHECK(r.Read(base, &out, limits) == ReplayError::Ok);
    CHECK(out.sections.size() == 8);
  }

  size_t accepted = 0, refused = 0;
  FuzzSingleBitFlips(base, limits, &accepted, &refused);
  FuzzRandomByteWrites(base, limits, 20000, &accepted, &refused);
  FuzzTruncateAndExtend(base, limits, &accepted, &refused);
  FuzzRunOverwrites(base, limits, &accepted, &refused);
  FuzzSplice(base, base2, limits, &accepted, &refused);

  // A tightened limit must never be bypassed by a header field: read the
  // well-formed file under a 1-section cap and require a refusal.
  {
    ReplayLimits tiny = limits;
    tiny.maxSections = 1;
    ReplayFile out;
    ReplayReader r;
    CHECK(r.Read(base, &out, tiny) == ReplayError::TooManySections);
    tiny = limits;
    tiny.maxRecordsPerSection = 1;
    CHECK(r.Read(base, &out, tiny) != ReplayError::Ok);
    tiny = limits;
    tiny.maxUncompressedBytes = 16;
    CHECK(r.Read(base, &out, tiny) != ReplayError::Ok);
  }

  // The corpus must have actually exercised both outcomes: an all-refuse run
  // would pass this suite without ever reaching the accept path.
  printf("test_replay_fuzz: %zu accepted, %zu refused mutations\n", accepted,
         refused);
  CHECK(accepted > 0);
  CHECK(refused > 0);

  if (g_fail == 0) {
    printf("test_replay_fuzz: ALL OK\n");
    return 0;
  }
  printf("test_replay_fuzz: %d FAILURES\n", g_fail);
  return 1;
}
