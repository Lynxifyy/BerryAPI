// Host tests for OTA signature packs (data-driven signatures + struct layouts).
//
// Covers:
//   * Parse: format/name/build required, unknown key refused, bad pattern and
//     duplicate id refused, bad layout refused, a layout whose build differs
//     from the pack's refused, an empty pack refused.
//   * Apply: gated by the active build fingerprint. A pack for another build
//     (or an empty active build) registers nothing.
//   * Apply: a matching pack registers its rows into SigCache and its layouts
//     into LayoutRegistry, atomically.
//   * Directory load: only `*.sigpack.json` files are picked up, a bad pack
//     does not stop the others, and the per-pack report is accurate.

#include "berry/sigpack.h"
#include "berry/layout.h"
#include "berry/loader.h"
#include "berry/sigscan.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

std::string PackJson(const std::string& body) {
  return std::string("{\"format\":1,\"name\":\"test-pack\",\"build\":\"BUILD-A\",") +
         body + "}";
}

std::string LayoutJson(const std::string& build) {
  return "{\"name\":\"Actor\",\"version\":\"1.20\",\"build\":\"" + build +
         "\",\"size\":16,\"fields\":["
         "{\"name\":\"health\",\"type\":\"f32\",\"offset\":0},"
         "{\"name\":\"id\",\"type\":\"u64\",\"offset\":8}]}";
}

}  // namespace

//----------------------------------------------------------------------------
// Parse
//----------------------------------------------------------------------------

static void TestParseValid() {
  SignaturePack pack;
  SigPackError e = SigPackError::Ok;
  std::string err;
  const std::string json = PackJson(
      "\"signatures\":["
      "{\"id\":\"world:levelTick\",\"pattern\":\"48 8D 0D ? ? ? ? E8\"},"
      "{\"id\":\"combat:damageFn\",\"pattern\":\"AA BB\",\"from_end\":2}],"
      "\"layouts\":[" + LayoutJson("BUILD-A") + "]");
  CHECK(SignaturePack::Parse(json, &pack, &e, &err));
  CHECK(e == SigPackError::Ok);
  CHECK(pack.name == "test-pack");
  CHECK(pack.build == "BUILD-A");
  CHECK(pack.signatures.size() == 2);
  CHECK(pack.signatures[0].id == "world:levelTick");
  CHECK(pack.signatures[0].fromEndOffset == 0);
  CHECK(pack.signatures[1].id == "combat:damageFn");
  CHECK(pack.signatures[1].fromEndOffset == 2);
  CHECK(pack.layouts.size() == 1);
  CHECK(pack.layouts[0].name == "Actor");
}

static void TestParseRejections() {
  SignaturePack pack;
  SigPackError e = SigPackError::Ok;
  std::string err;

  // Not JSON.
  CHECK(!SignaturePack::Parse("not json", &pack, &e, &err));
  CHECK(e == SigPackError::BadJson);

  // Wrong format version.
  CHECK(!SignaturePack::Parse(
      "{\"format\":99,\"name\":\"p\",\"build\":\"B\",\"signatures\":["
      "{\"id\":\"a\",\"pattern\":\"AA\"}]}",
      &pack, &e, &err));
  CHECK(e == SigPackError::BadFormat);

  // Missing build.
  CHECK(!SignaturePack::Parse(
      "{\"format\":1,\"name\":\"p\",\"signatures\":[{\"id\":\"a\",\"pattern\":\"AA\"}]}",
      &pack, &e, &err));
  CHECK(e == SigPackError::MissingBuild);

  // Unknown top-level key.
  CHECK(!SignaturePack::Parse(
      PackJson("\"signatures\":[{\"id\":\"a\",\"pattern\":\"AA\"}],\"extra\":1"),
      &pack, &e, &err));
  CHECK(e == SigPackError::BadJson);

  // Unknown key inside a row (typo must fail loudly).
  CHECK(!SignaturePack::Parse(
      PackJson("\"signatures\":[{\"id\":\"a\",\"patern\":\"AA\"}]"),
      &pack, &e, &err));
  CHECK(e == SigPackError::BadSignature);

  // Duplicate id.
  CHECK(!SignaturePack::Parse(
      PackJson("\"signatures\":[{\"id\":\"a\",\"pattern\":\"AA\"},"
               "{\"id\":\"a\",\"pattern\":\"BB\"}]"),
      &pack, &e, &err));
  CHECK(e == SigPackError::DuplicateId);

  // A pattern with no hex bytes is not a real signature.
  CHECK(!SignaturePack::Parse(
      PackJson("\"signatures\":[{\"id\":\"a\",\"pattern\":\"   \"}]"),
      &pack, &e, &err));
  CHECK(e == SigPackError::BadPattern);

  // Layout targeting a different build than the pack.
  CHECK(!SignaturePack::Parse(
      PackJson("\"layouts\":[" + LayoutJson("BUILD-B") + "]"),
      &pack, &e, &err));
  CHECK(e == SigPackError::LayoutBuildMismatch);

  // A malformed layout.
  CHECK(!SignaturePack::Parse(
      PackJson("\"layouts\":[{\"name\":\"X\",\"version\":\"1\",\"build\":\"BUILD-A\","
               "\"size\":4,\"fields\":[{\"name\":\"f\",\"type\":\"u32\",\"offset\":2}]}]"),
      &pack, &e, &err));
  CHECK(e == SigPackError::BadLayout);

  // Empty pack (no signatures, no layouts).
  CHECK(!SignaturePack::Parse(
      "{\"format\":1,\"name\":\"p\",\"build\":\"B\"}", &pack, &e, &err));
  CHECK(e == SigPackError::NoEntries);
}

//----------------------------------------------------------------------------
// Build-fingerprint gate
//----------------------------------------------------------------------------

static void TestBuildGate() {
  SignaturePack pack;
  SigPackError e = SigPackError::Ok;
  CHECK(SignaturePack::Parse(
      PackJson("\"signatures\":[{\"id\":\"gate:rule\",\"pattern\":\"AA BB CC\"}]"),
      &pack, &e));

  SigCache& sc = SigCache::Instance();

  // Empty active build: nothing applies (fail closed, never "by default").
  SigPackReport rep;
  CHECK(ApplySignaturePack(pack, "", sc, nullptr, &rep) == SigPackError::BuildMismatch);
  CHECK(rep.signaturesRegistered == 0);

  // A different build: refused.
  CHECK(ApplySignaturePack(pack, "BUILD-B", sc, nullptr, &rep) ==
        SigPackError::BuildMismatch);
  CHECK(rep.signaturesRegistered == 0);

  // Matching build (case- and 0x-insensitive): applies.
  CHECK(ApplySignaturePack(pack, "0xbuild-a", sc, nullptr, &rep) == SigPackError::Ok);
  CHECK(rep.signaturesRegistered == 1);
  CHECK(sc.Has("gate:rule"));
}

//----------------------------------------------------------------------------
// Apply registers into both registries
//----------------------------------------------------------------------------

static void TestApplyRegisters() {
  SignaturePack pack;
  SigPackError e = SigPackError::Ok;
  CHECK(SignaturePack::Parse(
      PackJson("\"signatures\":[{\"id\":\"apply:rule\",\"pattern\":\"11 22 33\"}],"
               "\"layouts\":[" + LayoutJson("BUILD-A") + "]"),
      &pack, &e));

  SigCache& sc = SigCache::Instance();
  LayoutRegistry layouts;
  SigPackReport rep;
  CHECK(ApplySignaturePack(pack, "BUILD-A", sc, &layouts, &rep) == SigPackError::Ok);
  CHECK(rep.signaturesRegistered == 1);
  CHECK(rep.layoutsRegistered == 1);
  CHECK(sc.Has("apply:rule"));
  CHECK(layouts.Count() == 1);

  // The layout resolves only under the active build it was written for.
  layouts.SetActiveBuild("BUILD-A");
  StructLayout out;
  CHECK(layouts.Find("Actor", &out));
  CHECK(out.size == 16);
  layouts.SetActiveBuild("BUILD-Z");
  CHECK(!layouts.Find("Actor", &out));
}

//----------------------------------------------------------------------------
// Directory loading
//----------------------------------------------------------------------------

static void TestDirectoryLoad() {
  MemoryScriptFileSystem fs;
  const std::string good =
      PackJson("\"signatures\":[{\"id\":\"dir:rule\",\"pattern\":\"44 55 66\"}]");
  fs.Write("packs/a.sigpack.json", good);
  fs.Write("packs/b.sigpack.json", good);          // duplicate rule id -> ok (replace)
  fs.Write("packs/bad.sigpack.json", "{\"format\":1}");   // missing name -> refused
  fs.Write("packs/notes.json", good);              // wrong suffix -> ignored
  fs.Write("packs/readme.txt", "hello");           // ignored

  SignaturePackLoader loader(&fs, "packs");
  SigCache& sc = SigCache::Instance();
  SigPackLoadReport rep = loader.LoadDirectory("BUILD-A", sc, nullptr);

  CHECK(rep.packsTried == 3);        // a, b, bad (notes.txt/readme ignored)
  CHECK(rep.packsApplied == 2);
  CHECK(rep.packsFailed == 1);
  CHECK(rep.signaturesRegistered == 2);
  CHECK(!rep.truncated);
  CHECK(sc.Has("dir:rule"));
}

int main() {
  TestParseValid();
  TestParseRejections();
  TestBuildGate();
  TestApplyRegisters();
  TestDirectoryLoad();
  if (g_fail == 0) printf("test_sigpack: ALL OK\n");
  return g_fail;
}
