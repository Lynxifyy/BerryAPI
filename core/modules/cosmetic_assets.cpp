// Cosmetic asset data: mesh and texture loader. See include/berry/cosmetic_assets.h.
#include "berry/cosmetic_assets.h"
#include "berry/logger.h"
#include "../script/json_min.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace Berry {

using detail::json::Parse;
using detail::json::Value;

//----------------------------------------------------------------------------
// Error names
//----------------------------------------------------------------------------

const char* AssetLoadErrorName(AssetLoadError e) {
  switch (e) {
    case AssetLoadError::Ok: return "ok";
    case AssetLoadError::Unreadable: return "asset unreadable";
    case AssetLoadError::BadJson: return "invalid JSON";
    case AssetLoadError::BadFormat: return "unsupported asset format";
    case AssetLoadError::MissingName: return "asset has no name";
    case AssetLoadError::BadMesh: return "invalid mesh";
    case AssetLoadError::BadTexture: return "invalid texture";
    case AssetLoadError::TooLarge: return "asset too large";
    case AssetLoadError::StoreFull: return "asset store full";
  }
  return "unknown";
}

namespace {

//----------------------------------------------------------------------------
// Strict base64 (standard alphabet, '=' padding). Fail closed: an unknown
// character, a misplaced pad, or a length that is not a multiple of four is an
// error, never a best-effort decode that would corrupt pixels.
//----------------------------------------------------------------------------
bool Base64Decode(const std::string& in, std::vector<u8>* out,
                  std::string* detail) {
  auto val = [](unsigned char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  if (in.size() % 4 != 0) {
    if (detail) *detail = "base64 length is not a multiple of 4";
    return false;
  }
  out->clear();
  out->reserve(in.size() / 4 * 3);
  for (size_t i = 0; i < in.size(); i += 4) {
    const int a = val((unsigned char)in[i]);
    const int b = val((unsigned char)in[i + 1]);
    const bool pad2 = in[i + 2] == '=';
    const bool pad3 = in[i + 3] == '=';
    const int c = pad2 ? 0 : val((unsigned char)in[i + 2]);
    const int d = pad3 ? 0 : val((unsigned char)in[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) {
      if (detail) *detail = "base64 has an invalid character";
      return false;
    }
    // A pad may only appear in the last group, and only as described.
    if ((pad2 || pad3) && i + 4 != in.size()) {
      if (detail) *detail = "base64 pad before the final group";
      return false;
    }
    if (pad2 && pad3) {   // "==" : one byte
      if (b & 0x0f) {
        if (detail) *detail = "base64 has non-zero trailing bits";
        return false;
      }
      out->push_back((u8)((a << 2) | (b >> 4)));
    } else if (pad3) {    // "=" : two bytes
      if (c & 0x03) {
        if (detail) *detail = "base64 has non-zero trailing bits";
        return false;
      }
      out->push_back((u8)((a << 2) | (b >> 4)));
      out->push_back((u8)(((b & 0x0f) << 4) | (c >> 2)));
    } else {              // three bytes
      out->push_back((u8)((a << 2) | (b >> 4)));
      out->push_back((u8)(((b & 0x0f) << 4) | (c >> 2)));
      out->push_back((u8)(((c & 0x03) << 6) | d));
    }
  }
  return true;
}

/// Printable, non-empty, bounded text (a name that appears in a log line).
bool SafeText(const std::string& s, size_t maxLen) {
  if (s.empty() || s.size() > maxLen) return false;
  for (char ch : s) {
    const unsigned char c = (unsigned char)ch;
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

bool OnlyKeys(const Value& obj, const std::set<std::string>& allowed,
              std::string* detail) {
  for (const auto& kv : obj.object) {
    if (allowed.find(kv.first) == allowed.end()) {
      if (detail) *detail = "unknown key '" + kv.first + "'";
      return false;
    }
  }
  return true;
}

bool ReadFinite(const Value& obj, const char* key, f64* out) {
  const Value* v = obj.Get(key);
  if (!v) return false;
  if (!v->IsNumber() || !std::isfinite(v->number)) return false;
  *out = v->number;
  return true;
}

bool ReadU32Field(const Value& obj, const char* key, u32* out) {
  const Value* v = obj.Get(key);
  if (!v) return false;
  if (!v->IsNumber() || !std::isfinite(v->number)) return false;
  if (v->number < 0 || v->number > 4294967295.0) return false;
  if (v->number != std::floor(v->number)) return false;
  *out = (u32)v->number;
  return true;
}

const std::set<std::string>& MeshKeys() {
  static const std::set<std::string> k = {"format", "name", "vertices", "indices"};
  return k;
}
const std::set<std::string>& VertexKeys() {
  static const std::set<std::string> k = {"x", "y", "z", "nx", "ny", "nz", "u", "v"};
  return k;
}
const std::set<std::string>& TextureKeys() {
  static const std::set<std::string> k = {"format", "name", "width", "height", "rgba"};
  return k;
}

bool EndsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

//----------------------------------------------------------------------------
// Store
//----------------------------------------------------------------------------

CosmeticAssetStore& CosmeticAssetStore::Instance() {
  static CosmeticAssetStore instance;
  return instance;
}

GeometryId CosmeticAssetStore::AddMesh(const MeshData& m) {
  if (!m.valid()) return 0;
  if (m.vertices.size() > kMaxVertices || m.indices.size() > kMaxIndices) return 0;
  std::lock_guard<std::mutex> lk(mutex_);
  if (meshes_.size() >= kMaxMeshes) return 0;
  const GeometryId id = nextMesh_++;
  meshes_[id] = m;
  return id;
}

TextureId CosmeticAssetStore::AddTexture(const TextureData& t) {
  if (!t.valid()) return 0;
  if (t.width > kMaxTextureDim || t.height > kMaxTextureDim) return 0;
  std::lock_guard<std::mutex> lk(mutex_);
  if (textures_.size() >= kMaxTextures) return 0;
  const TextureId id = nextTexture_++;
  textures_[id] = t;
  return id;
}

bool CosmeticAssetStore::Mesh(GeometryId id, MeshData* out) const {
  if (id == 0) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = meshes_.find(id);
  if (it == meshes_.end()) return false;
  if (out) *out = it->second;
  return true;
}

bool CosmeticAssetStore::Texture(TextureId id, TextureData* out) const {
  if (id == 0) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = textures_.find(id);
  if (it == textures_.end()) return false;
  if (out) *out = it->second;
  return true;
}

bool CosmeticAssetStore::HasMesh(GeometryId id) const {
  if (id == 0) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  return meshes_.find(id) != meshes_.end();
}

bool CosmeticAssetStore::HasTexture(TextureId id) const {
  if (id == 0) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  return textures_.find(id) != textures_.end();
}

size_t CosmeticAssetStore::meshCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return meshes_.size();
}

size_t CosmeticAssetStore::textureCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return textures_.size();
}

void CosmeticAssetStore::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  meshes_.clear();
  textures_.clear();
  builtinGeometry_.clear();
  builtinTextures_.clear();
}

//----------------------------------------------------------------------------
// Procedural built-in geometry
//----------------------------------------------------------------------------

namespace {

/// A flat, subdivided quad in the XY plane spanning [-w/2, w/2] x [-h/2, h/2],
/// facing +Z. `nx`/`ny` subdivide it so a trailing sway deforms smoothly.
MeshData MakePlane(f32 w, f32 h, u32 nx, u32 ny) {
  MeshData m;
  m.name = "builtin.plane";
  m.vertices.reserve((size_t)(nx + 1) * (ny + 1));
  for (u32 y = 0; y <= ny; ++y) {
    for (u32 x = 0; x <= nx; ++x) {
      MeshVertex v;
      const f32 u = (f32)x / (f32)nx;
      const f32 t = (f32)y / (f32)ny;
      v.position = Vec3((u - 0.5f) * w, (t - 0.5f) * h, 0);
      v.normal = Vec3(0, 0, 1);
      v.uv = Vec2(u, t);
      m.vertices.push_back(v);
    }
  }
  for (u32 y = 0; y < ny; ++y) {
    for (u32 x = 0; x < nx; ++x) {
      const u32 i = y * (nx + 1) + x;
      m.indices.push_back(i);
      m.indices.push_back(i + 1);
      m.indices.push_back(i + nx + 1);
      m.indices.push_back(i + 1);
      m.indices.push_back(i + nx + 2);
      m.indices.push_back(i + nx + 1);
    }
  }
  m.bounds = Aabb(Vec3(-w * 0.5f, -h * 0.5f, 0), Vec3(w * 0.5f, h * 0.5f, 0));
  return m;
}

/// A unit cube centred on the origin, with per-face normals and UVs.
MeshData MakeBox(f32 sx, f32 sy, f32 sz) {
  MeshData m;
  m.name = "builtin.box";
  const f32 hx = sx * 0.5f, hy = sy * 0.5f, hz = sz * 0.5f;
  struct Face { Vec3 n; Vec3 c[4]; };
  const Face faces[6] = {
      {Vec3(0, 0, 1), {Vec3(-hx, -hy, hz), Vec3(hx, -hy, hz), Vec3(hx, hy, hz), Vec3(-hx, hy, hz)}},
      {Vec3(0, 0, -1), {Vec3(hx, -hy, -hz), Vec3(-hx, -hy, -hz), Vec3(-hx, hy, -hz), Vec3(hx, hy, -hz)}},
      {Vec3(1, 0, 0), {Vec3(hx, -hy, hz), Vec3(hx, -hy, -hz), Vec3(hx, hy, -hz), Vec3(hx, hy, hz)}},
      {Vec3(-1, 0, 0), {Vec3(-hx, -hy, -hz), Vec3(-hx, -hy, hz), Vec3(-hx, hy, hz), Vec3(-hx, hy, -hz)}},
      {Vec3(0, 1, 0), {Vec3(-hx, hy, hz), Vec3(hx, hy, hz), Vec3(hx, hy, -hz), Vec3(-hx, hy, -hz)}},
      {Vec3(0, -1, 0), {Vec3(-hx, -hy, -hz), Vec3(hx, -hy, -hz), Vec3(hx, -hy, hz), Vec3(-hx, -hy, hz)}},
  };
  const Vec2 uv[4] = {Vec2(0, 0), Vec2(1, 0), Vec2(1, 1), Vec2(0, 1)};
  for (const Face& f : faces) {
    const u32 base = (u32)m.vertices.size();
    for (int i = 0; i < 4; ++i) {
      MeshVertex v;
      v.position = f.c[i];
      v.normal = f.n;
      v.uv = uv[i];
      m.vertices.push_back(v);
    }
    m.indices.push_back(base);
    m.indices.push_back(base + 1);
    m.indices.push_back(base + 2);
    m.indices.push_back(base);
    m.indices.push_back(base + 2);
    m.indices.push_back(base + 3);
  }
  m.bounds = Aabb(Vec3(-hx, -hy, -hz), Vec3(hx, hy, hz));
  return m;
}

}  // namespace

GeometryId CosmeticAssetStore::BuiltinGeometry(BuiltinShape shape) {
  const u32 key = (u32)shape;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = builtinGeometry_.find(key);
    if (it != builtinGeometry_.end()) return it->second;
  }
  MeshData m;
  switch (shape) {
    case BuiltinShape::Plane: m = MakePlane(1.0f, 1.0f, 4, 6); break;
    case BuiltinShape::Box:   m = MakeBox(1.0f, 1.0f, 1.0f); break;
    case BuiltinShape::Quad:  m = MakePlane(1.0f, 1.0f, 1, 1); break;
  }
  if (!m.valid()) return 0;
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = builtinGeometry_.find(key);
  if (it != builtinGeometry_.end()) return it->second;   // raced; reuse
  if (meshes_.size() >= kMaxMeshes) return 0;
  const GeometryId id = nextMesh_++;
  meshes_[id] = std::move(m);
  builtinGeometry_[key] = id;
  return id;
}

TextureId CosmeticAssetStore::BuiltinTexture(u32 rgba) {
  {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = builtinTextures_.find(rgba);
    if (it != builtinTextures_.end()) return it->second;
  }
  TextureData t;
  t.name = "builtin.solid";
  t.width = 2;
  t.height = 2;
  const u8 r = (u8)((rgba >> 24) & 0xff);
  const u8 g = (u8)((rgba >> 16) & 0xff);
  const u8 b = (u8)((rgba >> 8) & 0xff);
  const u8 a = (u8)(rgba & 0xff);
  t.rgba = {r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a};
  if (!t.valid()) return 0;
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = builtinTextures_.find(rgba);
  if (it != builtinTextures_.end()) return it->second;
  if (textures_.size() >= kMaxTextures) return 0;
  const TextureId id = nextTexture_++;
  textures_[id] = std::move(t);
  builtinTextures_[rgba] = id;
  return id;
}

//----------------------------------------------------------------------------
// Loader
//----------------------------------------------------------------------------

CosmeticAssetLoader::CosmeticAssetLoader(IScriptFileSystem* fs, std::string root)
    : fs_(fs), root_(std::move(root)) {}

AssetLoadError CosmeticAssetLoader::ParseMesh(const std::string& text,
                                              MeshData* out,
                                              std::string* detail) {
  auto fail = [&](AssetLoadError e, const std::string& why) {
    if (detail) *detail = why;
    return e;
  };
  if (!out) return fail(AssetLoadError::BadMesh, "no output");

  Value doc;
  std::string jerr;
  if (!Parse(text, &doc, &jerr)) return fail(AssetLoadError::BadJson, jerr);
  if (!doc.IsObject()) return fail(AssetLoadError::BadJson, "document is not an object");
  std::string kdetail;
  if (!OnlyKeys(doc, MeshKeys(), &kdetail)) return fail(AssetLoadError::BadJson, kdetail);

  u32 format = 0;
  if (!ReadU32Field(doc, "format", &format)) {
    return fail(AssetLoadError::BadFormat, "'format' must be an integer");
  }
  if (format != kFormatVersion) {
    return fail(AssetLoadError::BadFormat, "unsupported mesh format");
  }

  const Value* name = doc.Get("name");
  if (!name || !name->IsString() || !SafeText(name->str, 128)) {
    return fail(AssetLoadError::MissingName, "'name' must be a non-empty string");
  }

  const Value* verts = doc.Get("vertices");
  if (!verts || !verts->IsArray() || verts->array.empty()) {
    return fail(AssetLoadError::BadMesh, "'vertices' must be a non-empty array");
  }
  if (verts->array.size() > CosmeticAssetStore::kMaxVertices) {
    return fail(AssetLoadError::TooLarge, "too many vertices");
  }

  MeshData m;
  m.name = name->str;
  m.vertices.reserve(verts->array.size());
  for (const Value& v : verts->array) {
    if (!v.IsObject()) return fail(AssetLoadError::BadMesh, "each vertex must be an object");
    std::string vdetail;
    if (!OnlyKeys(v, VertexKeys(), &vdetail)) return fail(AssetLoadError::BadMesh, vdetail);
    f64 x = 0, y = 0, z = 0, nx = 0, ny = 1, nz = 0, u = 0, vv = 0;
    if (!ReadFinite(v, "x", &x) || !ReadFinite(v, "y", &y) || !ReadFinite(v, "z", &z)) {
      return fail(AssetLoadError::BadMesh, "vertex position must be three finite numbers");
    }
    const bool hasN = v.Get("nx") || v.Get("ny") || v.Get("nz");
    if (hasN && (!ReadFinite(v, "nx", &nx) || !ReadFinite(v, "ny", &ny) ||
                 !ReadFinite(v, "nz", &nz))) {
      return fail(AssetLoadError::BadMesh, "vertex normal must be three finite numbers");
    }
    const bool hasUv = v.Get("u") || v.Get("v");
    if (hasUv && (!ReadFinite(v, "u", &u) || !ReadFinite(v, "v", &vv))) {
      return fail(AssetLoadError::BadMesh, "vertex uv must be two finite numbers");
    }
    MeshVertex mv;
    mv.position = Vec3((f32)x, (f32)y, (f32)z);
    Vec3 n((f32)nx, (f32)ny, (f32)nz);
    mv.normal = n.LengthSq() > 1e-12f ? n.Normalized() : Vec3(0, 1, 0);
    mv.uv = Vec2((f32)u, (f32)vv);
    m.vertices.push_back(mv);
  }

  const Value* idx = doc.Get("indices");
  if (!idx || !idx->IsArray() || idx->array.empty()) {
    return fail(AssetLoadError::BadMesh, "'indices' must be a non-empty array");
  }
  if (idx->array.size() % 3 != 0) {
    return fail(AssetLoadError::BadMesh, "index count must be a multiple of 3");
  }
  if (idx->array.size() > CosmeticAssetStore::kMaxIndices) {
    return fail(AssetLoadError::TooLarge, "too many indices");
  }
  m.indices.reserve(idx->array.size());
  for (const Value& i : idx->array) {
    if (!i.IsNumber() || !std::isfinite(i.number) || i.number < 0 ||
        i.number != std::floor(i.number)) {
      return fail(AssetLoadError::BadMesh, "index must be a non-negative integer");
    }
    if (i.number >= (f64)m.vertices.size()) {
      return fail(AssetLoadError::BadMesh, "index out of range");
    }
    m.indices.push_back((u32)i.number);
  }

  // Local-space bounds, so the render path can cull a mesh without walking it.
  Vec3 mn = m.vertices[0].position, mx = m.vertices[0].position;
  for (const auto& v : m.vertices) {
    mn = mn.Min(v.position);
    mx = mx.Max(v.position);
  }
  m.bounds = Aabb(mn, mx);

  *out = std::move(m);
  if (detail) detail->clear();
  return AssetLoadError::Ok;
}

AssetLoadError CosmeticAssetLoader::ParseTexture(const std::string& text,
                                                 TextureData* out,
                                                 std::string* detail) {
  auto fail = [&](AssetLoadError e, const std::string& why) {
    if (detail) *detail = why;
    return e;
  };
  if (!out) return fail(AssetLoadError::BadTexture, "no output");

  Value doc;
  std::string jerr;
  if (!Parse(text, &doc, &jerr)) return fail(AssetLoadError::BadJson, jerr);
  if (!doc.IsObject()) return fail(AssetLoadError::BadJson, "document is not an object");
  std::string kdetail;
  if (!OnlyKeys(doc, TextureKeys(), &kdetail)) return fail(AssetLoadError::BadJson, kdetail);

  u32 format = 0;
  if (!ReadU32Field(doc, "format", &format)) {
    return fail(AssetLoadError::BadFormat, "'format' must be an integer");
  }
  if (format != kFormatVersion) {
    return fail(AssetLoadError::BadFormat, "unsupported texture format");
  }

  const Value* name = doc.Get("name");
  if (!name || !name->IsString() || !SafeText(name->str, 128)) {
    return fail(AssetLoadError::MissingName, "'name' must be a non-empty string");
  }

  u32 w = 0, h = 0;
  if (!ReadU32Field(doc, "width", &w) || !ReadU32Field(doc, "height", &h)) {
    return fail(AssetLoadError::BadTexture, "'width'/'height' must be integers");
  }
  if (w == 0 || h == 0) return fail(AssetLoadError::BadTexture, "zero-sized texture");
  if (w > CosmeticAssetStore::kMaxTextureDim ||
      h > CosmeticAssetStore::kMaxTextureDim) {
    return fail(AssetLoadError::TooLarge, "texture dimensions over the cap");
  }

  const Value* rgba = doc.Get("rgba");
  if (!rgba || !rgba->IsString()) {
    return fail(AssetLoadError::BadTexture, "'rgba' must be a base64 string");
  }
  std::vector<u8> pixels;
  std::string bdetail;
  if (!Base64Decode(rgba->str, &pixels, &bdetail)) {
    return fail(AssetLoadError::BadTexture, bdetail);
  }
  const size_t expected = (size_t)w * (size_t)h * 4u;
  if (pixels.size() != expected) {
    return fail(AssetLoadError::BadTexture,
                "rgba payload does not match width*height*4");
  }

  TextureData t;
  t.name = name->str;
  t.width = w;
  t.height = h;
  t.rgba = std::move(pixels);
  *out = std::move(t);
  if (detail) detail->clear();
  return AssetLoadError::Ok;
}

AssetLoadError CosmeticAssetLoader::LoadMeshFile(const std::string& path,
                                                 GeometryId* out,
                                                 std::string* detail) {
  if (out) *out = 0;
  if (!fs_) {
    if (detail) *detail = "no filesystem seam";
    return AssetLoadError::Unreadable;
  }
  std::string text;
  if (!fs_->ReadFile(path, &text)) {
    if (detail) *detail = "cannot read file";
    return AssetLoadError::Unreadable;
  }
  MeshData m;
  const AssetLoadError err = ParseMesh(text, &m, detail);
  if (err != AssetLoadError::Ok) return err;
  const GeometryId id = CosmeticAssetStore::Instance().AddMesh(m);
  if (id == 0) {
    if (detail) *detail = "store full or mesh over a cap";
    return AssetLoadError::StoreFull;
  }
  if (out) *out = id;
  return AssetLoadError::Ok;
}

AssetLoadError CosmeticAssetLoader::LoadTextureFile(const std::string& path,
                                                    TextureId* out,
                                                    std::string* detail) {
  if (out) *out = 0;
  if (!fs_) {
    if (detail) *detail = "no filesystem seam";
    return AssetLoadError::Unreadable;
  }
  std::string text;
  if (!fs_->ReadFile(path, &text)) {
    if (detail) *detail = "cannot read file";
    return AssetLoadError::Unreadable;
  }
  TextureData t;
  const AssetLoadError err = ParseTexture(text, &t, detail);
  if (err != AssetLoadError::Ok) return err;
  const TextureId id = CosmeticAssetStore::Instance().AddTexture(t);
  if (id == 0) {
    if (detail) *detail = "store full or texture over a cap";
    return AssetLoadError::StoreFull;
  }
  if (out) *out = id;
  return AssetLoadError::Ok;
}

CosmeticAssetLoader::LoadReport CosmeticAssetLoader::LoadDirectory(
    const std::string& root) {
  LoadReport report;
  if (!fs_) return report;
  const std::string dir = root.empty() ? root_ : root;
  if (dir.empty()) return report;

  std::vector<std::string> names = fs_->ListDir(dir);
  // Deterministic order, so a load's handle assignment is reproducible.
  std::sort(names.begin(), names.end());

  size_t tried = 0;
  for (const std::string& name : names) {
    const bool isMesh = EndsWith(name, kMeshSuffix);
    const bool isTex = EndsWith(name, kTextureSuffix);
    if (!isMesh && !isTex) continue;
    if (tried >= kMaxFilesPerLoad) {
      report.truncated = true;
      break;
    }
    ++tried;
    const std::string path = dir + "/" + name;
    std::string detail;
    if (isMesh) {
      GeometryId id = 0;
      if (LoadMeshFile(path, &id, &detail) == AssetLoadError::Ok) {
        ++report.meshes;
      } else {
        ++report.failed;
        Log::Warn("CosmeticAssetLoader: %s refused: %s", path.c_str(), detail.c_str());
      }
    } else {
      TextureId id = 0;
      if (LoadTextureFile(path, &id, &detail) == AssetLoadError::Ok) {
        ++report.textures;
      } else {
        ++report.failed;
        Log::Warn("CosmeticAssetLoader: %s refused: %s", path.c_str(), detail.c_str());
      }
    }
  }
  return report;
}

}  // namespace Berry
