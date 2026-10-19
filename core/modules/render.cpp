#include "berry/render.h"
#include "berry/logger.h"
#include "berry/event.h"
#include <cmath>
#include <algorithm>

namespace Berry {

//----------------------------------------------------------------------------
// Mat4
//----------------------------------------------------------------------------

Vec4 Mat4::operator*(const Vec4& v) const {
  Vec4 out;
  out.x = m[0] * v.x + m[4] * v.y + m[8]  * v.z + m[12] * v.w;
  out.y = m[1] * v.x + m[5] * v.y + m[9]  * v.z + m[13] * v.w;
  out.z = m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w;
  out.w = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w;
  return out;
}

Mat4 Mat4::Identity() {
  Mat4 r;
  r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
  return r;
}

Mat4 Mat4::Perspective(f32 fovyDeg, f32 aspect, f32 znear, f32 zfar) {
  Mat4 r;
  f32 f = 1.0f / std::tan(fovyDeg * 0.5f * Mathf::DEG2RAD);
  r.m[0]  = f / aspect;
  r.m[5]  = f;
  r.m[10] = (zfar + znear) / (znear - zfar);
  r.m[11] = -1.0f;
  r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
  r.m[15] = 0.0f;
  return r;
}

Mat4 Mat4::LookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
  Vec3 f = (target - eye).Normalized();
  Vec3 s = f.Cross(up).Normalized();
  Vec3 u = s.Cross(f);

  Mat4 r = Identity();
  r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;
  r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;
  r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
  r.m[12] = -s.Dot(eye);
  r.m[13] = -u.Dot(eye);
  r.m[14] = f.Dot(eye);
  return r;
}

bool CameraState::WorldToScreen(const Vec3& world, Vec2* out) const {
  if (!valid || !out) return false;
  Vec4 p = projection * (view * Vec4(world.x, world.y, world.z, 1.0f));
  if (p.w <= 0.0001f) return false;   // behind camera
  f32 nx = p.x / p.w;
  f32 ny = p.y / p.w;
  if (nx < -1.0f || nx > 1.0f || ny < -1.0f || ny > 1.0f) return false;
  out->x = (nx * 0.5f + 0.5f) * width;
  out->y = (1.0f - (ny * 0.5f + 0.5f)) * height;
  return true;
}

//----------------------------------------------------------------------------
// Renderer
//----------------------------------------------------------------------------

Renderer::~Renderer() {
  Detach();
}

void Renderer::Attach(RenderSeam* seam) {
  seam_ = seam;
  Log::Info(seam_ ? "Berry::Render attached (seam present)"
                  : "Berry::Render attached (null seam — no output)");
}

void Renderer::Detach() {
  std::lock_guard<std::mutex> lk(mutex_);
  seam_ = nullptr;
  inFrame_ = false;
  lines_.clear();
  texts_.clear();
  rects_.clear();
  circles_.clear();
}

void Renderer::BeginFrame(const Vec2& screenSize) {
  std::lock_guard<std::mutex> lk(mutex_);
  inFrame_ = true;
  stats_.frameCount++;
  cam_.width = screenSize.x;
  cam_.height = screenSize.y;
}

void Renderer::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  lines_.clear();
  texts_.clear();
  rects_.clear();
  circles_.clear();
}

void Renderer::EndFrame() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inFrame_) return;
  if (showOverlay_ && seam_) {
    u64 t0 = NowNs();
    seam_->Present(lines_, texts_, rects_, circles_, Vec2(cam_.width, cam_.height));
    stats_.frameMs = (f32)((NowNs() - t0) / 1e6);
    stats_.primitives = lines_.size() + texts_.size() + rects_.size() + circles_.size();
  }
  lines_.clear();
  texts_.clear();
  rects_.clear();
  circles_.clear();
  inFrame_ = false;
}

void Renderer::DrawLine(const Vec2& a, const Vec2& b, const Rgba& color, f32 thickness) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inFrame_) return;
  lines_.push_back(Line2D{a, b, color, thickness});
}

void Renderer::DrawText(const Vec2& pos, const char* text, const Rgba& color, f32 scale) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inFrame_ || !text) return;
  texts_.push_back(Text2D{pos, text, color, scale});
}

void Renderer::DrawRect(const Rect2D& rect) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inFrame_) return;
  rects_.push_back(rect);
}

void Renderer::DrawCircle(const Vec2& c, f32 radius, const Rgba& color, i32 segments) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inFrame_) return;
  circles_.push_back(Circle2D{c, radius, color, segments});
}

void Renderer::DrawTextCentered(const Vec2& center, const char* text, const Rgba& color, f32 scale) {
  // NOTE: without measuring glyph widths, horizontal centering is approximate
  // (assumes ~8px/char at scale 1). Acceptable for debug overlays.
  f32 approxW = (f32)(text ? strlen(text) : 0) * 8.0f * scale;
  DrawText(Vec2(center.x - approxW * 0.5f, center.y), text, color, scale);
}

void Renderer::SetCamera(const CameraState& cam) {
  std::lock_guard<std::mutex> lk(mutex_);
  cam_ = cam;
}

bool Renderer::EspBoxForEntity(const EntitySnapshot& ent, const CameraState& cam,
                               EspBox* out) const {
  if (!out || !cam.valid) return false;
  // Approximate entity bounds as a 0.6 x 1.8 x 0.6 box centered slightly below
  // the snapshot position and at its feet.
  const f32 hw = 0.3f, hh = 0.9f;
  Vec3 corners[8];
  const Vec3& p = ent.position;
  corners[0] = Vec3(p.x - hw, p.y - hh, p.z - hw);
  corners[1] = Vec3(p.x + hw, p.y - hh, p.z - hw);
  corners[2] = Vec3(p.x - hw, p.y - hh, p.z + hw);
  corners[3] = Vec3(p.x + hw, p.y - hh, p.z + hw);
  corners[4] = Vec3(p.x - hw, p.y + hh, p.z - hw);
  corners[5] = Vec3(p.x + hw, p.y + hh, p.z - hw);
  corners[6] = Vec3(p.x - hw, p.y + hh, p.z + hw);
  corners[7] = Vec3(p.x + hw, p.y + hh, p.z + hw);

  Vec2 screen[8];
  bool anyValid = false;
  for (int i = 0; i < 8; ++i) {
    if (cam.WorldToScreen(corners[i], &screen[i])) anyValid = true;
    else screen[i] = Vec2(0, 0);
  }
  if (!anyValid) return false;

  Vec2 mn(1e9f, 1e9f), mx(-1e9f, -1e9f);
  for (int i = 0; i < 8; ++i) {
    mn.x = std::min(mn.x, screen[i].x);
    mn.y = std::min(mn.y, screen[i].y);
    mx.x = std::max(mx.x, screen[i].x);
    mx.y = std::max(mx.y, screen[i].y);
  }
  out->id = ent.id;
  out->screenMin = mn;
  out->screenMax = mx;
  out->label = ent.name;
  return true;
}

}  // namespace Berry