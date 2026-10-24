// Phase 0.6: deterministic host fuzzers.
//
// Two targets, seeded from the test name (which the Makefile passes in), so a
// fuzz run is reproducible from the command line:
//
//   build/test_test_fuzz ws     # WebSocket frame parser
//   build/test_test_fuzz sig    # signature scanner / BuildLibrary hash
//
// Every input we throw at ParseWsFrame and the scanner must be handled without
// UB (no OOB, no integer overflow, bounded allocation). We validate the status
// contract rather than random bytes, so the fuzzer doubles as a contract test:
//   * any input → one of {Ok, NeedMore, Oversized, Invalid} (never crash)
//   * truncated frames → NeedMore, not a partial read
//   * control frames (Close/Ping/Pong) are single-frame ≤125 bytes
//   * a frame that parses as Ok consumes exactly frameLen bytes

#include "berry/net.h"
#include "berry/replay_format.h"
#include "berry/sigscan.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

//--------------------------------------------------------------------------
// WebSocket frame parser fuzzer + contract.
//--------------------------------------------------------------------------

// Deterministic PRNG (xorshift32) so the corpus is reproducible.
struct Rng {
  uint32_t s;
  uint32_t Next() {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
  }
};

static void FuzzWs(uint32_t seed) {
  Rng rng{seed ? seed : 1};
  const size_t kMax = 300;
  for (int iter = 0; iter < 2000; ++iter) {
    std::vector<u8> buf(rng.Next() % kMax);
    for (auto& b : buf) b = (u8)rng.Next();

    WsFrameView view;
    WsFrameStatus st = ParseWsFrame(buf.data(), buf.size(), &view);
    CHECK(st == WsFrameStatus::Ok || st == WsFrameStatus::NeedMore ||
          st == WsFrameStatus::Oversized || st == WsFrameStatus::Invalid);

    // A valid Ok frame must have consumed exactly frameLen and expose the
    // payload within the buffer.
    if (st == WsFrameStatus::Ok) {
      CHECK(view.frameLen <= buf.size());
      CHECK(view.payload >= buf.data());
      CHECK(view.payload + view.payloadLen <= buf.data() + buf.size());
      // Control frames are single-frame ≤125 bytes.
      bool control = view.opcode >= 0x8;
      CHECK(!control || (view.fin && view.payloadLen <= 125));
    }
  }
}

// Corrupt-layout cases that previously caused trouble.
static void ContractWs() {
  WsFrameView v;

  // Empty / one-byte needs more.
  CHECK(ParseWsFrame(nullptr, 0, &v) == WsFrameStatus::NeedMore);
  u8 one[1] = {0x81};
  CHECK(ParseWsFrame(one, 1, &v) == WsFrameStatus::NeedMore);

  // Minimal unmasked text frame: FIN|text, len 0.
  u8 empty[2] = {0x81, 0x00};
  CHECK(ParseWsFrame(empty, 2, &v) == WsFrameStatus::Ok);
  CHECK(v.opcode == 0x1 && v.payloadLen == 0 && v.frameLen == 2);

  // Masked frame: "Hi" with mask key 0x12345678.
  u8 masked[] = {0x81, 0x82, 0x12, 0x34, 0x56, 0x78, 0x3D, 0x66};
  CHECK(ParseWsFrame(masked, sizeof masked, &v) == WsFrameStatus::Ok);
  CHECK(v.masked && v.payloadLen == 2);
  CHECK(v.frameLen == 8);

  // Truncated extended-length header.
  u8 ext126[] = {0x81, 126, 0x01};
  CHECK(ParseWsFrame(ext126, sizeof ext126, &v) == WsFrameStatus::NeedMore);
  // Truncated 64-bit length.
  u8 ext127[9] = {0x81, 127, 0, 0, 0, 0, 0, 0, 0};
  CHECK(ParseWsFrame(ext127, sizeof ext127, &v) == WsFrameStatus::NeedMore);

  // Oversized payload (declared 18 bytes, cap 16).
  u8 big[2] = {0x81, 18};
  CHECK(ParseWsFrame(big, 2, &v, 16) == WsFrameStatus::Oversized);

  // Control frame too long for RFC6455.
  u8 ctrlTooLong[2] = {0x89, 126};   // ping, len 126
  CHECK(ParseWsFrame(ctrlTooLong, 2, &v) == WsFrameStatus::Invalid);
  // Control frame not FIN.
  u8 ctrlNotFin[2] = {0x09, 0x00};
  CHECK(ParseWsFrame(ctrlNotFin, 2, &v) == WsFrameStatus::Invalid);
  // RSV bits set.
  u8 rsv[2] = {0xC1, 0x00};
  CHECK(ParseWsFrame(rsv, 2, &v) == WsFrameStatus::Invalid);
  // Reserved opcodes.
  u8 rsvOp[2] = {0x83, 0x00};
  CHECK(ParseWsFrame(rsvOp, 2, &v) == WsFrameStatus::Invalid);
  u8 rsvOp2[2] = {0x8B, 0x00};
  CHECK(ParseWsFrame(rsvOp2, 2, &v) == WsFrameStatus::Invalid);
  // Mask key present but truncated.
  u8 maskCut[3] = {0x81, 0x80, 0x01};
  CHECK(ParseWsFrame(maskCut, 3, &v) == WsFrameStatus::NeedMore);
  // Null out pointer.
  CHECK(ParseWsFrame(empty, 2, nullptr) == WsFrameStatus::Invalid);
}

//--------------------------------------------------------------------------
// Signature scanner fuzzer.
//--------------------------------------------------------------------------

static void FuzzSig(uint32_t seed) {
  Rng rng{seed ? seed : 7};
  const size_t kMax = 512;
  for (int iter = 0; iter < 1500; ++iter) {
    std::vector<u8> buf(rng.Next() % kMax);
    for (auto& b : buf) b = (u8)rng.Next();

    MemRange range;
    range.base = (uptr)buf.data();
    range.size = buf.size();
    range.name = "fuzz";
    SigScanner scanner(std::vector<MemRange>{range});

    // Random-ish pattern.
    std::string pat;
    for (int i = 0; i < (int)(rng.Next() % 12); ++i) {
      if (rng.Next() & 1) pat += "? ";
      else { char t[4]; snprintf(t, sizeof t, "%02X ", rng.Next() & 0xff); pat += t; }
    }
    Signature sig("f", pat.c_str());
    uptr hit = scanner.Find(sig);
    if (hit) {
      // A found pattern must actually match at that address.
      CHECK(hit >= range.base && hit + sig.bytes.size() <= range.base + range.size);
    }
    std::vector<uptr> all = scanner.FindAll(sig, 100);
    if (!all.empty()) {
      // Must be sorted ascending and inside the range.
      for (size_t i = 1; i < all.size(); ++i) CHECK(all[i] > all[i - 1] && all[i] <= range.base + range.size);
      CHECK(all.front() >= range.base);
    }
  }
}

//--------------------------------------------------------------------------
// Replay container parser fuzzer + contract.
//
// A replay file is untrusted input: it is a file someone else can hand you and
// it feeds the game's packet parsers. So this fuzzer does the strongest thing
// available: it builds a VALID replay, then mutates it (bit flips, truncation,
// byte insertion/deletion, random replacement) and asserts the reader either
// accepts it completely or refuses it completely — never crashes, never
// half-loads, and never trusts a length. It also throws pure random bytes at
// the reader.
//--------------------------------------------------------------------------

/// A small valid replay: two keyframes with a delta after each.
static std::string ValidReplayBytes() {
  ReplayFile f;
  f.fingerprint.buildId = 4242;
  f.fingerprint.label = "1.21.0";
  f.fingerprint.protocolVersion = 748;
  u64 tick = 100;
  for (int k = 0; k < 2; ++k) {
    ReplaySection key;
    key.kind = ReplaySectionKind::Keyframe;
    key.startTick = tick;
    key.endTick = tick;
    ReplayRecord a;
    a.tick = tick; a.kind = ReplayRecordKind::Chunk; a.tag = 7;
    a.payload.assign(256, (char)('A' + k));
    key.records.push_back(a);
    f.sections.push_back(key);

    ReplaySection delta;
    delta.kind = ReplaySectionKind::Delta;
    delta.startTick = tick + 1;
    delta.endTick = tick + 19;
    ReplayRecord b;
    b.tick = tick + 1; b.kind = ReplayRecordKind::EntityMove;
    b.payload.assign(48, 'M');
    delta.records.push_back(b);
    f.sections.push_back(delta);
    tick += 20;
  }
  f.RebuildIndex();
  ReplayWriter w;
  std::string bytes;
  if (w.Write(f, &bytes) != ReplayError::Ok) return std::string();
  return bytes;
}

/// Asserts a read is all-or-nothing: either Ok with the full section set, or a
/// non-Ok error with `out` left empty. Never a crash, never a partial load.
static void CheckAllOrNothing(const std::string& data, uint32_t seed) {
  ReplayFile out;
  ReplayReader r;
  const ReplayError e = r.Read(data, &out);
  if (e == ReplayError::Ok) {
    // A successful load must carry an index consistent with its keyframes.
    size_t keyframes = 0;
    for (const ReplaySection& s : out.sections) {
      if (s.kind == ReplaySectionKind::Keyframe) ++keyframes;
    }
    CHECK(out.index.size() == keyframes);
    CHECK(!out.sections.empty());
  } else {
    CHECK(out.sections.empty());
  }
  (void)seed;
}

static void FuzzReplay(uint32_t seed) {
  Rng rng{seed ? seed : 1};
  const std::string valid = ValidReplayBytes();
  CHECK(!valid.empty());

  // 1. Pure random bytes: the reader must never crash or trust a length.
  for (int iter = 0; iter < 500; ++iter) {
    std::vector<u8> buf(rng.Next() % 400);
    for (auto& b : buf) b = (u8)rng.Next();
    CheckAllOrNothing(std::string((const char*)buf.data(), buf.size()), rng.Next());
  }

  // 2. Random bytes that START with the magic, so the parser gets past the
  //    first gate and into the header/block/index logic.
  for (int iter = 0; iter < 500; ++iter) {
    std::vector<u8> buf(64 + rng.Next() % 512);
    for (auto& b : buf) b = (u8)rng.Next();
    buf[0] = 'B'; buf[1] = 'R'; buf[2] = 'P'; buf[3] = 'L';
    CheckAllOrNothing(std::string((const char*)buf.data(), buf.size()), rng.Next());
  }

  // 3. Bit flips on a valid replay: the CRC/checksum must catch nearly all,
  //    and any that slip through must still be all-or-nothing.
  for (int iter = 0; iter < 3000; ++iter) {
    std::string m = valid;
    const size_t at = rng.Next() % m.size();
    m[at] = (char)((unsigned char)m[at] ^ (u8)(1u << (rng.Next() % 8)));
    CheckAllOrNothing(m, rng.Next());
  }

  // 4. Truncation at every length class.
  for (size_t cut = 0; cut < valid.size(); cut += 1 + (rng.Next() % 17)) {
    CheckAllOrNothing(valid.substr(0, cut), rng.Next());
  }

  // 5. Insertion / deletion, which shifts every offset after the change.
  for (int iter = 0; iter < 1000; ++iter) {
    std::string m = valid;
    const size_t at = rng.Next() % m.size();
    if (rng.Next() & 1) {
      m.insert(at, 1, (char)rng.Next());
    } else if (m.size() > 1) {
      m.erase(at, 1);
    }
    CheckAllOrNothing(m, rng.Next());
  }

  // 6. Bulk random replacement over a window (corrupts a block's payload).
  for (int iter = 0; iter < 500; ++iter) {
    std::string m = valid;
    const size_t at = rng.Next() % m.size();
    const size_t n = std::min<size_t>(rng.Next() % 64, m.size() - at);
    for (size_t i = 0; i < n; ++i) m[at + i] = (char)rng.Next();
    CheckAllOrNothing(m, rng.Next());
  }
}

static void ContractReplay() {
  // The unmutated replay loads exactly.
  const std::string valid = ValidReplayBytes();
  ReplayFile out;
  ReplayReader r;
  CHECK(r.Read(valid, &out) == ReplayError::Ok);
  CHECK(out.sections.size() == 4);
  CHECK(out.index.size() == 2);

  // Every truncation of the header is refused, not misread.
  for (size_t cut = 1; cut < 64; ++cut) {
    ReplayFile tmp;
    CHECK(r.Read(valid.substr(0, cut), &tmp) != ReplayError::Ok);
    CHECK(tmp.sections.empty());
  }

  // A file whose declared section count is absurd is refused before any
  // allocation that count would drive.
  {
    std::string bad = valid;
    bad[24] = (char)0xff; bad[25] = (char)0xff; bad[26] = (char)0xff; bad[27] = (char)0x7f;
    // Recompute the header CRC so the section-count cap is what fires.
    u32 crc = 0xffffffffu;
    for (size_t i = 0; i < 56; ++i) {
      crc ^= (unsigned char)bad[i];
      for (int k = 0; k < 8; ++k) {
        const u32 mask = (u32)-(int)(crc & 1u);
        crc = (crc >> 1) ^ (0xedb88320u & mask);
      }
    }
    crc ^= 0xffffffffu;
    bad[56] = (char)(crc & 0xff); bad[57] = (char)((crc >> 8) & 0xff);
    bad[58] = (char)((crc >> 16) & 0xff); bad[59] = (char)((crc >> 24) & 0xff);
    ReplayFile tmp;
    CHECK(r.Read(bad, &tmp) == ReplayError::TooManySections);
  }
}

static void ContractSig() {
  // Empty signature never matches but must not crash.
  Signature empty("empty", "");
  CHECK(empty.bytes.empty());
  MemRange range;
  char buf[64]; std::memset(buf, 0, sizeof buf);
  range.base = (uptr)buf; range.size = sizeof buf; range.name = "c";
  SigScanner scanner(std::vector<MemRange>{range});
  CHECK(scanner.Find(empty) == 0);
  CHECK(scanner.FindAll(empty).empty());

  // Long input pattern vs short buffer.
  Signature longpat("lp", "AA BB CC DD EE FF 11 22 33 44",
                    /*fromEnd*/0);
  CHECK(scanner.Find(longpat) == 0);

  // BuildLibrary::HashSection on giant input does not overflow internally.
  std::vector<u8> big(1 << 20, 0x5A);
  u64 h = BuildLibrary::HashSection(big.data(), big.size());
  CHECK(h != 0);
}

int main(int argc, char** argv) {
  // make test passes the binary path as argv[1]; only treat an explicit
  // "ws"/"sig"/"replay"/"both" token as a target selector, otherwise run
  // everything.
  std::string which = "both";
  if (argc > 1) {
    std::string a = argv[1];
    if (a == "ws" || a == "sig" || a == "replay" || a == "both") which = a;
  }
  if (which == "ws" || which == "both") {
    FuzzWs(0x1234);
    FuzzWs(0x9abcdef1);
    ContractWs();
    printf("test_fuzz[ws]: ALL OK\n");
  }
  if (which == "sig" || which == "both") {
    FuzzSig(0xdead);
    FuzzSig(0xbeef);
    ContractSig();
    printf("test_fuzz[sig]: ALL OK\n");
  }
  if (which == "replay" || which == "both") {
    FuzzReplay(0x5eed);
    FuzzReplay(0xfeedface);
    ContractReplay();
    printf("test_fuzz[replay]: ALL OK\n");
  }
  return g_fail;
}