// libFuzzer entry points for the pure, allocation-light parsers (improvement
// 97). Each target takes an arbitrary byte buffer and must never crash, read
// out of bounds or loop forever; the sanitizer build turns a fault into a
// finding. Build with:
//
//   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined \
//     -Iinclude fuzz/fuzz_targets.cpp <core objects> -o fuzz/berry_fuzz
//
// The same functions are also driven deterministically by tests/test_fuzz.cpp,
// so the corpus can grow without the fuzzer.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "berry/net.h"
#include "berry/sigscan.h"
#include "berry/url.h"
#include "berry/value_codec.h"

using namespace Berry;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) return 0;
  const u8* p = reinterpret_cast<const u8*>(data);

  // RFC6455 frame parser.
  {
    WsFrameView view;
    WsFrameStatus st = ParseWsFrame(p, size, &view);
    (void)st;
  }

  // Signature pattern parser + match.
  {
    std::string pattern(reinterpret_cast<const char*>(data), size);
    Signature sig("fuzz", pattern.c_str());
    if (!sig.bytes.empty()) {
      sig.Match(p);
    }
  }

  // URL parser + policy check.
  {
    std::string url(reinterpret_cast<const char*>(data), size);
    Url parsed;
    if (parsed.Parse(url)) {
      UrlPolicy policy;
      (void)policy.Check(parsed);
      (void)parsed.EffectivePort();
    }
  }

  // Value codec decode of an arbitrary buffer.
  {
    std::string buf(reinterpret_cast<const char*>(data), size);
    size_t pos = 0;
    ScriptValue out;
    (void)ValueCodec::DecodeAt(buf, &pos, &out);
  }

  return 0;
}
