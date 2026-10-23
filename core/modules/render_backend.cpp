// On-device render backend: a runtime-dlsym GLES/EGL overlay driven by the
// engine frame callback.
//
// Design:
//   * Symbols are resolved at runtime from the current process (RTLD_DEFAULT),
//     so the injected library never links GLES/EGL at build time. On a
//     headless host these lookups fail and Start() reports a clean error.
//   * Geometry is rendered with a minimal immediate-mode GLES2 pipeline: one
//     ortho-projected shader, GL_LINES/GL_TRIANGLES batches rebuilt each
//     frame from the Renderer draw list.
//
// This keeps Berry::Render a strict module: no device GL headers are pulled
// into module.h, and the host harness (no GL) can link everything without
// the backend pulling in egl/gles3 at build time.

#include "berry/render_backend.h"
#include "berry/logger.h"
#include "berry/types.h"

#include <dlfcn.h>
#include <cstring>
#include <cstdio>
#include <cstdint>

#if defined(BERRY_PLATFORM_ANDROID) && !defined(BERRY_RENDER_BACKEND_NULL)

namespace Berry {

namespace {

//----------------------------------------------------------------------------
// Function-pointer table resolved via dlsym(RTLD_DEFAULT). Only the subset
// needed for the ortho overlay is declared. GLES2-style signatures only;
// Android's ES3 lib exports the superset under the same names.
//----------------------------------------------------------------------------
struct GlProcs {
  void* (*(eglGetProcAddress))(const char*);

  void (*(glGenBuffers))(int32_t, uint32_t*);
  void (*(glBindBuffer))(uint32_t, uint32_t);
  void (*(glBufferData))(uint32_t, intptr_t, const void*, uint32_t);
  void (*(glGenVertexArrays))(int32_t, uint32_t*);
  void (*(glBindVertexArray))(uint32_t);
  void (*(glEnableVertexAttribArray))(uint32_t);
  void (*(glVertexAttribPointer))(uint32_t, int32_t, uint32_t, uint8_t, int32_t, const void*);
  void (*(glUseProgram))(uint32_t);
  void (*(glUniformMatrix4fv))(int32_t, int32_t, uint8_t, const float*);
  void (*(glUniform4f))(int32_t, float, float, float, float);
  void (*(glEnable))(uint32_t);
  void (*(glDisable))(uint32_t);
  void (*(glBlendFunc))(uint32_t, uint32_t);
  void (*(glDrawArrays))(uint32_t, int32_t, int32_t);
  void (*(glDeleteBuffers))(int32_t, const uint32_t*);
  void (*(glDeleteVertexArrays))(int32_t, const uint32_t*);
  void (*(glDeleteProgram))(uint32_t);
  void (*(glShaderSource))(uint32_t, int32_t, const char* const*, const int32_t*);
  void (*(glCompileShader))(uint32_t);
  void (*(glAttachShader))(uint32_t, uint32_t);
  void (*(glLinkProgram))(uint32_t);
  void (*(glGetShaderiv))(uint32_t, uint32_t, int32_t*);
  void (*(glGetShaderInfoLog))(uint32_t, int32_t, int32_t*, char*);
  void (*(glGetProgramiv))(uint32_t, uint32_t, int32_t*);
  void (*(glGetProgramInfoLog))(uint32_t, int32_t, int32_t*, char*);
  void (*(glUniform1i))(int32_t, int32_t);
  void (*(glViewport))(int32_t, int32_t, int32_t, int32_t);
  uint32_t (*(glCreateProgram))(void);
  uint32_t (*(glCreateShader))(uint32_t);
  void (*(glDeleteShader))(uint32_t);
  void (*(glBindAttribLocation))(uint32_t, uint32_t, const char*);
  int32_t (*(glGetUniformLocation))(uint32_t, const char*);
};

constexpr int32_t  kGlArrayBuffer      = 0x8892;
constexpr uint32_t kGlStaticDraw       = 0x88E4;
constexpr uint32_t kGlFloat            = 0x1406;
constexpr uint32_t kGlUnsignedByte     = 0x1401;
constexpr uint32_t kGlTriangles        = 0x0004;
constexpr uint32_t kGlLines            = 0x0001;
constexpr uint32_t kGlBlend            = 0x0BE2;
constexpr uint32_t kGlOneMinusSrcAlpha = 0x0303;
constexpr uint32_t kGlSrcAlpha         = 0x0302;
constexpr uint32_t kGlCompileStatus    = 0x8B81;
constexpr uint32_t kGlLinkStatus       = 0x8B82;
constexpr uint32_t kGlVertexShader     = 0x8B31;
constexpr uint32_t kGlFragmentShader   = 0x8B30;

const char* g_vertexSrc =
    "attribute vec2 aPos;\n"
    "attribute vec4 aColor;\n"
    "uniform mat4 uProj;\n"
    "varying vec4 vColor;\n"
    "void main() {\n"
    "  vColor = aColor;\n"
    "  gl_Position = uProj * vec4(aPos, 0.0, 1.0);\n"
    "}\n";
const char* g_fragmentSrc =
    "precision mediump float;\n"
    "varying vec4 vColor;\n"
    "void main() {\n"
    "  gl_FragColor = vColor;\n"
    "}\n";

//----------------------------------------------------------------------------
// A tiny orthographic projection mapping pixel coords [0..w]x[h..0] to NDC.
//----------------------------------------------------------------------------
inline void Ortho(float* m, float w, float h) {
  m[0] = 2.0f / w;  m[4] = 0;         m[8]  = 0; m[12] = -1.0f;
  m[1] = 0;         m[5] = -2.0f / h; m[9]  = 0; m[13] = 1.0f;
  m[2] = 0;         m[6] = 0;         m[10] = 1; m[14] = 0;
  m[3] = 0;         m[7] = 0;         m[11] = 0; m[15] = 1;
}

//----------------------------------------------------------------------------
// GLES overlay backend.
//----------------------------------------------------------------------------
class GlesOverlayBackend final : public IRenderBackend {
 public:
  GlesOverlayBackend() = default;
  ~GlesOverlayBackend() override { Stop(); }

  bool Init() override {
    std::memset(&gl_, 0, sizeof(gl_));
    gl_.eglGetProcAddress =
        (void* (*)(const char*))dlsym(RTLD_DEFAULT, "eglGetProcAddress");
    if (!gl_.eglGetProcAddress) {
      Log::Warn("GlesBackend: no eglGetProcAddress (headless?) — overlay off");
      return false;
    }
    Load("glGenBuffers",   &gl_.glGenBuffers);
    Load("glBindBuffer",   &gl_.glBindBuffer);
    Load("glBufferData",   &gl_.glBufferData);
    Load("glGenVertexArrays", &gl_.glGenVertexArrays);
    Load("glBindVertexArray", &gl_.glBindVertexArray);
    Load("glEnableVertexAttribArray", &gl_.glEnableVertexAttribArray);
    Load("glVertexAttribPointer", &gl_.glVertexAttribPointer);
    Load("glUseProgram",   &gl_.glUseProgram);
    Load("glUniformMatrix4fv", &gl_.glUniformMatrix4fv);
    Load("glUniform4f",    &gl_.glUniform4f);
    Load("glEnable",       &gl_.glEnable);
    Load("glDisable",      &gl_.glDisable);
    Load("glBlendFunc",    &gl_.glBlendFunc);
    Load("glDrawArrays",   &gl_.glDrawArrays);
    Load("glDeleteBuffers",&gl_.glDeleteBuffers);
    Load("glDeleteVertexArrays", &gl_.glDeleteVertexArrays);
    Load("glDeleteProgram",&gl_.glDeleteProgram);
    Load("glShaderSource", &gl_.glShaderSource);
    Load("glCompileShader",&gl_.glCompileShader);
    Load("glAttachShader", &gl_.glAttachShader);
    Load("glLinkProgram",  &gl_.glLinkProgram);
    Load("glGetShaderiv",  &gl_.glGetShaderiv);
    Load("glGetShaderInfoLog", &gl_.glGetShaderInfoLog);
    Load("glGetProgramiv", &gl_.glGetProgramiv);
    Load("glGetProgramInfoLog", &gl_.glGetProgramInfoLog);
    Load("glUniform1i",    &gl_.glUniform1i);
    Load("glViewport",     &gl_.glViewport);
    Load("glCreateProgram",&gl_.glCreateProgram);
    Load("glCreateShader", &gl_.glCreateShader);
    Load("glDeleteShader", &gl_.glDeleteShader);
    Load("glBindAttribLocation", &gl_.glBindAttribLocation);
    Load("glGetUniformLocation", &gl_.glGetUniformLocation);

    if (!gl_.glShaderSource || !gl_.glGenBuffers || !gl_.glDrawArrays) {
      Log::Warn("GlesBackend: partial GLES2 symbols — overlay disabled");
      return false;
    }
    return true;
  }

  bool Start() override {
    if (!running_) {
      if (!Init()) return false;
      // Program build is deferred to first Present; just create buffers.
      gl_.glGenBuffers(1, &vbo_);
      gl_.glGenVertexArrays(1, &vao_);
      running_ = (vbo_ != 0 && vao_ != 0);
    }
    return running_;
  }

  void Stop() override {
    if (!running_) return;
    if (program_) gl_.glDeleteProgram(program_);
    if (vbo_) gl_.glDeleteBuffers(1, &vbo_);
    if (vao_) gl_.glDeleteVertexArrays(1, &vao_);
    vbo_ = vao_ = program_ = 0;
    running_ = false;
  }

  bool running() const override { return running_; }
  const char* name() const override { return "GLES-overlay"; }

  /// The dynamic-resolution scale. The overlay is drawn into a viewport of
  /// `screenSize * scale` physical pixels with the ortho projection still over
  /// the full logical screen, so fewer pixels are shaded and the embedder's
  /// compositor upscales the result. Clamped to (0, 1]; 1.0 is native.
  void SetRenderScale(f32 scale) override {
    renderScale_ = (scale > 0.0f && scale <= 1.0f) ? scale : 1.0f;
  }

  /// Uploads the frame's coalesced geometry and issues the two draw calls.
  /// Triangles first (fills), then lines (strokes), both from the same vertex
  /// format and the same program, so a frame costs two `glDrawArrays` calls no
  /// matter how many primitives the script queued.
  void PresentBatch(const DrawBatch& batch, const Vec2& screenSize) override {
    if (!running_ || !EnsureProgram()) return;
    if (screenSize.x > 0 && screenSize.y > 0) {
      const int32_t vw = (int32_t)(screenSize.x * renderScale_);
      const int32_t vh = (int32_t)(screenSize.y * renderScale_);
      gl_.glViewport(0, 0, vw > 0 ? vw : 1, vh > 0 ? vh : 1);
      Ortho(proj_, screenSize.x, screenSize.y);
    }
    gl_.glUseProgram(program_);
    gl_.glUniformMatrix4fv(projLoc_, 1, 0, proj_);
    gl_.glEnable(kGlBlend);
    gl_.glBlendFunc(kGlSrcAlpha, kGlOneMinusSrcAlpha);
    gl_.glBindVertexArray(vao_);

    if (!batch.triangles.empty()) {
      Upload(batch.triangles);
      gl_.glDrawArrays(kGlTriangles, 0, (int32_t)batch.triangles.size());
    }
    if (!batch.lines.empty()) {
      Upload(batch.lines);
      gl_.glDrawArrays(kGlLines, 0, (int32_t)batch.lines.size());
    }
  }

 private:
  /// Uploads one vertex stream into the shared VBO and re-points the
  /// attributes at it. The stream is interleaved (x, y, r, g, b, a), so the
  /// stride is 6 floats and the colour attribute starts at offset 2.
  void Upload(const std::vector<PipelineVertex>& verts) {
    gl_.glBindBuffer(kGlArrayBuffer, vbo_);
    gl_.glBufferData(kGlArrayBuffer, (intptr_t)(verts.size() * sizeof(PipelineVertex)),
                     verts.data(), kGlStaticDraw);
    const int32_t stride = (int32_t)sizeof(PipelineVertex);
    gl_.glEnableVertexAttribArray(0);
    gl_.glVertexAttribPointer(0, 2, kGlFloat, 0, stride, (const void*)0);
    gl_.glEnableVertexAttribArray(1);
    gl_.glVertexAttribPointer(1, 4, kGlFloat, 0, stride,
                              (const void*)(2 * sizeof(float)));
  }

  /// Builds the shader program on first use. Deferred out of Start() so the
  /// context is current when it runs.
  bool EnsureProgram() {
    if (program_) return true;
    const uint32_t vs = Compile(kGlVertexShader, g_vertexSrc);
    const uint32_t fs = Compile(kGlFragmentShader, g_fragmentSrc);
    if (!vs || !fs) return false;
    program_ = gl_.glCreateProgram ? gl_.glCreateProgram() : 0;
    if (!program_) return false;
    gl_.glAttachShader(program_, vs);
    gl_.glAttachShader(program_, fs);
    gl_.glBindAttribLocation(program_, 0, "aPos");
    gl_.glBindAttribLocation(program_, 1, "aColor");
    gl_.glLinkProgram(program_);
    int32_t ok = 0;
    gl_.glGetProgramiv(program_, kGlLinkStatus, &ok);
    if (!ok) {
      Log::Warn("GlesBackend: program link failed");
      gl_.glDeleteProgram(program_);
      program_ = 0;
      return false;
    }
    projLoc_ = gl_.glGetUniformLocation(program_, "uProj");
    return true;
  }

  uint32_t Compile(uint32_t type, const char* src) {
    if (!gl_.glCreateShader) return 0;
    const uint32_t sh = gl_.glCreateShader(type);
    if (!sh) return 0;
    gl_.glShaderSource(sh, 1, &src, nullptr);
    gl_.glCompileShader(sh);
    int32_t ok = 0;
    gl_.glGetShaderiv(sh, kGlCompileStatus, &ok);
    if (!ok) {
      char log[256] = {0};
      if (gl_.glGetShaderInfoLog) gl_.glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
      Log::Warn("GlesBackend: shader compile failed: %s", log);
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
  uint32_t vbo_ = 0, vao_ = 0, program_ = 0;
  int32_t projLoc_ = -1;
  float proj_[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  bool running_ = false;
  f32 renderScale_ = 1.0f;
};

}  // namespace

IRenderBackend* CreateRenderBackend() { return new GlesOverlayBackend(); }

}  // namespace Berry

#else
// Host / non-Android builds: the backend is a null stub.
#include "berry/render_backend.h"

namespace Berry {

class NullRenderBackend final : public IRenderBackend {
 public:
  bool Init() override { return false; }
  bool Start() override { return false; }
  void Stop() override {}
  void PresentBatch(const DrawBatch&, const Vec2&) override {}
  bool running() const override { return false; }
  const char* name() const override { return "null"; }
};

IRenderBackend* CreateRenderBackend() { return new NullRenderBackend(); }

}  // namespace Berry

#endif

//----------------------------------------------------------------------------
// The seam that turns a frame's primitives into one backend batch. It lives
// outside the platform guard: the host tests use it too, so the batch path is
// the same code on both.
//----------------------------------------------------------------------------
namespace Berry {
namespace {

class BackendSeam final : public RenderSeam {
 public:
  explicit BackendSeam(IRenderBackend* backend) : backend_(backend) {}

  void Present(const std::vector<Line2D>& lines,
               const std::vector<Text2D>& texts,
               const std::vector<Rect2D>& rects,
               const std::vector<Circle2D>& circles,
               const Vec2& screenSize) override {
    (void)texts;   // text is rasterised by the backend's own glyph path
    if (!backend_) return;
    BatchBuilder::Build(lines, rects, circles, &batch_);
    backend_->PresentBatch(batch_, screenSize);
  }

  void SetRenderScale(f32 scale) override {
    if (backend_) backend_->SetRenderScale(scale);
  }

 private:
  IRenderBackend* backend_ = nullptr;
  DrawBatch batch_;
};

}  // namespace

RenderSeam* CreateBackendSeam(IRenderBackend* backend) {
  if (!backend) return nullptr;
  return new BackendSeam(backend);
}

}  // namespace Berry
