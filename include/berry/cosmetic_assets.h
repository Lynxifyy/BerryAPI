#pragma once
// Cosmetic asset data: the geometry and texture loader behind the render seam.
//
// A cosmetic pack names a `geometry` and a `texture` handle, but nothing in the
// library said where those handles came from. This is the other half of the
// authoring path: mesh and texture documents loaded through the loader's
// `IScriptFileSystem` seam, validated, and registered under the opaque
// `GeometryId`/`TextureId` handles a `CosmeticAsset` already carries.
//
// Two document kinds, both versioned JSON so a creator can author them by hand
// and a device build can route the read through the game's own file layer:
//
//   * `<name>.mesh.json`  — a triangle mesh: vertices (position, normal, UV)
//     and a triangle-list index array.
//   * `<name>.tex.json`   — an RGBA8 texture: width, height and a base64 pixel
//     payload.
//
// Fail closed, wholesale: a document that is malformed, names an unknown key,
// carries a non-finite coordinate, an index out of range, an over-cap count or
// a pixel payload whose length does not match width*height*4 registers NOTHING.
// A half-loaded mesh would render garbage, so validation is complete before a
// handle is handed out.
//
// The store is bounded (mesh/texture counts, vertex/index counts, texture
// dimensions) so a hostile pack cannot exhaust memory, and handles are never
// reused, so a stale `GeometryId` can never silently address a different mesh.

#include "berry/cosmetic.h"
#include "berry/export.h"
#include "berry/loader.h"
#include "berry/math.h"
#include "berry/types.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Berry {

//----------------------------------------------------------------------------
// Loaded data.
//----------------------------------------------------------------------------

/// One mesh vertex: a local-space position, a normal and a UV.
struct BERRY_API MeshVertex {
  Vec3 position;
  Vec3 normal;
  Vec2 uv;
};

/// A loaded triangle mesh.
struct BERRY_API MeshData {
  std::string name;
  std::vector<MeshVertex> vertices;
  std::vector<u32> indices;   // triangle list: 3 per face
  Aabb bounds;                // local-space bounds, for culling
  bool valid() const {
    return !vertices.empty() && !indices.empty() && indices.size() % 3 == 0;
  }
  size_t triangleCount() const { return indices.size() / 3; }
};

/// A loaded RGBA8 texture.
struct BERRY_API TextureData {
  std::string name;
  u32 width = 0;
  u32 height = 0;
  std::vector<u8> rgba;   // width * height * 4, row-major, top-left origin
  bool valid() const {
    return width > 0 && height > 0 &&
           rgba.size() == (size_t)width * (size_t)height * 4u;
  }
};

//----------------------------------------------------------------------------
// Failures.
//----------------------------------------------------------------------------

/// Why a mesh or texture document was refused. Every one of these leaves the
/// store unchanged.
enum class AssetLoadError : u32 {
  Ok = 0,
  Unreadable,     // file missing or unreadable
  BadJson,        // not a well-formed JSON document, or an unknown key
  BadFormat,      // absent or unsupported `format` version
  MissingName,    // absent or empty `name`
  BadMesh,        // a vertex/normal/UV/index is malformed or out of range
  BadTexture,     // width/height/base64 payload is malformed or mismatched
  TooLarge,       // over a mesh/vertex/index/texture-dimension cap
  StoreFull,      // the mesh or texture store is at its cap
};

const char* AssetLoadErrorName(AssetLoadError e);

//----------------------------------------------------------------------------
// Store.
//----------------------------------------------------------------------------

/// A procedural shape the store can build without a file, so the built-in
/// assets draw real geometry on a fresh client (nothing was registered for
/// them, so a cape rendered nothing). Each shape is built once and cached.
enum class BuiltinShape : u32 {
  Plane,   // a subdivided quad in the XY plane — a cape or a pair of wings
  Box,     // a unit cube — a hat
  Quad,    // a camera-facing billboard quad — an aura
};

/// Owns loaded meshes and textures behind opaque handles. Thread-safe: the
/// loader fills it on the script worker while the render thread reads a handle.
///
/// Handles start at 1 and are never reused, so an id that outlives a `Clear()`
/// (a stale draw item) fails to resolve rather than addressing a new mesh.
class BERRY_API CosmeticAssetStore {
 public:
  static CosmeticAssetStore& Instance();

  static constexpr size_t kMaxMeshes = 256;
  static constexpr size_t kMaxTextures = 256;
  static constexpr size_t kMaxVertices = 1u << 16;    // 65536
  static constexpr size_t kMaxIndices = 1u << 18;     // 262144
  static constexpr u32 kMaxTextureDim = 4096;

  /// Registers a mesh, returning its handle, or 0 when it is invalid, over a
  /// cap, or the store is full.
  GeometryId AddMesh(const MeshData& m);
  TextureId AddTexture(const TextureData& t);

  /// A cached procedural mesh for `shape`, built on first use. Returns 0 only
  /// when the store is full. The handle stays valid until `Clear()`, after
  /// which a rebuild returns a fresh handle (the old one is retired).
  GeometryId BuiltinGeometry(BuiltinShape shape);

  /// A cached 2x2 solid-colour texture for `rgba` (0xRRGGBBAA). Built on first
  /// use and keyed by colour, so every built-in asset gets a distinct texture
  /// without a file.
  TextureId BuiltinTexture(u32 rgba);

  /// Looks up a mesh/texture by handle. False for an unknown handle.
  bool Mesh(GeometryId id, MeshData* out) const;
  bool Texture(TextureId id, TextureData* out) const;

  bool HasMesh(GeometryId id) const;
  bool HasTexture(TextureId id) const;

  size_t meshCount() const;
  size_t textureCount() const;

  /// Drops every mesh and texture. Handles stay retired (never reused).
  void Clear();

 private:
  CosmeticAssetStore() = default;

  mutable std::mutex mutex_;
  std::map<GeometryId, MeshData> meshes_;
  std::map<TextureId, TextureData> textures_;
  GeometryId nextMesh_ = 1;
  TextureId nextTexture_ = 1;
  // Cached procedural handles, built on first use. Keyed by shape/colour, so a
  // repeated request reuses one upload. Cleared with the store.
  std::map<u32, GeometryId> builtinGeometry_;
  std::map<u32, TextureId> builtinTextures_;
};

//----------------------------------------------------------------------------
// Loader.
//----------------------------------------------------------------------------

/// Parses and loads mesh/texture documents into `CosmeticAssetStore::Instance()`.
///
/// Reads go through the `IScriptFileSystem` seam, never the host filesystem
/// directly, so a device build routes them through the game's file layer and
/// the host tests need no disk. The loader holds no mutable state beyond the
/// seam and root, so it is safe to share once constructed.
class BERRY_API CosmeticAssetLoader {
 public:
  /// The suffix a discoverable mesh/texture file must carry. The extra
  /// extension keeps a directory load from picking up unrelated JSON.
  static constexpr const char* kMeshSuffix = ".mesh.json";
  static constexpr const char* kTextureSuffix = ".tex.json";
  /// The only document format this build understands.
  static constexpr u32 kFormatVersion = 1;
  /// Caps so a hostile directory cannot exhaust the loader.
  static constexpr size_t kMaxFilesPerLoad = 512;

  /// `fs` is borrowed and must outlive the loader. `root` is the directory
  /// `LoadDirectory()` reads when called with no argument.
  explicit CosmeticAssetLoader(IScriptFileSystem* fs, std::string root = "");

  CosmeticAssetLoader(const CosmeticAssetLoader&) = delete;
  CosmeticAssetLoader& operator=(const CosmeticAssetLoader&) = delete;

  /// Parses and validates a mesh document WITHOUT touching the store. Pure, so
  /// tests can exercise the format independently of the store.
  static AssetLoadError ParseMesh(const std::string& text, MeshData* out,
                                  std::string* detail = nullptr);

  /// Parses and validates a texture document WITHOUT touching the store.
  static AssetLoadError ParseTexture(const std::string& text, TextureData* out,
                                     std::string* detail = nullptr);

  /// Reads, parses and registers a mesh file. On success `out` holds the
  /// handle. `detail` receives a human-readable reason on failure.
  AssetLoadError LoadMeshFile(const std::string& path, GeometryId* out,
                              std::string* detail = nullptr);

  /// Reads, parses and registers a texture file.
  AssetLoadError LoadTextureFile(const std::string& path, TextureId* out,
                                 std::string* detail = nullptr);

  /// The aggregate of a directory load.
  struct LoadReport {
    size_t meshes = 0;
    size_t textures = 0;
    size_t failed = 0;
    bool truncated = false;
  };

  /// Loads every `*.mesh.json` and `*.tex.json` directly under `root` (or the
  /// configured root when `root` is empty). A failed file does not stop the
  /// others.
  LoadReport LoadDirectory(const std::string& root = "");

  const std::string& root() const { return root_; }
  void SetRoot(std::string root) { root_ = std::move(root); }

 private:
  IScriptFileSystem* fs_ = nullptr;
  std::string root_;
};

}  // namespace Berry
