// Host tests for local cosmetic model packs (the data-driven authoring path).
//
// Covers:
//   * ParsePack: a full asset round-trips field-for-field; a minimal asset
//     takes the documented defaults (bone from slot, white tint, scale 1),
//   * ParsePack fail-closed: bad JSON, an unknown key, a wrong/absent format,
//     a missing name, no assets, a bad slot/bone/transform, a duplicate id,
//     and every other malformed field is refused,
//   * atomicity: a pack whose second asset collides with the catalog registers
//     nothing (RegisterAll), and a pack cannot repeat an id internally,
//   * LoadDirectory: only *.pack.json is picked up, in sorted order, and one
//     failed pack does not stop the others,
//   * the cosmetic.reload_packs binding: capability-gated, arity-safe, and
//     fail-closed with no loader.
#include "berry/api_bindings.h"
#include "berry/cosmetic.h"
#include "berry/loader.h"
#include "berry/pack.h"
#include "berry/script.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

void FreshRegistry() {
  CosmeticRegistry::Instance().Clear();
}

/// A minimal, valid pack with a single asset. `assets` is spliced in verbatim.
std::string Pack(const std::string& assets) {
  return std::string("{\"format\":1,\"name\":\"test.pack\",\"assets\":[") +
         assets + "]}";
}

ScriptStatus CallBinding(ScriptBindings& bindings, const std::string& name,
                         const std::vector<ScriptValue>& args,
                         const std::vector<std::string>& grants,
                         ScriptValue* out) {
  CapabilitySet s;
  for (const auto& c : grants) s.Add(c);
  ScriptHost host("pack-test", s, nullptr);
  host.SetBindings(&bindings);
  return host.CallBinding(name, args, out);
}

}  // namespace

//----------------------------------------------------------------------------
// ParsePack
//----------------------------------------------------------------------------

static void TestParseFullAsset() {
  const std::string text = R"({
    "format": 1,
    "name": "creator.winter",
    "author": "Pack Author",
    "assets": [
      {
        "id": "creator.cape.snow",
        "slot": "back",
        "bone": "torso",
        "geometry": 42,
        "texture": 43,
        "offset": {"x": 0.1, "y": 0.2, "z": -0.3},
        "rotation": {"x": 0, "y": 90, "z": 0},
        "scale": 1.5,
        "tint": {"r": 0.9, "g": 0.9, "b": 1.0},
        "trail": 0.7,
        "tags": ["winter", "limited"]
      }
    ]
  })";
  std::vector<CosmeticAsset> assets;
  std::string name, detail;
  CHECK(CosmeticPackLoader::ParsePack(text, &assets, &name, &detail) ==
        PackError::Ok);
  CHECK(name == "creator.winter");
  CHECK(assets.size() == 1);
  const CosmeticAsset& a = assets[0];
  CHECK(a.id == "creator.cape.snow");
  CHECK(a.slot == CosmeticSlot::Back);
  CHECK(a.bone == Bone::Torso);
  CHECK(a.geometry == 42);
  CHECK(a.texture == 43);
  CHECK(std::fabs(a.offset.x - 0.1f) < 1e-6f);
  CHECK(std::fabs(a.offset.z + 0.3f) < 1e-6f);
  CHECK(std::fabs(a.rotation.y - 90.0f) < 1e-6f);
  CHECK(std::fabs(a.scale - 1.5f) < 1e-6f);
  CHECK(std::fabs(a.tint.x - 0.9f) < 1e-6f);
  CHECK(std::fabs(a.trail - 0.7f) < 1e-6f);
  CHECK(a.tags.size() == 2 && a.tags[0] == "winter");
  // Provenance defaults to the pack.
  CHECK(a.pack == "creator.winter");
  CHECK(a.author == "Pack Author");
  CHECK(a.valid());
}

static void TestParseMinimalAssetDefaults() {
  std::vector<CosmeticAsset> assets;
  CHECK(CosmeticPackLoader::ParsePack(
            Pack(R"({"id":"m.hat","slot":"head"})"), &assets) == PackError::Ok);
  CHECK(assets.size() == 1);
  const CosmeticAsset& a = assets[0];
  // No bone named: resolved from the slot.
  CHECK(a.bone == Bone::None);
  CHECK(a.ResolvedBone() == Bone::Head);
  CHECK(a.scale == 1.0f);
  CHECK(a.tint == Vec3(1, 1, 1));
  CHECK(a.trail == 0.0f);
  CHECK(a.geometry == 0 && a.texture == 0);
  CHECK(a.tags.empty());
  // Explicit bone "none" is accepted and means "use the slot default".
  CHECK(CosmeticPackLoader::ParsePack(
            Pack(R"({"id":"m.hat","slot":"head","bone":"none"})"), &assets) ==
        PackError::Ok);
  CHECK(assets[0].bone == Bone::None);
}

static void TestParseRejections() {
  std::vector<CosmeticAsset> assets;
  struct Case { std::string text; PackError want; };
  const Case cases[] = {
      // Bad document shape.
      {"{not json", PackError::BadJson},
      {"[1,2,3]", PackError::BadJson},
      {"\"a string\"", PackError::BadJson},
      // Unknown pack key.
      {R"({"format":1,"name":"p","assets":[{"id":"a","slot":"head"}],"extra":1})",
       PackError::BadAsset},
      // Format absent / wrong.
      {R"({"name":"p","assets":[{"id":"a","slot":"head"}]})", PackError::BadFormat},
      {R"({"format":2,"name":"p","assets":[{"id":"a","slot":"head"}]})",
       PackError::BadFormat},
      {R"({"format":"1","name":"p","assets":[{"id":"a","slot":"head"}]})",
       PackError::BadFormat},
      // Name absent / empty / bad.
      {R"({"format":1,"assets":[{"id":"a","slot":"head"}]})", PackError::MissingName},
      {R"({"format":1,"name":"","assets":[{"id":"a","slot":"head"}]})",
       PackError::MissingName},
      {R"({"format":1,"name":7,"assets":[{"id":"a","slot":"head"}]})",
       PackError::MissingName},
      // No assets / empty / not an array.
      {R"({"format":1,"name":"p"})", PackError::NoAssets},
      {R"({"format":1,"name":"p","assets":[]})", PackError::NoAssets},
      {R"({"format":1,"name":"p","assets":{}})", PackError::NoAssets},
      // Asset not an object.
      {R"({"format":1,"name":"p","assets":[42]})", PackError::BadAsset},
      // Unknown asset key.
      {Pack(R"({"id":"a","slot":"head","offest":{"x":0,"y":0,"z":0}})"),
       PackError::BadAsset},
      // id absent / empty / non-string / with a newline.
      {Pack(R"({"slot":"head"})"), PackError::BadAsset},
      {Pack(R"({"id":"","slot":"head"})"), PackError::BadAsset},
      {Pack(R"({"id":5,"slot":"head"})"), PackError::BadAsset},
      {Pack("{\"id\":\"a\\nb\",\"slot\":\"head\"}"), PackError::BadAsset},
      // slot absent / unknown / non-string.
      {Pack(R"({"id":"a"})"), PackError::BadAsset},
      {Pack(R"({"id":"a","slot":"hat"})"), PackError::BadAsset},
      {Pack(R"({"id":"a","slot":1})"), PackError::BadAsset},
      // bone unknown.
      {Pack(R"({"id":"a","slot":"head","bone":"tail"})"), PackError::BadAsset},
      // geometry/texture bad.
      {Pack(R"({"id":"a","slot":"head","geometry":-1})"), PackError::BadAsset},
      {Pack(R"({"id":"a","slot":"head","texture":"x"})"), PackError::BadAsset},
      // offset/rotation shape and values.
      {Pack(R"({"id":"a","slot":"head","offset":{"x":0,"y":0}})"),
       PackError::BadAsset},
      {Pack(R"({"id":"a","slot":"head","offset":{"x":0,"y":0,"z":"z"}})"),
       PackError::BadAsset},
      // scale bad.
      {Pack(R"({"id":"a","slot":"head","scale":0})"), PackError::BadAsset},
      {Pack(R"({"id":"a","slot":"head","scale":-2})"), PackError::BadAsset},
      {Pack(R"({"id":"a","slot":"head","scale":"big"})"), PackError::BadAsset},
      // tint bad.
      {Pack(R"({"id":"a","slot":"head","tint":{"r":1,"g":1}})"),
       PackError::BadAsset},
      // trail out of range.
      {Pack(R"({"id":"a","slot":"head","trail":1.5})"), PackError::BadAsset},
      {Pack(R"({"id":"a","slot":"head","trail":-0.1})"), PackError::BadAsset},
      // tags not an array / bad element.
      {Pack(R"({"id":"a","slot":"head","tags":"x"})"), PackError::BadAsset},
      {Pack(R"({"id":"a","slot":"head","tags":[1]})"), PackError::BadAsset},
      // author/pack bad types.
      {R"({"format":1,"name":"p","author":5,"assets":[{"id":"a","slot":"head"}]})",
       PackError::BadAsset},
      {Pack(R"({"id":"a","slot":"head","pack":5})"), PackError::BadAsset},
      // Duplicate id within the pack.
      {Pack(R"({"id":"a","slot":"head"},{"id":"a","slot":"back"})"),
       PackError::DuplicateAsset},
  };
  for (const auto& c : cases) {
    std::vector<CosmeticAsset> out;
    const PackError got = CosmeticPackLoader::ParsePack(c.text, &out);
    if (got != c.want) {
      printf("FAIL ParseRejections: got %s want %s for %.60s\n",
             PackErrorName(got), PackErrorName(c.want), c.text.c_str());
      ++g_fail;
    }
    CHECK(out.empty());
  }
}

//----------------------------------------------------------------------------
// Registration (atomicity)
//----------------------------------------------------------------------------

static void TestLoadPackTextRegisters() {
  FreshRegistry();
  MemoryScriptFileSystem fs;
  CosmeticPackLoader loader(&fs);
  size_t registered = 0;
  std::string name;
  CHECK(loader.LoadPackText(
            Pack(R"({"id":"p.cape","slot":"back","trail":0.5})"), "mem", &name,
            &registered) == PackError::Ok);
  CHECK(name == "test.pack");
  CHECK(registered == 1);
  CosmeticAsset got;
  CHECK(CosmeticRegistry::Instance().Find("p.cape", &got));
  CHECK(got.pack == "test.pack");
  CHECK(std::fabs(got.trail - 0.5f) < 1e-6f);
}

static void TestLoadPackAtomicOnCollision() {
  FreshRegistry();
  MemoryScriptFileSystem fs;
  CosmeticPackLoader loader(&fs);
  // Seed one id the pack will collide with on its SECOND asset.
  CosmeticAsset seed;
  seed.id = "p.one";
  seed.slot = CosmeticSlot::Head;
  CHECK(CosmeticRegistry::Instance().Register(seed));
  const size_t before = CosmeticRegistry::Instance().Count();

  size_t registered = 0;
  const PackError err = loader.LoadPackText(
      Pack(R"({"id":"p.two","slot":"back"},{"id":"p.one","slot":"head"})"),
      "mem", nullptr, &registered);
  CHECK(err == PackError::DuplicateAsset);
  CHECK(registered == 0);
  // Nothing was added: not even the non-colliding first asset.
  CHECK(CosmeticRegistry::Instance().Count() == before);
  CHECK(!CosmeticRegistry::Instance().Contains("p.two"));
}

static void TestRegisterAllBatchRules() {
  FreshRegistry();
  std::vector<CosmeticAsset> batch;
  CosmeticAsset a;
  a.id = "b.a";
  a.slot = CosmeticSlot::Head;
  CosmeticAsset b = a;
  b.id = "b.b";
  b.slot = CosmeticSlot::Back;
  batch = {a, b};
  CosmeticError err = CosmeticError::Ok;
  CHECK(CosmeticRegistry::Instance().RegisterAll(batch, &err));
  CHECK(err == CosmeticError::Ok);
  CHECK(CosmeticRegistry::Instance().Count() == 2);

  // A batch that repeats an id internally is refused wholesale.
  CosmeticAsset c = a;
  c.id = "b.c";
  CosmeticAsset c2 = c;
  c2.slot = CosmeticSlot::Back;
  CHECK(!CosmeticRegistry::Instance().RegisterAll({c, c2}, &err));
  CHECK(err == CosmeticError::DuplicateId);
  CHECK(!CosmeticRegistry::Instance().Contains("b.c"));

  // A batch colliding with the existing catalog is refused wholesale.
  CosmeticAsset d = a;
  d.id = "b.d";
  CHECK(!CosmeticRegistry::Instance().RegisterAll({d, a}, &err));
  CHECK(err == CosmeticError::DuplicateId);
  CHECK(!CosmeticRegistry::Instance().Contains("b.d"));

  // An empty batch is a no-op success.
  CHECK(CosmeticRegistry::Instance().RegisterAll({}, &err));
}

//----------------------------------------------------------------------------
// Directory load
//----------------------------------------------------------------------------

static void TestLoadDirectory() {
  FreshRegistry();
  MemoryScriptFileSystem fs;
  fs.Write("packs/a.pack.json", Pack(R"({"id":"d.a","slot":"head"})"));
  fs.Write("packs/b.pack.json", Pack(R"({"id":"d.b","slot":"back"})"));
  // Not a pack file: ignored by the suffix filter.
  fs.Write("packs/notes.json", Pack(R"({"id":"d.c","slot":"head"})"));
  fs.Write("packs/readme.txt", "hello");
  // A broken pack must not stop the others.
  fs.Write("packs/bad.pack.json", "{not json");

  CosmeticPackLoader loader(&fs, "packs");
  const PackLoadReport report = loader.LoadDirectory();
  CHECK(report.packsTried == 3);      // a, b, bad
  CHECK(report.packsLoaded == 2);
  CHECK(report.packsFailed == 1);
  CHECK(report.assetsRegistered == 2);
  CHECK(CosmeticRegistry::Instance().Contains("d.a"));
  CHECK(CosmeticRegistry::Instance().Contains("d.b"));
  CHECK(!CosmeticRegistry::Instance().Contains("d.c"));

  // Results are in sorted path order and carry the failure.
  CHECK(report.results.size() == 3);
  CHECK(report.results[0].path == "packs/a.pack.json");
  CHECK(report.results[2].path == "packs/bad.pack.json");
  CHECK(report.results[2].error == PackError::BadJson);
}

static void TestLoadPackFileUnreadable() {
  MemoryScriptFileSystem fs;
  CosmeticPackLoader loader(&fs);
  std::string name;
  size_t registered = 0;
  CHECK(loader.LoadPackFile("missing.pack.json", &name, &registered) ==
        PackError::Unreadable);
  CHECK(registered == 0);
  // No filesystem at all: fail closed, no crash.
  CosmeticPackLoader noFs(nullptr);
  CHECK(noFs.LoadPackFile("x.pack.json") == PackError::Unreadable);
  CHECK(noFs.LoadDirectory().packsTried == 0);
}

//----------------------------------------------------------------------------
// Binding
//----------------------------------------------------------------------------

static void TestReloadPacksBinding() {
  FreshRegistry();
  MemoryScriptFileSystem fs;
  fs.Write("packs/one.pack.json", Pack(R"({"id":"r.a","slot":"head"})"));
  CosmeticPackLoader loader(&fs, "packs");

  ScriptApi::Services services;
  services.packs = &loader;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);
  CHECK(bindings.Has("cosmetic.reload_packs"));

  ScriptValue out;
  // Granted: returns the number of packs loaded.
  CHECK(CallBinding(bindings, "cosmetic.reload_packs", {}, {"cosmetic"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsNumber() && out.AsNumber() == 1.0);
  CHECK(CosmeticRegistry::Instance().Contains("r.a"));

  // Not granted: denied, and nothing loaded.
  FreshRegistry();
  ScriptValue denied;
  CHECK(CallBinding(bindings, "cosmetic.reload_packs", {}, {}, &denied) ==
        ScriptStatus::CapabilityDenied);
  CHECK(!CosmeticRegistry::Instance().Contains("r.a"));

  // No loader: fail closed with a raised error (mirrors the other
  // "service unavailable" bindings), and nothing is loaded.
  ScriptApi::Services none;
  ScriptApi bare(none);
  ScriptBindings bareBindings;
  bare.InstallBindings(&bareBindings);
  ScriptValue zero;
  CHECK(CallBinding(bareBindings, "cosmetic.reload_packs", {}, {"cosmetic"},
                    &zero) == ScriptStatus::Error);
  CHECK(CosmeticRegistry::Instance().Count() == 0);
}

//----------------------------------------------------------------------------
// Real filesystem
//----------------------------------------------------------------------------

static void TestRealFileSystemListDirAndLoad() {
  // Exercises RealScriptFileSystem: it must list only regular files, and the
  // loader must pick up exactly the *.pack.json ones from a real directory.
  char tmpl[] = "/tmp/berry_pack_XXXXXX";
  char* dir = mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (!dir) return;
  const std::string root = dir;

  auto write = [&](const std::string& name, const std::string& body) {
    std::ofstream out(root + "/" + name, std::ios::binary);
    out << body;
  };
  write("a.pack.json", Pack(R"({"id":"rf.a","slot":"head"})"));
  write("b.pack.json", Pack(R"({"id":"rf.b","slot":"back"})"));
  write("skip.json", Pack(R"({"id":"rf.c","slot":"head"})"));
  write("note.txt", "hi");
  // A subdirectory named like a pack file must NOT be listed as a file.
  CHECK(mkdir((root + "/dir.pack.json").c_str(), 0700) == 0);

  RealScriptFileSystem fs;
  const std::vector<std::string> listed = fs.ListDir(root);
  CHECK(listed.size() == 4);   // a, b, skip, note (not the directory)
  CHECK(listed[0] == "a.pack.json");
  CHECK(listed[1] == "b.pack.json");

  FreshRegistry();
  CosmeticPackLoader loader(&fs, root);
  const PackLoadReport report = loader.LoadDirectory();
  CHECK(report.packsTried == 2);
  CHECK(report.packsLoaded == 2);
  CHECK(CosmeticRegistry::Instance().Contains("rf.a"));
  CHECK(CosmeticRegistry::Instance().Contains("rf.b"));
  CHECK(!CosmeticRegistry::Instance().Contains("rf.c"));

  // A missing directory is not an error: no packs, no crash.
  CHECK(fs.ListDir("/tmp/berry_no_such_dir_9999").empty());

  std::remove((root + "/a.pack.json").c_str());
  std::remove((root + "/b.pack.json").c_str());
  std::remove((root + "/skip.json").c_str());
  std::remove((root + "/note.txt").c_str());
  rmdir((root + "/dir.pack.json").c_str());
  rmdir(root.c_str());
}

static void TestShippedSamplePack() {
  // The example under samples/ must stay a valid pack, so an author copying it
  // gets a working starting point. `make test` runs from the repo root.
  RealScriptFileSystem fs;
  std::string text;
  if (!fs.ReadFile("samples/winter.pack.json", &text)) {
    printf("FAIL: samples/winter.pack.json is missing (run from repo root)\n");
    ++g_fail;
    return;
  }
  std::vector<CosmeticAsset> assets;
  std::string name, detail;
  const PackError err = CosmeticPackLoader::ParsePack(text, &assets, &name, &detail);
  if (err != PackError::Ok) {
    printf("FAIL: shipped sample pack invalid: %s (%s)\n", PackErrorName(err),
           detail.c_str());
    ++g_fail;
    return;
  }
  CHECK(name == "creator.winter");
  CHECK(assets.size() == 2);
  CHECK(assets[0].slot == CosmeticSlot::Back);
  CHECK(assets[1].slot == CosmeticSlot::Head);
}

static void TestLoadDirectoryTruncates() {
  // More pack files than the cap: the first kMaxPacksPerLoad load, the rest are
  // reported as truncation rather than silently dropped.
  FreshRegistry();
  MemoryScriptFileSystem fs;
  const size_t total = CosmeticPackLoader::kMaxPacksPerLoad + 3;
  for (size_t i = 0; i < total; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "packs/%04zu.pack.json", i);
    fs.Write(name, Pack(R"({"id":"t.x","slot":"head"})"));
  }
  CosmeticPackLoader loader(&fs, "packs");
  const PackLoadReport report = loader.LoadDirectory();
  CHECK(report.truncated);
  CHECK(report.packsTried == CosmeticPackLoader::kMaxPacksPerLoad);
  // The first pack registers t.x; the rest collide with it, so only 1 loads.
  CHECK(report.packsLoaded == 1);
  CHECK(report.results.size() == CosmeticPackLoader::kMaxPacksPerLoad + 1);
  CHECK(report.results.back().error == PackError::TooManyPacks);
}

static void TestLoadDetailAndCaps() {
  FreshRegistry();
  MemoryScriptFileSystem fs;
  CosmeticPackLoader loader(&fs);

  // A rejected pack reports a reason.
  std::string detail;
  size_t registered = 0;
  CHECK(loader.LoadPackText(Pack(R"({"id":"a","slot":"hat"})"), "mem", nullptr,
                            &registered, &detail) == PackError::BadAsset);
  CHECK(!detail.empty());

  // An unreadable file reports a reason.
  detail.clear();
  CHECK(loader.LoadPackFile("nope.pack.json", nullptr, &registered, &detail) ==
        PackError::Unreadable);
  CHECK(!detail.empty());

  // A pack over the per-pack asset cap is refused.
  std::string assets = "{\"id\":\"c0\",\"slot\":\"head\"}";
  for (size_t i = 1; i <= CosmeticPackLoader::kMaxAssetsPerPack; ++i) {
    char item[64];
    std::snprintf(item, sizeof(item), ",{\"id\":\"c%zu\",\"slot\":\"head\"}", i);
    assets += item;
  }
  CHECK(CosmeticPackLoader::ParsePack(Pack(assets), nullptr) ==
        PackError::TooManyAssets);
}

int main() {
  TestParseFullAsset();
  TestParseMinimalAssetDefaults();
  TestParseRejections();
  TestLoadPackTextRegisters();
  TestLoadPackAtomicOnCollision();
  TestRegisterAllBatchRules();
  TestLoadDirectory();
  TestLoadDirectoryTruncates();
  TestLoadPackFileUnreadable();
  TestRealFileSystemListDirAndLoad();
  TestShippedSamplePack();
  TestLoadDetailAndCaps();
  TestReloadPacksBinding();
  if (g_fail == 0) printf("test_pack: ALL OK\n");
  return g_fail;
}
