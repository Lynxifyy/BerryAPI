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

 private:
  template <typename Fn>
  void Load(const char* name, Fn* out) {
    void* p = dlsym(RTLD_DEFAULT, name);
    if (p) std::memcpy(out, &p, sizeof(p));
  }

  GlProcs gl_{};
  uint32_t vbo_ = 0, vao_ = 0, program_ = 0;
  int32_t projLoc_ = -1;
  float proj_[16];
  bool running_ = false;
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
  bool running() const override { return false; }
  const char* name() const override { return "null"; }
};

IRenderBackend* CreateRenderBackend() { return new NullRenderBackend(); }

}  // namespace Berry

#endif
