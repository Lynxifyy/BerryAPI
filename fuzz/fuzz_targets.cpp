// libFuzzer entry points for the pure, allocation-light parsers (improvements
// 97 and 79). Each target takes an arbitrary byte buffer and must never crash,
// read out of bounds or loop forever; the sanitizer build turns a fault into a
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

#include "berry/manifest.h"
#include "berry/net.h"
#include "berry/pack.h"
#include "berry/package.h"
#include "berry/replay_format.h"
#include "berry/sigscan.h"
#include "berry/url.h"
#include "berry/value_codec.h"

using namespace Berry;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) return 0;
  const u8* p = reinterpret_cast<const u8*>(data);
  const std::string s(reinterpret_cast<const char*>(data), size);

  // RFC6455 frame parser.
  {
    WsFrameView view;
    (void)ParseWsFrame(p, size, &view);
  }

  // Signature pattern parser + match.
  {
    Signature sig("fuzz", s.c_str());
    if (!sig.bytes.empty()) sig.Match(p);
  }

  // URL parser + policy check.
  {
    Url parsed;
    if (parsed.Parse(s)) {
      UrlPolicy policy;
      (void)policy.Check(parsed);
      (void)parsed.EffectivePort();
    }
  }

  // Value codec decode of an arbitrary buffer.
  {
    size_t pos = 0;
    ScriptValue out;
    (void)ValueCodec::DecodeAt(s, &pos, &out);
  }

  // Script manifest parse (improvement 79).
  {
    ScriptManifest m;
    ManifestError err = ManifestError::Ok;
    std::string detail;
    (void)ScriptManifest::Parse(s, &m, &err, &detail);
  }

  // Package manifest parse.
  {
    PackageManifest m;
    PackageError err = PackageError::Ok;
    std::string detail;
    (void)PackageManifest::Parse(s, &m, &err, &detail);
  }

  // Cosmetic pack parse (a JSON document, validated without the registry).
  {
    std::vector<CosmeticAsset> assets;
    std::string name, detail;
    (void)CosmeticPackLoader::ParsePack(s, &assets, &name, &detail);
  }

  // Replay container read.
  {
    ReplayFile f;
    ReplayReader r;
    (void)r.Read(s, &f);
  }

  return 0;
}
