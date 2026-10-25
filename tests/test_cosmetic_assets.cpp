// Host tests for the cosmetic asset loader: mesh and texture documents, the
// bounded store and the fail-closed validation of every field.
#include "berry/cosmetic_assets.h"
#include "berry/loader.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static bool Near(f32 a, f32 b, f32 eps = 1e-4f) {
  return std::fabs(a - b) <= eps;
}

namespace {

/// A minimal valid mesh document: one triangle with three vertices.
std::string TriangleMesh(const std::string& name = "tri") {
  return std::string(R"({
    "format": 1,
    "name": ")") + name + R"(",
    "vertices": [
      {"x": 0, "y": 0, "z": 0, "nx": 0, "ny": 0, "nz": 1, "u": 0, "v": 0},
      {"x": 1, "y": 0, "z": 0, "nx": 0, "ny": 0, "nz": 1, "u": 1, "v": 0},
      {"x": 0, "y": 1, "z": 0, "nx": 0, "ny": 0, "nz": 1, "u": 0, "v": 1}
    ],
    "indices": [0, 1, 2]
  })";
}

/// A 2x2 RGBA texture: 16 bytes, base64 of the 16 bytes 0..15.
std::string TwoByTwoTexture() {
  // Bytes 0..15 base64-encode to "AAECAwQFBgcICQoLDA0ODw==".
  return R"({
    "format": 1,
    "name": "checker",
    "width": 2,
    "height": 2,
    "rgba": "AAECAwQFBgcICQoLDA0ODw=="
  })";
}

}  // namespace

//----------------------------------------------------------------------------
// Mesh parsing
//----------------------------------------------------------------------------

static void TestMeshHappyPath() {
  MeshData m;
  std::string detail;
  CHECK(CosmeticAssetLoader::ParseMesh(TriangleMesh("cape"), &m, &detail) ==
        AssetLoadError::Ok);
  CHECK(m.name == "cape");
  CHECK(m.vertices.size() == 3);
  CHECK(m.indices.size() == 3);
  CHECK(m.triangleCount() == 1);
  CHECK(m.valid());
  CHECK(Near(m.vertices[0].normal.z, 1.0f));
  CHECK(Near(m.vertices[1].uv.x, 1.0f));
  // Bounds span (0,0,0)..(1,1,0).
  CHECK(Near(m.bounds.min.x, 0.0f) && Near(m.bounds.max.x, 1.0f));
  CHECK(Near(m.bounds.max.y, 1.0f) && Near(m.bounds.max.z, 0.0f));
}

static void TestMeshRejectsBadDocuments() {
  MeshData m;
  std::string d;
  auto bad = [&](const std::string& text) {
    return CosmeticAssetLoader::ParseMesh(text, &m, &d) != AssetLoadError::Ok;
  };
  CHECK(bad(""));                                     // empty
  CHECK(bad("not json"));
  CHECK(bad("[]"));                                   // not an object
  CHECK(bad(R"({"format":1,"name":"x","vertices":[],"indices":[0,1,2]})"));
  CHECK(bad(R"({"format":1,"name":"x","vertices":[{"x":0,"y":0,"z":0}],"indices":[]})"));
  // Unknown key fails loudly rather than loading a half-specified mesh.
  CHECK(bad(R"({"format":1,"name":"x","vertices":[{"x":0,"y":0,"z":0,"w":1}],"indices":[0,0,0]})"));
  // A wrong format version is refused, not guessed.
  CHECK(bad(R"({"format":2,"name":"x","vertices":[{"x":0,"y":0,"z":0}],"indices":[0,0,0]})"));
  // An index past the vertex array is out of range.
  CHECK(bad(R"({"format":1,"name":"x","vertices":[{"x":0,"y":0,"z":0}],"indices":[0,0,1]})"));
  // A non-multiple-of-3 index count is refused.
  CHECK(bad(R"({"format":1,"name":"x","vertices":[{"x":0,"y":0,"z":0}],"indices":[0,0]})"));
  // A non-finite coordinate is refused.
  CHECK(bad(R"({"format":1,"name":"x","vertices":[{"x":1e999,"y":0,"z":0}],"indices":[0,0,0]})"));
}

//----------------------------------------------------------------------------
// Texture parsing
//----------------------------------------------------------------------------

static void TestTextureHappyPath() {
  TextureData t;
  std::string detail;
  CHECK(CosmeticAssetLoader::ParseTexture(TwoByTwoTexture(), &t, &detail) ==
        AssetLoadError::Ok);
  CHECK(t.name == "checker");
  CHECK(t.width == 2 && t.height == 2);
  CHECK(t.rgba.size() == 16);
  CHECK(t.rgba[0] == 0 && t.rgba[15] == 15);
  CHECK(t.valid());
}

static void TestTextureRejectsBadDocuments() {
  TextureData t;
  std::string d;
  auto bad = [&](const std::string& text) {
    return CosmeticAssetLoader::ParseTexture(text, &t, &d) != AssetLoadError::Ok;
  };
  CHECK(bad(""));
  CHECK(bad(R"({"format":1,"name":"x","width":2,"height":2})"));  // no rgba
  // A payload whose length does not match width*height*4 is refused.
  CHECK(bad(R"({"format":1,"name":"x","width":4,"height":4,"rgba":"AAECAwQFBgcICQoLDA0ODw=="})"));
  // A bad base64 character is refused.
  CHECK(bad(R"({"format":1,"name":"x","width":1,"height":1,"rgba":"AAAA*A=="})"));
  // A zero dimension is refused.
  CHECK(bad(R"({"format":1,"name":"x","width":0,"height":1,"rgba":""})"));
  // An unknown key is refused.
  CHECK(bad(R"({"format":1,"name":"x","width":1,"height":1,"rgba":"AAAA","fmt":"rgba8"})"));
}

//----------------------------------------------------------------------------
// Store
//----------------------------------------------------------------------------

static void TestStoreHandles() {
  CosmeticAssetStore& store = CosmeticAssetStore::Instance();
  store.Clear();
  CHECK(store.meshCount() == 0);

  MeshData m;
  CHECK(CosmeticAssetLoader::ParseMesh(TriangleMesh(), &m, nullptr) ==
        AssetLoadError::Ok);
  const GeometryId g = store.AddMesh(m);
  CHECK(g != 0);
  CHECK(store.HasMesh(g));
  MeshData back;
  CHECK(store.Mesh(g, &back));
  CHECK(back.name == "tri");

  // A stale handle is never reused after a Clear: it simply fails to resolve.
  store.Clear();
  CHECK(!store.HasMesh(g));
  CHECK(!store.Mesh(g, &back));
  CHECK(store.meshCount() == 0);

  // The next handle is distinct from the retired one.
  const GeometryId g2 = store.AddMesh(m);
  CHECK(g2 != 0 && g2 != g);
}

static void TestStoreRefusesInvalid() {
  CosmeticAssetStore& store = CosmeticAssetStore::Instance();
  store.Clear();
  MeshData bad;
  CHECK(store.AddMesh(bad) == 0);   // no vertices/indices
  TextureData bt;
  CHECK(store.AddTexture(bt) == 0);
}

//----------------------------------------------------------------------------
// File + directory load through the seam
//----------------------------------------------------------------------------

static void TestFileAndDirectoryLoad() {
  MemoryScriptFileSystem fs;
  fs.Write("/packs/cape.mesh.json", TriangleMesh("cape"));
  fs.Write("/packs/checker.tex.json", TwoByTwoTexture());
  fs.Write("/packs/broken.mesh.json", R"({"format":1,"name":"bad","vertices":[],"indices":[]})");
  fs.Write("/packs/notes.txt", "ignored");

  CosmeticAssetStore::Instance().Clear();
  CosmeticAssetLoader loader(&fs, "/packs");
  const auto report = loader.LoadDirectory();
  CHECK(report.meshes == 1);
  CHECK(report.textures == 1);
  CHECK(report.failed == 1);   // the broken pack is refused, the rest load
  CHECK(!report.truncated);
  CHECK(CosmeticAssetStore::Instance().meshCount() == 1);
  CHECK(CosmeticAssetStore::Instance().textureCount() == 1);

  // A direct file load returns the handle.
  GeometryId g = 0;
  std::string detail;
  CHECK(loader.LoadMeshFile("/packs/cape.mesh.json", &g, &detail) ==
        AssetLoadError::Ok);
  CHECK(g != 0);
  // A missing file fails closed.
  CHECK(loader.LoadMeshFile("/packs/nope.mesh.json", &g, &detail) ==
        AssetLoadError::Unreadable);
  CHECK(g == 0);
}

int main() {
  TestMeshHappyPath();
  TestMeshRejectsBadDocuments();
  TestTextureHappyPath();
  TestTextureRejectsBadDocuments();
  TestStoreHandles();
  TestStoreRefusesInvalid();
  TestFileAndDirectoryLoad();
  if (g_fail == 0) printf("test_cosmetic_assets: ALL OK\n");
  return g_fail;
}
