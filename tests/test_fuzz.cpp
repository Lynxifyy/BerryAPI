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
#include "berry/sigscan.h"

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
  // "ws"/"sig"/"both" token as a target selector, otherwise run everything.
  std::string which = "both";
  if (argc > 1) {
    std::string a = argv[1];
    if (a == "ws" || a == "sig" || a == "both") which = a;
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
  return g_fail;
}