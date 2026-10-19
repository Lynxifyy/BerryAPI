#pragma once
#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/game.h"
#include "berry/event.h"
#include <functional>
#include <string>
#include <vector>
#include <mutex>

namespace Berry {

//----------------------------------------------------------------------------
// Berry::Render module.
//
// Presents debug overlays and auxiliary UI through a render seam. The genuine
// Dear ImGui integration (GLES3 backend, eglSwapBuffers hook, input capture)
// lives behind the seam; the host shim provides a "null" (measurement-only)
// implementation. The API here is immediate-mode: every frame the module
// clears the draw list and re-renders whatever is active.
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

//----------------------------------------------------------------------------
// The strict module.
//----------------------------------------------------------------------------

struct RenderStats {
  u64 frameCount = 0;
  f32 frameMs = 0;
  u64 drawCalls = 0;
  size_t primitives = 0;
};

class BERRY_API Renderer {
 public:
  Renderer() = default;
  ~Renderer();

  // Lifecycle
  void Attach(RenderSeam* seam);
  /// Re-binds the render seam at runtime (test harness or device adapter swap).
  void BindSeam(RenderSeam* seam) { seam_ = seam; }
  void Detach();

  // Immediate-mode gear
  void BeginFrame(const Vec2& screenSize);
  void EndFrame();  // presents batch via seam, clears it
  void Clear();

  // Draw commands (valid between BeginFrame/EndFrame)
  void DrawLine(const Vec2& a, const Vec2& b, const Rgba& color, f32 thickness = 1);
  void DrawText(const Vec2& pos, const char* text, const Rgba& color, f32 scale = 1);
  void DrawRect(const Rect2D& rect);
  void DrawCircle(const Vec2& c, f32 radius, const Rgba& color, i32 segments = 32);
  void DrawTextCentered(const Vec2& center, const char* text, const Rgba& color, f32 scale);

  // ESP helpers (need camera)
  void SetCamera(const CameraState& cam);
  bool EspBoxForEntity(const EntitySnapshot& ent, const CameraState& cam,
                       EspBox* out) const;

  // Debug overlay state
  bool showOverlay() const { return showOverlay_; }
  void SetShowOverlay(bool v) { showOverlay_ = v; }
  const RenderStats& stats() const { return stats_; }

 private:
  RenderSeam* seam_ = nullptr;
  bool inFrame_ = false;
  bool showOverlay_ = true;

  std::vector<Line2D> lines_;
  std::vector<Text2D> texts_;
  std::vector<Rect2D> rects_;
  std::vector<Circle2D> circles_;
  mutable std::mutex mutex_;
  CameraState cam_;
  RenderStats stats_;

  void PresentLocked();
};

}  // namespace Berry