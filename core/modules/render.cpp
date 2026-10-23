#include "berry/render.h"
#include "berry/pipeline.h"
#include "berry/logger.h"
#include "berry/event.h"
#include <cmath>
#include <algorithm>
#include <cstring>

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

Mat4 Mat4::operator*(const Mat4& o) const {
  // Column-major: out.column[j] = this * o.column[j].
  Mat4 r;
  for (int c = 0; c < 4; ++c) {
    for (int row = 0; row < 4; ++row) {
      f32 sum = 0;
      for (int k = 0; k < 4; ++k) {
        sum += m[k * 4 + row] * o.m[c * 4 + k];
      }
      r.m[c * 4 + row] = sum;
    }
  }
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
// Renderer helpers
//----------------------------------------------------------------------------

namespace {

/// Normalizes a rectangle so min <= max on both axes.
Rect2D Normalized(const Rect2D& r) {
  Rect2D out = r;
  if (out.max.x < out.min.x) std::swap(out.min.x, out.max.x);
  if (out.max.y < out.min.y) std::swap(out.min.y, out.max.y);
  return out;
}

/// Liang-Barsky clip of the segment p0..p1 against `c`. Returns false when the
/// segment lies entirely outside; otherwise narrows p0/p1 to the visible part.
/// Exact, unlike the point-in-rect test used for text and circles.
bool ClipSegment(const Rect2D& c, Vec2* p0, Vec2* p1) {
  f32 t0 = 0.0f, t1 = 1.0f;
  const f32 dx = p1->x - p0->x;
  const f32 dy = p1->y - p0->y;
  const f32 p[4] = {-dx, dx, -dy, dy};
  const f32 q[4] = {p0->x - c.min.x, c.max.x - p0->x,
                    p0->y - c.min.y, c.max.y - p0->y};
  for (int i = 0; i < 4; ++i) {
    if (p[i] == 0.0f) {
      // Parallel to this edge: outside it means the whole segment is out.
      if (q[i] < 0.0f) return false;
      continue;
    }
    const f32 r = q[i] / p[i];
    if (p[i] < 0.0f) {
      if (r > t1) return false;
      if (r > t0) t0 = r;
    } else {
      if (r < t0) return false;
      if (r < t1) t1 = r;
    }
  }
  const Vec2 a = *p0;
  p0->x = a.x + t0 * dx;
  p0->y = a.y + t0 * dy;
  p1->x = a.x + t1 * dx;
  p1->y = a.y + t1 * dy;
  return true;
}

/// Estimated pixel box a single line of text occupies, so a clipped draw can
/// decide whether any of it is visible. The renderer does not measure glyphs
/// (see DrawTextCentered), so this is the same ~8px/char approximation.
Rect2D TextBox(const Vec2& pos, const char* text, f32 scale) {
  const size_t n = text ? strlen(text) : 0;
  const f32 w = (f32)n * 8.0f * scale;
  const f32 h = 8.0f * scale;
  Rect2D r;
  r.min = pos;
  r.max = Vec2(pos.x + w, pos.y + h);
  return r;
}

}  // namespace

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
  clips_.clear();
}

void Renderer::BeginFrame(const Vec2& screenSize) {
  std::lock_guard<std::mutex> lk(mutex_);
  inFrame_ = true;
  stats_.frameCount++;
  stats_.clipped = 0;
  cam_.width = screenSize.x;
  cam_.height = screenSize.y;
  // The clip stack is per-frame state; a script's unmatched push must not leak
  // into the next frame.
  clips_.clear();
  // Start the frame pipeline: rebuild the frustum from the bound camera and
  // adopt the governor's settings, so this frame's culls and LOD tiers follow
  // the quality level the governor settled on.
  pipeline_.BeginFrame(cam_, settings_);
}

void Renderer::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  lines_.clear();
  texts_.clear();
  rects_.clear();
  circles_.clear();
  clips_.clear();
}

void Renderer::EndFrame() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inFrame_) return;
  // Account the frame's geometry before presenting: the shape of the batch the
  // backend will upload, and the draw calls it costs (triangles + lines). The
  // pipeline records the same numbers, so its stats describe the real frame
  // rather than a synthetic one.
  lastCounts_ = BatchBuilder::Count(lines_, rects_, circles_);
  pipeline_.SubmitCounts(lastCounts_);
  stats_.drawCalls = (u64)lastCounts_.drawCalls();
  stats_.primitives = lines_.size() + texts_.size() + rects_.size() + circles_.size();
  if (showOverlay_ && seam_) {
    u64 t0 = NowNs();
    seam_->Present(lines_, texts_, rects_, circles_, Vec2(cam_.width, cam_.height));
    stats_.frameMs = (f32)((NowNs() - t0) / 1e6);
  }
  pipeline_.EndFrame();
  lines_.clear();
  texts_.clear();
  rects_.clear();
  circles_.clear();
  clips_.clear();
  inFrame_ = false;
}

bool Renderer::ClipBox(Rect2D* box, bool exact) const {
  if (clips_.empty()) return true;
  Rect2D r = Normalized(*box);
  const Rect2D& c = clips_.back();
  const bool intersects = !(r.max.x < c.min.x || c.max.x < r.min.x ||
                            r.max.y < c.min.y || c.max.y < r.min.y);
  if (!intersects) return false;
  if (exact) {
    r.min.x = std::max(r.min.x, c.min.x);
    r.min.y = std::max(r.min.y, c.min.y);
    r.max.x = std::min(r.max.x, c.max.x);
    r.max.y = std::min(r.max.y, c.max.y);
    *box = r;
  }
  return true;
}

void Renderer::PushClipRect(const Rect2D& rect) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (clips_.size() >= kMaxClipDepth) return;   // fail closed: ignore the push
  Rect2D r = Normalized(rect);
  if (!clips_.empty()) {
    const Rect2D& c = clips_.back();
    r.min.x = std::max(r.min.x, c.min.x);
    r.min.y = std::max(r.min.y, c.min.y);
    r.max.x = std::min(r.max.x, c.max.x);
    r.max.y = std::min(r.max.y, c.max.y);
  }
  clips_.push_back(r);
}

void Renderer::PopClipRect() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!clips_.empty()) clips_.pop_back();
}

bool Renderer::clipRect(Rect2D* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (clips_.empty()) return false;
  if (out) *out = clips_.back();
  return true;
}

size_t Renderer::clipDepth() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return clips_.size();
}

void Renderer::DrawLine(const Vec2& a, const Vec2& b, const Rgba& color, f32 thickness) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inFrame_) return;
  Vec2 p0 = a, p1 = b;
  if (!clips_.empty() && !ClipSegment(clips_.back(), &p0, &p1)) {
    ++stats_.clipped;
    return;
  }
  lines_.push_back(Line2D{p0, p1, color, thickness});
}

void Renderer::DrawText(const Vec2& pos, const char* text, const Rgba& color, f32 scale) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inFrame_ || !text) return;
  Rect2D box = TextBox(pos, text, scale);
  if (!ClipBox(&box, false)) {
    ++stats_.clipped;
    return;
  }
  texts_.push_back(Text2D{pos, text, color, scale});
}

void Renderer::DrawRect(const Rect2D& rect) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inFrame_) return;
  Rect2D r = rect;
  if (!ClipBox(&r, true)) {
    ++stats_.clipped;
    return;
  }
  rects_.push_back(r);
}

void Renderer::DrawCircle(const Vec2& c, f32 radius, const Rgba& color, i32 segments) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inFrame_) return;
  // A circle has no exact clip representation in the batch; keep it when any
  // part could be visible, drop it only when the bounding box misses the clip.
  Rect2D box;
  box.min = Vec2(c.x - radius, c.y - radius);
  box.max = Vec2(c.x + radius, c.y + radius);
  if (!ClipBox(&box, false)) {
    ++stats_.clipped;
    return;
  }
  circles_.push_back(Circle2D{c, radius, color, segments});
}

bool Renderer::ProjectWorld(const Vec3& world, Vec2* screen) const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!cam_.valid || !screen) return false;
  return cam_.WorldToScreen(world, screen);
}

void Renderer::Draw3DLine(const Vec3& a, const Vec3& b, const Rgba& color,
                          f32 thickness) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inFrame_ || !cam_.valid) return;
  Vec2 p0, p1;
  if (!cam_.WorldToScreen(a, &p0) || !cam_.WorldToScreen(b, &p1)) {
    ++stats_.clipped;
    return;
  }
  if (!clips_.empty() && !ClipSegment(clips_.back(), &p0, &p1)) {
    ++stats_.clipped;
    return;
  }
  lines_.push_back(Line2D{p0, p1, color, thickness});
}

void Renderer::Draw3DBox(const Vec3& min, const Vec3& max, const Rgba& color,
                         f32 thickness) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inFrame_ || !cam_.valid) return;
  // The eight corners; the twelve edges are the pairs that differ in exactly
  // one coordinate.
  const Vec3 c[8] = {
      Vec3(min.x, min.y, min.z), Vec3(max.x, min.y, min.z),
      Vec3(min.x, max.y, min.z), Vec3(max.x, max.y, min.z),
      Vec3(min.x, min.y, max.z), Vec3(max.x, min.y, max.z),
      Vec3(min.x, max.y, max.z), Vec3(max.x, max.y, max.z),
  };
  static const int kEdges[12][2] = {
      {0, 1}, {2, 3}, {4, 5}, {6, 7},   // along X
      {0, 2}, {1, 3}, {4, 6}, {5, 7},   // along Y
      {0, 4}, {1, 5}, {2, 6}, {3, 7},   // along Z
  };
  for (const auto& e : kEdges) {
    Vec2 p0, p1;
    if (!cam_.WorldToScreen(c[e[0]], &p0) || !cam_.WorldToScreen(c[e[1]], &p1)) {
      continue;   // a corner behind the camera: skip this edge, not the box
    }
    if (!clips_.empty() && !ClipSegment(clips_.back(), &p0, &p1)) continue;
    lines_.push_back(Line2D{p0, p1, color, thickness});
  }
}

void Renderer::Draw3DText(const Vec3& world, const char* text, const Rgba& color,
                          f32 scale) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inFrame_ || !cam_.valid || !text) return;
  Vec2 p;
  if (!cam_.WorldToScreen(world, &p)) {
    ++stats_.clipped;
    return;
  }
  Rect2D box = TextBox(p, text, scale);
  if (!ClipBox(&box, false)) {
    ++stats_.clipped;
    return;
  }
  texts_.push_back(Text2D{p, text, color, scale});
}

bool Renderer::DrawBillboard(const Billboard3D& bb) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inFrame_ || !cam_.valid) return false;
  if (!(bb.size.x > 0.0f) || !(bb.size.y > 0.0f)) return false;
  // Frustum pre-test: a billboard whose world AABB is entirely outside the view
  // volume cannot project, so skip the projection work. This is the one cull
  // the overlay path needs, and it runs against the same frustum the pipeline
  // built for the frame.
  if (!pipeline_.frustum().Intersects(Aabb::Around(bb.position, Vec3(0, 0, 0)))) {
    return false;
  }

  // Project the centre, then a point offset by half the size along the camera's
  // right and up axes, so the quad's screen size tracks perspective instead of
  // assuming a fixed pixels-per-block. The axes come from the view matrix's
  // rows, which are the world-space right/up of the camera.
  Vec2 center;
  if (!cam_.WorldToScreen(bb.position, &center)) return false;
  const Vec3 right(cam_.view.m[0], cam_.view.m[4], cam_.view.m[8]);
  const Vec3 up(cam_.view.m[1], cam_.view.m[5], cam_.view.m[9]);
  Vec2 rx, ry;
  if (!cam_.WorldToScreen(bb.position + right * (bb.size.x * 0.5f), &rx)) return false;
  if (!cam_.WorldToScreen(bb.position + up * (bb.size.y * 0.5f), &ry)) return false;

  const f32 halfW = std::fabs(rx.x - center.x);
  const f32 halfH = std::fabs(ry.y - center.y);
  if (!(halfW > 0.0f) || !(halfH > 0.0f)) return false;

  // Rotate the half-extents by the roll to get the quad's corners.
  const f32 roll = bb.rollDeg * Mathf::DEG2RAD;
  const f32 cr = std::cos(roll), sr = std::sin(roll);
  const Vec2 ex(halfW * cr, halfW * sr);
  const Vec2 ey(-halfH * sr, halfH * cr);
  Rect2D quad;
  quad.min = Vec2(center.x - std::fabs(ex.x) - std::fabs(ey.x),
                  center.y - std::fabs(ex.y) - std::fabs(ey.y));
  quad.max = Vec2(center.x + std::fabs(ex.x) + std::fabs(ey.x),
                  center.y + std::fabs(ex.y) + std::fabs(ey.y));
  quad.color = bb.tint;
  quad.fill = bb.tint;
  quad.filled = true;
  // A billboard is a quad in the batch, so it is clipped like any other rect.
  if (!ClipBox(&quad, true)) {
    ++stats_.clipped;
    return false;
  }
  rects_.push_back(quad);
  return true;
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

CameraState Renderer::camera() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return cam_;
}

void Renderer::SetRenderSettings(const RenderSettings& s) {
  std::lock_guard<std::mutex> lk(mutex_);
  settings_ = s;
}

RenderSettings Renderer::renderSettings() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return settings_;
}

f32 Renderer::OnFrameTime(f32 frameMs) {
  std::lock_guard<std::mutex> lk(mutex_);
  return pipeline_.OnFrameTime(frameMs);
}

PipelineStats Renderer::pipelineStats() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return pipeline_.stats();
}

DrawCounts Renderer::lastBatchCounts() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return lastCounts_;
}

LodTier Renderer::LodFor(f32 distance) const {
  std::lock_guard<std::mutex> lk(mutex_);
  return pipeline_.lod().TierFor(distance);
}

bool Renderer::Visible(const Aabb& box) const {
  std::lock_guard<std::mutex> lk(mutex_);
  return pipeline_.frustum().Intersects(box);
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