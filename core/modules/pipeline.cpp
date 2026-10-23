#include "berry/pipeline.h"
#include "berry/game.h"

#include <algorithm>
#include <cmath>

namespace Berry {

//----------------------------------------------------------------------------
// Frustum
//----------------------------------------------------------------------------

bool Frustum::SetFromViewProjection(const Mat4& m) {
  // Gribb-Hartmann: the planes are the row combinations of the clip matrix.
  // A point is inside when -w <= x <= w, etc.; the plane coefficients below
  // are (row4 + rowN) for the near side and (row4 - rowN) for the far side.
  auto row = [&m](int r, int c) { return m.m[c * 4 + r]; };

  const Vec4 r0(row(0, 0), row(0, 1), row(0, 2), row(0, 3));
  const Vec4 r1(row(1, 0), row(1, 1), row(1, 2), row(1, 3));
  const Vec4 r2(row(2, 0), row(2, 1), row(2, 2), row(2, 3));
  const Vec4 r3(row(3, 0), row(3, 1), row(3, 2), row(3, 3));

  auto set = [this](Plane p, const Vec4& v) {
    planes_[(size_t)p] = v;
  };
  set(Plane::Left,   Vec4(r3.x + r0.x, r3.y + r0.y, r3.z + r0.z, r3.w + r0.w));
  set(Plane::Right,  Vec4(r3.x - r0.x, r3.y - r0.y, r3.z - r0.z, r3.w - r0.w));
  set(Plane::Bottom, Vec4(r3.x + r1.x, r3.y + r1.y, r3.z + r1.z, r3.w + r1.w));
  set(Plane::Top,    Vec4(r3.x - r1.x, r3.y - r1.y, r3.z - r1.z, r3.w - r1.w));
  set(Plane::Near,   Vec4(r3.x + r2.x, r3.y + r2.y, r3.z + r2.z, r3.w + r2.w));
  set(Plane::Far,    Vec4(r3.x - r2.x, r3.y - r2.y, r3.z - r2.z, r3.w - r2.w));

  // A degenerate matrix yields zero-length normals; refuse rather than cull
  // everything against a garbage plane.
  for (const Vec4& p : planes_) {
    const f32 len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    if (!std::isfinite(len) || len < 1e-6f) {
      valid_ = false;
      planes_.fill(Vec4());
      return false;
    }
  }
  valid_ = true;
  return true;
}

bool Frustum::SetFromCamera(const Mat4& view, const Mat4& projection) {
  return SetFromViewProjection(projection * view);
}

f32 Frustum::Distance(Plane p, const Vec3& point) const {
  const Vec4& pl = planes_[(size_t)p];
  const f32 len = std::sqrt(pl.x * pl.x + pl.y * pl.y + pl.z * pl.z);
  if (len < 1e-6f) return 0.0f;
  return (pl.x * point.x + pl.y * point.y + pl.z * point.z + pl.w) / len;
}

bool Frustum::ContainsPoint(const Vec3& p) const {
  if (!valid_) return true;   // no frustum: cull nothing
  for (const Vec4& pl : planes_) {
    if (pl.x * p.x + pl.y * p.y + pl.z * p.z + pl.w < 0.0f) return false;
  }
  return true;
}

bool Frustum::Contains(const Aabb& box) const {
  if (!valid_) return true;
  for (const Vec4& pl : planes_) {
    // Fully inside: even the box's most negative vertex on this plane's normal
    // must be on the positive side.
    const Vec3 p(pl.x >= 0 ? box.min.x : box.max.x,
                 pl.y >= 0 ? box.min.y : box.max.y,
                 pl.z >= 0 ? box.min.z : box.max.z);
    if (pl.x * p.x + pl.y * p.y + pl.z * p.z + pl.w < 0.0f) return false;
  }
  return true;
}

bool Frustum::Intersects(const Aabb& box) const {
  if (!valid_) return true;
  for (const Vec4& pl : planes_) {
    // Entirely outside this half-space only when even the box's most positive
    // vertex on the normal is negative. A box straddling the plane intersects.
    const Vec3 p(pl.x >= 0 ? box.max.x : box.min.x,
                 pl.y >= 0 ? box.max.y : box.min.y,
                 pl.z >= 0 ? box.max.z : box.min.z);
    if (pl.x * p.x + pl.y * p.y + pl.z * p.z + pl.w < 0.0f) return false;
  }
  return true;
}

bool Frustum::IntersectsSphere(const Vec3& center, f32 radius) const {
  if (!valid_) return true;
  for (size_t i = 0; i < planes_.size(); ++i) {
    if (Distance((Plane)i, center) < -radius) return false;
  }
  return true;
}

//----------------------------------------------------------------------------
// LOD
//----------------------------------------------------------------------------

const char* LodTierName(LodTier t) {
  switch (t) {
    case LodTier::Full: return "full";
    case LodTier::Reduced: return "reduced";
    case LodTier::Static: return "static";
    case LodTier::Culled: return "culled";
  }
  return "unknown";
}

LodPolicy LodPolicy::Default() { return LodPolicy{}; }

LodPolicy LodPolicy::FromSettings(const RenderSettings& s) {
  LodPolicy p;
  // The governor's animation distance is where animation stops paying for
  // itself, so full detail sits at half of it and reduced detail at it. Static
  // is the draw distance; beyond that an entity is culled only when the
  // settings ask for it.
  p.fullDistance = std::max(4.0f, s.animationDistance * 0.5f);
  p.reducedDistance = std::max(p.fullDistance, s.animationDistance);
  p.staticDistance = std::max(p.reducedDistance, s.entityDistance);
  p.cullBeyondStatic = s.cullFarEntities;
  return p;
}

LodTier LodPolicy::TierFor(f32 distance) const {
  if (distance <= fullDistance) return LodTier::Full;
  if (distance <= reducedDistance) return LodTier::Reduced;
  if (distance <= staticDistance) return LodTier::Static;
  return cullBeyondStatic ? LodTier::Culled : LodTier::Static;
}

bool LodPolicy::ShouldTick(LodTier tier, u64 tick, u64 phase) const {
  switch (tier) {
    case LodTier::Full:
      return true;
    case LodTier::Reduced: {
      const u64 interval = reducedTickInterval < 1 ? 1 : (u64)reducedTickInterval;
      // `tick + phase` so two entities with different ids fall on different
      // ticks; without the phase a whole crowd would update on the same frame.
      return ((tick + phase) % interval) == 0;
    }
    case LodTier::Static:
    case LodTier::Culled:
      return false;
  }
  return true;
}

//----------------------------------------------------------------------------
// Dynamic resolution
//----------------------------------------------------------------------------

void DynamicResolution::Configure(const Config& cfg) {
  cfg_ = cfg;
  // A min above max would make the clamp below undefined; put them in order.
  if (cfg_.minScale > cfg_.maxScale) std::swap(cfg_.minScale, cfg_.maxScale);
  if (cfg_.minScale <= 0.0f) cfg_.minScale = 0.05f;
  if (cfg_.maxScale > 1.0f) cfg_.maxScale = 1.0f;
  if (cfg_.framesToReact < 1) cfg_.framesToReact = 1;
  scale_ = Mathf::Clamp(scale_, cfg_.minScale, cfg_.maxScale);
  dirty_ = false;
}

f32 DynamicResolution::OnFrame(f32 frameMs) {
  dirty_ = false;
  if (!cfg_.enabled || !std::isfinite(frameMs) || frameMs <= 0.0f) return scale_;

  const f32 slowAbove = cfg_.targetMs * (1.0f + cfg_.slowMargin);
  const f32 fastBelow = cfg_.targetMs * (1.0f - cfg_.fastMargin);

  if (frameMs > slowAbove) {
    ++slowStreak_;
    fastStreak_ = 0;
  } else if (frameMs < fastBelow) {
    ++fastStreak_;
    slowStreak_ = 0;
  } else {
    // Inside the dead band: hold, and let the streaks decay so an isolated
    // spike between two good frames does not accumulate.
    slowStreak_ = 0;
    fastStreak_ = 0;
  }

  const f32 before = scale_;
  if (slowStreak_ >= cfg_.framesToReact) {
    slowStreak_ = 0;
    scale_ = Mathf::Clamp(scale_ - cfg_.stepDown, cfg_.minScale, cfg_.maxScale);
  } else if (fastStreak_ >= cfg_.framesToReact) {
    fastStreak_ = 0;
    scale_ = Mathf::Clamp(scale_ + cfg_.stepUp, cfg_.minScale, cfg_.maxScale);
  }

  dirty_ = std::fabs(scale_ - before) > 1e-6f;
  return scale_;
}

void DynamicResolution::SetScale(f32 scale) {
  if (!std::isfinite(scale)) return;
  const f32 before = scale_;
  scale_ = Mathf::Clamp(scale, cfg_.minScale, cfg_.maxScale);
  dirty_ = std::fabs(scale_ - before) > 1e-6f;
}

void DynamicResolution::Reset() {
  scale_ = 1.0f;
  slowStreak_ = 0;
  fastStreak_ = 0;
  dirty_ = false;
}

//----------------------------------------------------------------------------
// Batch building
//----------------------------------------------------------------------------

namespace {

PipelineVertex V(f32 x, f32 y, const Rgba& c) {
  PipelineVertex v;
  v.x = x;
  v.y = y;
  v.r = (f32)c.r / 255.0f;
  v.g = (f32)c.g / 255.0f;
  v.b = (f32)c.b / 255.0f;
  v.a = (f32)c.a / 255.0f;
  return v;
}

/// One rect contributes two triangles when filled, plus four outline lines
/// when it has a stroke (an unfilled rect always has one).
inline void CountRect(const Rect2D& r, DrawCounts* c) {
  if (r.filled) c->triangleVertices += 6;
  if (!r.filled || r.thickness > 0.0f) c->lineVertices += 8;
}

inline i32 CircleSegments(i32 segments) {
  if (segments < BatchBuilder::kMinCircleSegments) return BatchBuilder::kMinCircleSegments;
  if (segments > BatchBuilder::kMaxCircleSegments) return BatchBuilder::kMaxCircleSegments;
  return segments;
}

}  // namespace

void BatchBuilder::Build(const std::vector<Line2D>& lines,
                         const std::vector<Rect2D>& rects,
                         const std::vector<Circle2D>& circles,
                         DrawBatch* out) {
  if (!out) return;
  out->Clear();

  out->lines.reserve(lines.size() * 2 + rects.size() * 8);
  out->triangles.reserve(rects.size() * 6 + circles.size() * 3 *
                             (size_t)kMaxCircleSegments);

  for (const Line2D& l : lines) {
    out->lines.push_back(V(l.a.x, l.a.y, l.color));
    out->lines.push_back(V(l.b.x, l.b.y, l.color));
  }

  for (const Rect2D& r : rects) {
    if (r.filled) {
      const PipelineVertex a = V(r.min.x, r.min.y, r.fill);
      const PipelineVertex b = V(r.max.x, r.min.y, r.fill);
      const PipelineVertex c = V(r.max.x, r.max.y, r.fill);
      const PipelineVertex d = V(r.min.x, r.max.y, r.fill);
      out->triangles.push_back(a);
      out->triangles.push_back(b);
      out->triangles.push_back(c);
      out->triangles.push_back(a);
      out->triangles.push_back(c);
      out->triangles.push_back(d);
    }
    if (!r.filled || r.thickness > 0.0f) {
      // An outline is four lines; for a filled rect it is still drawn so the
      // border keeps its own colour.
      const PipelineVertex a = V(r.min.x, r.min.y, r.color);
      const PipelineVertex b = V(r.max.x, r.min.y, r.color);
      const PipelineVertex c = V(r.max.x, r.max.y, r.color);
      const PipelineVertex d = V(r.min.x, r.max.y, r.color);
      out->lines.push_back(a); out->lines.push_back(b);
      out->lines.push_back(b); out->lines.push_back(c);
      out->lines.push_back(c); out->lines.push_back(d);
      out->lines.push_back(d); out->lines.push_back(a);
    }
  }

  for (const Circle2D& c : circles) {
    const i32 seg = CircleSegments(c.segments);
    const f32 step = Mathf::TWO_PI / (f32)seg;
    const PipelineVertex center = V(c.center.x, c.center.y, c.color);
    PipelineVertex prev = V(c.center.x + c.radius, c.center.y, c.color);
    for (i32 i = 1; i <= seg; ++i) {
      const f32 ang = step * (f32)i;
      const PipelineVertex cur = V(c.center.x + c.radius * std::cos(ang),
                                   c.center.y + c.radius * std::sin(ang),
                                   c.color);
      out->triangles.push_back(center);
      out->triangles.push_back(prev);
      out->triangles.push_back(cur);
      prev = cur;
    }
  }
}

DrawCounts BatchBuilder::Count(const std::vector<Line2D>& lines,
                               const std::vector<Rect2D>& rects,
                               const std::vector<Circle2D>& circles) {
  DrawCounts c;
  c.lineVertices = lines.size() * 2;
  for (const Rect2D& r : rects) CountRect(r, &c);
  for (const Circle2D& ci : circles) {
    c.triangleVertices += (size_t)CircleSegments(ci.segments) * 3;
  }
  return c;
}

//----------------------------------------------------------------------------
// Pipeline statistics
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
// Occlusion buffer
//----------------------------------------------------------------------------

namespace {

/// The view-space depth of a world point: the distance in front of the camera
/// along its forward axis, positive in front. Row 2 of the view matrix is the
/// negated forward axis, so `view * p` row 2 is `-depth`; negate to get a value
/// that grows with distance.
f32 ViewDepth(const Mat4& view, const Vec3& p) {
  const f32* m = view.m;
  return -(m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]);
}

/// The cell indices a span *touches* (inclusive), for a query: a box is only
/// culled when every cell it overlaps is filled, so this over-includes at a
/// boundary rather than under-include.
void CellSpanTouched(f32 lo, f32 hi, f32 cellSize, i32 cells, i32* out0, i32* out1) {
  i32 a = (i32)std::floor(lo / cellSize);
  i32 b = (i32)std::floor(hi / cellSize);
  if (a < 0) a = 0;
  if (b < 0) b = 0;
  if (a > cells - 1) a = cells - 1;
  if (b > cells - 1) b = cells - 1;
  *out0 = a;
  *out1 = b;
}

/// The cell indices fully inside a span (inclusive). An empty range (out1 <
/// out0) means the span covers no whole cell, so the occluder contributes
/// nothing — the safe direction.
void CellSpanInterior(f32 lo, f32 hi, f32 cellSize, i32 cells, i32* out0, i32* out1) {
  i32 a = (i32)std::ceil(lo / cellSize);
  i32 b = (i32)std::floor(hi / cellSize) - 1;
  if (a < 0) a = 0;
  if (b > cells - 1) b = cells - 1;
  *out0 = a;
  *out1 = b;
}

}  // namespace

void OcclusionBuffer::BeginFrame(const CameraState& camera) {
  cam_ = camera;
  valid_ = camera.valid;
  if (cellsX_ < 1) cellsX_ = kDefaultCellsX;
  if (cellsY_ < 1) cellsY_ = kDefaultCellsY;
  depth_.assign((size_t)cellsX_ * (size_t)cellsY_, -1.0f);
}

void OcclusionBuffer::Reset() {
  cam_ = CameraState{};
  valid_ = false;
  depth_.clear();
}

bool OcclusionBuffer::ProjectBox(const Aabb& box, Vec2* mn, Vec2* mx,
                                 f32* nearDepth, f32* farDepth) const {
  if (!valid_ || !mn || !mx || !nearDepth || !farDepth) return false;

  const Vec3 corners[8] = {
      Vec3(box.min.x, box.min.y, box.min.z), Vec3(box.max.x, box.min.y, box.min.z),
      Vec3(box.min.x, box.max.y, box.min.z), Vec3(box.max.x, box.max.y, box.min.z),
      Vec3(box.min.x, box.min.y, box.max.z), Vec3(box.max.x, box.min.y, box.max.z),
      Vec3(box.min.x, box.max.y, box.max.z), Vec3(box.max.x, box.max.y, box.max.z),
  };

  f32 dNear = 0.0f, dFar = 0.0f;
  Vec2 lo(1e30f, 1e30f), hi(-1e30f, -1e30f);
  bool first = true;
  for (const Vec3& c : corners) {
    const f32 d = ViewDepth(cam_.view, c);
    if (d <= 0.0001f) return false;   // a corner behind/at the eye: not safe
    if (first) { dNear = dFar = d; first = false; }
    else { dNear = std::min(dNear, d); dFar = std::max(dFar, d); }

    Vec2 s;
    if (!cam_.WorldToScreen(c, &s)) return false;   // outside the view: not safe
    lo.x = std::min(lo.x, s.x);
    lo.y = std::min(lo.y, s.y);
    hi.x = std::max(hi.x, s.x);
    hi.y = std::max(hi.y, s.y);
  }
  *mn = lo;
  *mx = hi;
  *nearDepth = dNear;
  *farDepth = dFar;
  return true;
}

void OcclusionBuffer::AddOccluder(const Aabb& box) {
  Vec2 mn, mx;
  f32 nearD = 0.0f, farD = 0.0f;
  if (!ProjectBox(box, &mn, &mx, &nearD, &farD)) return;
  if (cam_.width <= 0.0f || cam_.height <= 0.0f) return;

  // Only cells the box covers *completely* become opaque, so an occluder that
  // merely clips a cell edge cannot hide something in front of it.
  const f32 cellW = cam_.width / (f32)cellsX_;
  const f32 cellH = cam_.height / (f32)cellsY_;
  i32 x0 = 0, x1 = -1, y0 = 0, y1 = -1;
  CellSpanInterior(mn.x, mx.x, cellW, cellsX_, &x0, &x1);
  CellSpanInterior(mn.y, mx.y, cellH, cellsY_, &y0, &y1);
  if (x1 < x0 || y1 < y0) return;   // covers no whole cell

  for (i32 y = y0; y <= y1; ++y) {
    for (i32 x = x0; x <= x1; ++x) {
      f32& cell = depth_[(size_t)y * (size_t)cellsX_ + (size_t)x];
      // Store the *nearest* depth: an occluder only ever makes a cell shallower,
      // never deeper, so a later farther occluder cannot un-occlude a cell.
      if (cell < 0.0f || nearD < cell) cell = nearD;
    }
  }
}

bool OcclusionBuffer::IsOccluded(const Aabb& box) const {
  if (!valid_ || cam_.width <= 0.0f || cam_.height <= 0.0f) return false;

  Vec2 mn, mx;
  f32 nearD = 0.0f, farD = 0.0f;
  if (!ProjectBox(box, &mn, &mx, &nearD, &farD)) return false;

  // Every cell the box *touches* must be filled and nearer, so a box straddling
  // a cell edge is never culled on the strength of one filled neighbour.
  const f32 cellW = cam_.width / (f32)cellsX_;
  const f32 cellH = cam_.height / (f32)cellsY_;
  i32 x0 = 0, x1 = 0, y0 = 0, y1 = 0;
  CellSpanTouched(mn.x, mx.x, cellW, cellsX_, &x0, &x1);
  CellSpanTouched(mn.y, mx.y, cellH, cellsY_, &y0, &y1);

  for (i32 y = y0; y <= y1; ++y) {
    for (i32 x = x0; x <= x1; ++x) {
      const f32 cell = depth_[(size_t)y * (size_t)cellsX_ + (size_t)x];
      // Empty cell, or a cell whose occluder is behind this box's nearest
      // point: the box may be visible through it. Draw it.
      if (cell < 0.0f) return false;
      if (nearD <= cell) return false;
    }
  }
  return true;   // every covered cell is filled and nearer than the box
}

i32 OcclusionBuffer::filledCells() const {
  i32 n = 0;
  for (f32 d : depth_) if (d >= 0.0f) ++n;
  return n;
}

void PipelineStats::Reset() { *this = PipelineStats{}; }

//----------------------------------------------------------------------------
// RenderPipeline
//----------------------------------------------------------------------------

void RenderPipeline::BeginFrame(const CameraState& camera,
                                const RenderSettings& settings) {
  settings_ = settings;
  lod_ = LodPolicy::FromSettings(settings);
  if (camera.valid) {
    frustum_.SetFromCamera(camera.view, camera.projection);
  } else {
    // No camera: an empty frustum culls nothing, which is the safe direction
    // (a debug overlay that cannot see the camera draws everything).
    frustum_ = Frustum();
  }
  // The occlusion grid is rebuilt every frame; an invalid camera leaves it
  // invalid so nothing is culled on a stale depth.
  occlusion_.BeginFrame(camera);
  inFrame_ = true;
}

bool RenderPipeline::SubmitEntity(const Aabb& box) {
  ++stats_.entitiesSubmitted;
  if (!frustum_.Intersects(box)) {
    ++stats_.entitiesCulled;
    return false;
  }
  ++stats_.entitiesDrawn;
  return true;
}

bool RenderPipeline::SubmitEntity(const EntitySnapshot& snap) {
  return SubmitEntity(EntityHitbox(snap));
}

LodTier RenderPipeline::LodFor(f32 distance) {
  const LodTier t = lod_.TierFor(distance);
  if (t != LodTier::Full) ++stats_.entitiesLodReduced;
  return t;
}

bool RenderPipeline::ShouldTickEntity(f32 distance, u64 tick, u64 phase) {
  const LodTier t = lod_.TierFor(distance);
  const bool tickNow = lod_.ShouldTick(t, tick, phase);
  if (!tickNow) ++stats_.entitiesTickThrottled;
  return tickNow;
}

bool RenderPipeline::CullEntity(const Aabb& box) {
  ++stats_.entitiesSubmitted;
  // Frustum first: it is a handful of dot products and drops most off-screen
  // geometry before the grid lookup.
  if (!frustum_.Intersects(box)) {
    ++stats_.entitiesCulled;
    return false;
  }
  // Then occlusion. A box the buffer cannot place (behind the eye, off-screen,
  // or over an empty cell) is never occluded, so this only ever removes work.
  if (occlusion_.IsOccluded(box)) {
    ++stats_.entitiesOccluded;
    return false;
  }
  ++stats_.entitiesDrawn;
  return true;
}

f32 RenderPipeline::OnFrameTime(f32 frameMs) {
  return resolution_.OnFrame(frameMs);
}

u64 RenderPipeline::SubmitBatch(const DrawBatch& batch) {
  if (batch.empty()) return 0;
  DrawCounts c;
  c.triangleVertices = batch.triangles.size();
  c.lineVertices = batch.lines.size();
  return SubmitCounts(c);
}

u64 RenderPipeline::SubmitCounts(const DrawCounts& counts) {
  if (counts.empty()) return 0;
  ++stats_.batches;
  const u64 calls = (u64)counts.drawCalls();
  stats_.drawCalls += calls;
  stats_.vertices += (u64)counts.vertexCount();
  return calls;
}

void RenderPipeline::EndFrame() {
  stats_.frames++;
  stats_.resolutionScale = resolution_.scale();
  inFrame_ = false;
}

void RenderPipeline::Reset() {
  frustum_ = Frustum();
  lod_ = LodPolicy::Default();
  occlusion_.Reset();
  resolution_.Reset();
  settings_ = RenderSettings{};
  stats_.Reset();
  inFrame_ = false;
}

}  // namespace Berry
