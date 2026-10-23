#pragma once
#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/game.h"
#include "berry/perf.h"
#include <string>
#include <vector>

namespace Berry {

//----------------------------------------------------------------------------
// Berry::Render — the draw primitives and the seams they flow through.
//
// These types are the vocabulary the render module and the pipeline share: the
// 2D primitives a frame queues, the world-space billboard, the render seam the
// batch presents through, the camera, and the matrices. They live in their own
// header so the pipeline (which coalesces the primitives into a vertex stream)
// can name them without depending on the module, and the module can own a
// pipeline without a cycle.
//
// Presenting is immediate-mode: every frame the module clears the draw list and
// re-renders whatever is active. The genuine Dear ImGui integration (GLES3
// backend, eglSwapBuffers hook, input capture) lives behind the seam; the host
// shim provides a "null" (measurement-only) implementation.
//----------------------------------------------------------------------------

// Colors as RGBA bytes for the 2D API.
struct BERRY_API Rgba {
  u8 r, g, b, a;
  Rgba() : r(255), g(255), b(255), a(255) {}
  Rgba(u8 r_, u8 g_, u8 b_, u8 a_ = 255) : r(r_), g(g_), b(b_), a(a_) {}
  static Rgba Red()    { return Rgba(255, 60, 60); }
  static Rgba Green()  { return Rgba(60, 255, 120); }
  static Rgba Blue()   { return Rgba(80, 140, 255); }
  static Rgba Yellow() { return Rgba(255, 210, 60); }
  static Rgba White()  { return Rgba(255, 255, 255); }
  static Rgba Black()  { return Rgba(20, 20, 20); }
  Rgba WithAlpha(u8 alpha) const { return Rgba(r, g, b, alpha); }
};

struct BERRY_API Line2D {
  Vec2 a, b;
  Rgba color;
  f32 thickness = 1.0f;
};

struct BERRY_API Text2D {
  Vec2 pos;
  std::string text;
  Rgba color;
  f32 scale = 1.0f;
};

struct BERRY_API Rect2D {
  Vec2 min, max;
  Rgba color;
  Rgba fill;
  bool filled = false;
  f32 thickness = 1.0f;
};

struct BERRY_API Circle2D {
  Vec2 center;
  f32 radius;
  Rgba color;
  i32 segments = 32;
};

struct BERRY_API EspBox {
  EntityId id;
  Vec2 screenMin, screenMax;
  Rgba color;
  std::string label;
};

/// A world-space quad the renderer projects with the current camera. This is
/// how world geometry (a cosmetic asset on a bone, a nameplate, a marker)
/// enters the overlay: the caller supplies a world centre and a world size, and
/// the renderer turns it into the same 2D batch every other draw uses, so it
/// presents through the one RenderSeam. Nothing here is 3D state the device
/// backend must learn: a billboard is a quad facing the camera.
struct BERRY_API Billboard3D {
  Vec3 position;             // world centre
  Vec2 size;                 // world width/height, in blocks
  /// Screen-space roll, in degrees, about the quad's centre. 0 leaves the quad
  /// upright. A caller approximating a world orientation maps it here.
  f32 rollDeg = 0.0f;
  Rgba tint;                 // fill and outline colour
  u64 geometry = 0;          // opaque asset handle, for the device backend
  u64 texture = 0;           // opaque asset handle, for the device backend
  EntityId entity;           // whose billboard this is
};

class BERRY_API RenderSeam {
 public:
  virtual ~RenderSeam() = default;
  /// Called once per frame after presenting the batch. `screenSize` is in
  /// physical pixels.
  virtual void Present(const std::vector<Line2D>& lines,
                       const std::vector<Text2D>& texts,
                       const std::vector<Rect2D>& rects,
                       const std::vector<Circle2D>& circles,
                       const Vec2& screenSize) = 0;
};

// Minimal column-major 4x4 matrix.
struct BERRY_API Mat4 {
  f32 m[16] = {};
  Vec4 operator*(const Vec4& v) const;
  /// Matrix product (column-major, so `a * b` applies `b` first).
  Mat4 operator*(const Mat4& o) const;

  static Mat4 Identity();
  static Mat4 Perspective(f32 fovyDeg, f32 aspect, f32 znear, f32 zfar);
  static Mat4 LookAt(const Vec3& eye, const Vec3& target, const Vec3& up);
};

// Camera state for projecting world points to screen.
struct BERRY_API CameraState {
  Mat4 view;       // world -> view
  Mat4 projection; // view -> NDC (column-major)
  f32  width = 0;  // pixels
  f32  height = 0;
  bool valid = false;

  /// Projects world position to pixel coordinates. Returns false when behind
  /// the camera or out of frustum.
  bool WorldToScreen(const Vec3& world, Vec2* out) const;
};

}  // namespace Berry
