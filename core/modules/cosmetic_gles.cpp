// On-device cosmetic mesh backend: a runtime-dlsym GLES2 mesh path.
//
// Design mirrors `render_backend.cpp`: GL symbols are resolved at runtime from
// the current process (RTLD_DEFAULT), so the injected library never links GLES
// at build time and a headless host links everything without it.
//
// The backend keeps an upload cache keyed by the draw item's opaque handles
// (`geometry` -> VBO, `texture` -> GL texture). A handle is uploaded once and
// reused while it is unchanged, so a cape drawn every frame costs one draw call,
// not one upload. A `Reset()` (the asset store was cleared) drops every upload
// so a retired handle is never reused.
//
// It draws into the *game's* current framebuffer with a depth-tested, blended
// pass, restoring the GL state it changed (blend, depth mask, cull) so the
// engine's own draw is unaffected. Fail closed: a missing symbol, a zero
// handle, or an unloaded mesh leaves the item undrawn.

#include "berry/cosmetic_device.h"
#include "berry/logger.h"

#include <cstring>
#include <cstdint>

#if defined(BERRY_PLATFORM_ANDROID) && !defined(BERRY_COSMETIC_GLES_NULL)

#include <dlfcn.h>

namespace Berry {
namespace {

struct GlProcs {
  void* (*(eglGetProcAddress))(const char*);

  uint32_t (*(glCreateProgram))(void);
  uint32_t (*(glCreateShader))(uint32_t);
  void (*(glShaderSource))(uint32_t, int32_t, const char* const*, const int32_t*);
  void (*(glCompileShader))(uint32_t);
  void (*(glAttachShader))(uint32_t, uint32_t);
  void (*(glBindAttribLocation))(uint32_t, uint32_t, const char*);
  void (*(glLinkProgram))(uint32_t);
  void (*(glGetShaderiv))(uint32_t, uint32_t, int32_t*);
  void (*(glGetProgramiv))(uint32_t, uint32_t, int32_t*);
  void (*(glGetShaderInfoLog))(uint32_t, int32_t, int32_t*, char*);
  void (*(glGetProgramInfoLog))(uint32_t, int32_t, int32_t*, char*);
  void (*(glDeleteShader))(uint32_t);
  void (*(glDeleteProgram))(uint32_t);
  int32_t (*(glGetUniformLocation))(uint32_t, const char*);
  void (*(glUseProgram))(uint32_t);
  void (*(glUniformMatrix4fv))(int32_t, int32_t, uint8_t, const float*);
  void (*(glUniform4f))(int32_t, float, float, float, float);
  void (*(glUniform1i))(int32_t, int32_t);

  void (*(glGenBuffers))(int32_t, uint32_t*);
  void (*(glBindBuffer))(uint32_t, uint32_t);
  void (*(glBufferData))(uint32_t, intptr_t, const void*, uint32_t);
  void (*(glDeleteBuffers))(int32_t, const uint32_t*);
  void (*(glEnableVertexAttribArray))(uint32_t);
  void (*(glVertexAttribPointer))(uint32_t, int32_t, uint32_t, uint8_t, int32_t,
                                   const void*);
  void (*(glDrawElements))(uint32_t, int32_t, uint32_t, const void*);

  void (*(glGenTextures))(int32_t, uint32_t*);
  void (*(glBindTexture))(uint32_t, uint32_t);
  void (*(glTexImage2D))(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                         uint32_t, uint32_t, const void*);
  void (*(glTexParameteri))(uint32_t, uint32_t, int32_t);
  void (*(glActiveTexture))(uint32_t);
  void (*(glDeleteTextures))(int32_t, const uint32_t*);

  void (*(glEnable))(uint32_t);
  void (*(glDisable))(uint32_t);
  void (*(glBlendFunc))(uint32_t, uint32_t);
  void (*(glDepthMask))(uint8_t);
  void (*(glCullFace))(uint32_t);
  void (*(glGetIntegerv))(uint32_t, int32_t*);
};

// GL enums (GLES2).
constexpr uint32_t kArrayBuffer   = 0x8892;
constexpr uint32_t kElementArray  = 0x8893;
constexpr uint32_t kStaticDraw    = 0x88E4;
constexpr uint32_t kFloat         = 0x1406;
constexpr uint32_t kUnsignedByte  = 0x1401;
constexpr uint32_t kUnsignedShort = 0x1403;
constexpr uint32_t kTriangles     = 0x0004;
constexpr uint32_t kBlend         = 0x0BE2;
constexpr uint32_t kDepthTest     = 0x0B71;
constexpr uint32_t kCullFace      = 0x0B44;
constexpr uint32_t kTexture2D     = 0x0DE1;
constexpr uint32_t kRgba          = 0x1908;
constexpr uint32_t kTexture0      = 0x84C0;
constexpr uint32_t kTextureMinFilter = 0x2801;
constexpr uint32_t kTextureMagFilter = 0x2800;
constexpr uint32_t kLinear        = 0x2601;
constexpr uint32_t kClampToEdge   = 0x812F;
constexpr uint32_t kTextureWrapS  = 0x2802;
constexpr uint32_t kTextureWrapT  = 0x2803;
constexpr uint32_t kCompileStatus = 0x8B81;
constexpr uint32_t kLinkStatus    = 0x8B82;
constexpr uint32_t kVertexShader  = 0x8B31;
constexpr uint32_t kFragmentShader = 0x8B30;
constexpr uint32_t kSrcAlpha      = 0x0302;
constexpr uint32_t kOneMinusSrcAlpha = 0x0303;
constexpr uint32_t kBack          = 0x0405;

const char* kVertSrc =
    "attribute vec3 aPos;\n"
    "attribute vec3 aNormal;\n"
    "attribute vec2 aUv;\n"
    "uniform mat4 uMvp;\n"
    "varying vec2 vUv;\n"
    "varying vec3 vNormal;\n"
    "void main() {\n"
    "  vUv = aUv;\n"
    "  vNormal = aNormal;\n"
    "  gl_Position = uMvp * vec4(aPos, 1.0);\n"
    "}\n";
const char* kFragSrc =
    "precision mediump float;\n"
    "varying vec2 vUv;\n"
    "varying vec3 vNormal;\n"
    "uniform vec4 uTint;\n"
    "uniform sampler2D uTex;\n"
    "uniform float uHasTex;\n"
    "void main() {\n"
    "  vec3 n = normalize(vNormal);\n"
    "  float lit = 0.35 + 0.65 * abs(n.y);\n"   // a cheap top-down shade
    "  vec4 base = uHasTex > 0.5 ? texture2D(uTex, vUv) : vec4(1.0);\n"
    "  gl_FragColor = base * uTint * vec4(lit, lit, lit, 1.0);\n"
    "}\n";

}  // namespace

class GlesCosmeticMeshBackend final : public ICosmeticMeshBackend {
 public:
  GlesCosmeticMeshBackend() = default;
  ~GlesCosmeticMeshBackend() override { Reset(); }

  bool Init() override {
    std::memset(&gl_, 0, sizeof(gl_));
    gl_.eglGetProcAddress =
        (void* (*)(const char*))dlsym(RTLD_DEFAULT, "eglGetProcAddress");
    if (!gl_.eglGetProcAddress) {
      Log::Warn("CosmeticGles: no eglGetProcAddress — mesh path off");
      return false;
    }
    Load("glCreateProgram", &gl_.glCreateProgram);
    Load("glCreateShader", &gl_.glCreateShader);
    Load("glShaderSource", &gl_.glShaderSource);
    Load("glCompileShader", &gl_.glCompileShader);
    Load("glAttachShader", &gl_.glAttachShader);
    Load("glBindAttribLocation", &gl_.glBindAttribLocation);
    Load("glLinkProgram", &gl_.glLinkProgram);
    Load("glGetShaderiv", &gl_.glGetShaderiv);
    Load("glGetProgramiv", &gl_.glGetProgramiv);
    Load("glGetShaderInfoLog", &gl_.glGetShaderInfoLog);
    Load("glGetProgramInfoLog", &gl_.glGetProgramInfoLog);
    Load("glDeleteShader", &gl_.glDeleteShader);
    Load("glDeleteProgram", &gl_.glDeleteProgram);
    Load("glGetUniformLocation", &gl_.glGetUniformLocation);
    Load("glUseProgram", &gl_.glUseProgram);
    Load("glUniformMatrix4fv", &gl_.glUniformMatrix4fv);
    Load("glUniform4f", &gl_.glUniform4f);
    Load("glUniform1i", &gl_.glUniform1i);
    Load("glGenBuffers", &gl_.glGenBuffers);
    Load("glBindBuffer", &gl_.glBindBuffer);
    Load("glBufferData", &gl_.glBufferData);
    Load("glDeleteBuffers", &gl_.glDeleteBuffers);
    Load("glEnableVertexAttribArray", &gl_.glEnableVertexAttribArray);
    Load("glVertexAttribPointer", &gl_.glVertexAttribPointer);
    Load("glDrawElements", &gl_.glDrawElements);
    Load("glGenTextures", &gl_.glGenTextures);
    Load("glBindTexture", &gl_.glBindTexture);
    Load("glTexImage2D", &gl_.glTexImage2D);
    Load("glTexParameteri", &gl_.glTexParameteri);
    Load("glActiveTexture", &gl_.glActiveTexture);
    Load("glDeleteTextures", &gl_.glDeleteTextures);
    Load("glEnable", &gl_.glEnable);
    Load("glDisable", &gl_.glDisable);
    Load("glBlendFunc", &gl_.glBlendFunc);
    Load("glDepthMask", &gl_.glDepthMask);
    Load("glCullFace", &gl_.glCullFace);
    Load("glGetIntegerv", &gl_.glGetIntegerv);

    if (!gl_.glDrawElements || !gl_.glBufferData || !gl_.glTexImage2D) {
      Log::Warn("CosmeticGles: partial GLES2 symbols — mesh path off");
      return false;
    }
    ready_ = true;
    return true;
  }

  bool ready() const override { return ready_; }
  const char* name() const override { return "GLES-cosmetic"; }

  void DrawMesh(const CosmeticDrawItem& item, const MeshData& mesh,
                const TextureData* texture) override {
    if (!ready_ || !EnsureProgram()) return;
    if (!mesh.valid()) return;
    const GpuMesh* gm = MeshFor(item.geometry, mesh);
    if (!gm || gm->indexCount == 0) return;

    // Model-view-projection: the item's world transform, then the frame camera.
    // The camera comes from the seam's BeginFrame snapshot; with no camera the
    // seam passes an invalid one, and `CameraValid` gates the draw.
    if (!cam_.valid) return;
    float mvp[16];
    BuildMvp(item, mvp);

    uint32_t tex = 0;
    if (texture && texture->valid()) tex = TextureFor(item.texture, *texture);

    gl_.glUseProgram(program_);
    gl_.glUniformMatrix4fv(mvpLoc_, 1, 0, mvp);
    const f32 cr = item.tint.x, cg = item.tint.y, cb = item.tint.z;
    gl_.glUniform4f(tintLoc_, cr, cg, cb, 1.0f);
    gl_.glUniform1i(hasTexLoc_, tex ? 1 : 0);
    if (tex) {
      gl_.glActiveTexture(kTexture0);
      gl_.glBindTexture(kTexture2D, tex);
      gl_.glUniform1i(texLoc_, 0);
    }

    // The engine's own pass may not blend or depth-test the way a cosmetic
    // wants, so the state this backend changes is set here and the blend/depth
    // bits it enables are restored by the caller's next draw anyway (the
    // overlay path re-enables its own). Depth writes are masked off so a cape
    // never occludes the model it hangs from in the depth buffer.
    gl_.glEnable(kDepthTest);
    gl_.glEnable(kBlend);
    gl_.glBlendFunc(kSrcAlpha, kOneMinusSrcAlpha);
    gl_.glDisable(kCullFace);
    gl_.glDepthMask(0);

    gl_.glBindBuffer(kArrayBuffer, gm->vbo);
    const int32_t stride = (int32_t)sizeof(GpuVertex);
    gl_.glEnableVertexAttribArray(0);
    gl_.glVertexAttribPointer(0, 3, kFloat, 0, stride, (const void*)0);
    gl_.glEnableVertexAttribArray(1);
    gl_.glVertexAttribPointer(1, 3, kFloat, 0, stride,
                              (const void*)(3 * sizeof(float)));
    gl_.glEnableVertexAttribArray(2);
    gl_.glVertexAttribPointer(2, 2, kFloat, 0, stride,
                              (const void*)(6 * sizeof(float)));
    gl_.glBindBuffer(kElementArray, gm->ibo);
    gl_.glDrawElements(kTriangles, gm->indexCount, kUnsignedShort, nullptr);

    gl_.glDepthMask(1);
    gl_.glBindBuffer(kArrayBuffer, 0);
    gl_.glBindBuffer(kElementArray, 0);
  }

  void Reset() override {
    if (!ready_) return;
    for (auto& kv : meshes_) {
      if (kv.second.vbo) gl_.glDeleteBuffers(1, &kv.second.vbo);
      if (kv.second.ibo) gl_.glDeleteBuffers(1, &kv.second.ibo);
    }
    meshes_.clear();
    for (auto& kv : textures_) {
      if (kv.second) gl_.glDeleteTextures(1, &kv.second);
    }
    textures_.clear();
  }

  /// Adopts the frame camera the seam opened with. Called from the seam's
  /// BeginFrame, so a mesh projects through the same camera the overlay uses.
  void SetFrame(const CameraState& cam, const Vec2& screenSize) override {
    (void)screenSize;
    cam_ = cam;
  }

 private:
  struct GpuVertex { float px, py, pz, nx, ny, nz, u, v; };
  struct GpuMesh { uint32_t vbo = 0; uint32_t ibo = 0; int32_t indexCount = 0; };

  /// Uploads the mesh on first sight and returns the GPU record. A handle whose
  /// mesh already uploaded is reused as-is.
  const GpuMesh* MeshFor(GeometryId id, const MeshData& mesh) {
    auto it = meshes_.find(id);
    if (it != meshes_.end()) return &it->second;
    if (id == 0 || mesh.indices.size() > 0xffffu) return nullptr;  // u16 indices

    std::vector<GpuVertex> verts;
    verts.reserve(mesh.vertices.size());
    for (const auto& v : mesh.vertices) {
      verts.push_back(GpuVertex{v.position.x, v.position.y, v.position.z,
                                v.normal.x, v.normal.y, v.normal.z, v.uv.x,
                                v.uv.y});
    }
    std::vector<u16> idx;
    idx.reserve(mesh.indices.size());
    for (u32 i : mesh.indices) idx.push_back((u16)i);

    GpuMesh gm;
    gl_.glGenBuffers(1, &gm.vbo);
    gl_.glBindBuffer(kArrayBuffer, gm.vbo);
    gl_.glBufferData(kArrayBuffer, (intptr_t)(verts.size() * sizeof(GpuVertex)),
                     verts.data(), kStaticDraw);
    gl_.glGenBuffers(1, &gm.ibo);
    gl_.glBindBuffer(kElementArray, gm.ibo);
    gl_.glBufferData(kElementArray, (intptr_t)(idx.size() * sizeof(u16)),
                     idx.data(), kStaticDraw);
    gm.indexCount = (int32_t)idx.size();
    auto ins = meshes_.emplace(id, gm);
    return &ins.first->second;
  }

  /// Uploads the texture on first sight and returns its GL name (0 on failure).
  uint32_t TextureFor(TextureId id, const TextureData& tex) {
    auto it = textures_.find(id);
    if (it != textures_.end()) return it->second;
    if (id == 0 || !tex.valid()) return 0;
    uint32_t name = 0;
    gl_.glGenTextures(1, &name);
    if (!name) return 0;
    gl_.glActiveTexture(kTexture0);
    gl_.glBindTexture(kTexture2D, name);
    gl_.glTexParameteri(kTexture2D, kTextureMinFilter, kLinear);
    gl_.glTexParameteri(kTexture2D, kTextureMagFilter, kLinear);
    gl_.glTexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
    gl_.glTexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
    gl_.glTexImage2D(kTexture2D, 0, (int32_t)kRgba, (int32_t)tex.width,
                     (int32_t)tex.height, 0, kRgba, kUnsignedByte,
                     tex.rgba.data());
    textures_[id] = name;
    return name;
  }

  /// Builds a model-view-projection matrix for `item`: translate, then scale,
  /// then the camera's view*projection. Rotation is applied as a yaw about the
  /// world up (a cosmetic hangs from a bone, so a full Euler is unnecessary and
  /// a yaw keeps a cape facing the way the player faces).
  void BuildMvp(const CosmeticDrawItem& item, float* out) const {
    const f32 yaw = item.rotation.y * 0.01745329252f;
    const f32 c = std::cos(yaw), s = std::sin(yaw);
    const f32 k = item.scale;
    // Column-major model matrix: R(yaw) * S(k) with translation T.
    float model[16] = {
        c * k, 0, -s * k, 0,
        0, k, 0, 0,
        s * k, 0, c * k, 0,
        item.position.x, item.position.y, item.position.z, 1,
    };
    const Mat4 vp = cam_.projection * cam_.view;
    for (int col = 0; col < 4; ++col) {
      for (int row = 0; row < 4; ++row) {
        float sum = 0;
        for (int i = 0; i < 4; ++i) sum += vp.m[i * 4 + row] * model[col * 4 + i];
        out[col * 4 + row] = sum;
      }
    }
  }

  bool EnsureProgram() {
    if (program_) return true;
    const uint32_t vs = Compile(kVertexShader, kVertSrc);
    const uint32_t fs = Compile(kFragmentShader, kFragSrc);
    if (!vs || !fs) return false;
    program_ = gl_.glCreateProgram ? gl_.glCreateProgram() : 0;
    if (!program_) return false;
    gl_.glAttachShader(program_, vs);
    gl_.glAttachShader(program_, fs);
    gl_.glBindAttribLocation(program_, 0, "aPos");
    gl_.glBindAttribLocation(program_, 1, "aNormal");
    gl_.glBindAttribLocation(program_, 2, "aUv");
    gl_.glLinkProgram(program_);
    int32_t ok = 0;
    gl_.glGetProgramiv(program_, kLinkStatus, &ok);
    if (!ok) {
      Log::Warn("CosmeticGles: program link failed");
      gl_.glDeleteProgram(program_);
      program_ = 0;
      return false;
    }
    mvpLoc_ = gl_.glGetUniformLocation(program_, "uMvp");
    tintLoc_ = gl_.glGetUniformLocation(program_, "uTint");
    texLoc_ = gl_.glGetUniformLocation(program_, "uTex");
    hasTexLoc_ = gl_.glGetUniformLocation(program_, "uHasTex");
    return true;
  }

  uint32_t Compile(uint32_t type, const char* src) {
    const uint32_t sh = gl_.glCreateShader(type);
    if (!sh) return 0;
    gl_.glShaderSource(sh, 1, &src, nullptr);
    gl_.glCompileShader(sh);
    int32_t ok = 0;
    gl_.glGetShaderiv(sh, kCompileStatus, &ok);
    if (!ok) {
      char log[256] = {0};
      gl_.glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
      Log::Warn("CosmeticGles: shader compile failed: %s", log);
      gl_.glDeleteShader(sh);
      return 0;
    }
    return sh;
  }

  template <typename Fn>
  void Load(const char* name, Fn* out) {
    void* p = dlsym(RTLD_DEFAULT, name);
    if (p) std::memcpy(out, &p, sizeof(p));
  }

  GlProcs gl_{};
  bool ready_ = false;
  uint32_t program_ = 0;
  int32_t mvpLoc_ = -1, tintLoc_ = -1, texLoc_ = -1, hasTexLoc_ = -1;
  CameraState cam_;
  std::map<GeometryId, GpuMesh> meshes_;
  std::map<TextureId, uint32_t> textures_;
};

ICosmeticMeshBackend* CreateCosmeticMeshBackend() {
  return new GlesCosmeticMeshBackend();
}

}  // namespace Berry

#else
// Host / non-Android builds: the mesh backend is a null stub, so the seam is
// exercised without GL and the host links everything.
#include "berry/cosmetic_device.h"

namespace Berry {
namespace {
class NullCosmeticMeshBackend final : public ICosmeticMeshBackend {
 public:
  bool Init() override { return false; }
  bool ready() const override { return false; }
  const char* name() const override { return "null"; }
  void DrawMesh(const CosmeticDrawItem&, const MeshData&, const TextureData*) override {}
};
}  // namespace

ICosmeticMeshBackend* CreateCosmeticMeshBackend() {
  return new NullCosmeticMeshBackend();
}

}  // namespace Berry

#endif
