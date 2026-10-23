// Host tests for the render pipeline: frustum culling, LOD tiers, dynamic
// resolution, the vertex batch, and the backend seam.
//
// Everything here is GPU-free and clock-free, so the whole frame path is
// exercised on the host:
//   * a frustum built from a real perspective * view culls what is behind the
//     camera and keeps what is in front, and refuses a degenerate matrix,
//   * the LOD tiers follow the governor's render settings,
//   * the resolution controller reacts to a sustained streak, not a single
//     spike, and stays inside its floor/ceiling,
//   * the batch builder produces a deterministic vertex stream (two triangles
//     per filled rect, four lines per outline, a fan per circle),
//   * the backend seam drives that batch through a backend's PresentBatch.
#include "berry/pipeline.h"
#include "berry/render.h"
#include "berry/render_backend.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static bool Near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

namespace {

/// A 60-degree camera at the origin looking down +Z, 1280x720. The world point
/// (0,0,10) is dead centre; (0,0,-10) is behind the eye.
CameraState TestCamera() {
  CameraState cam;
  cam.width = 1280;
  cam.height = 720;
  cam.view = Mat4::LookAt(Vec3(0, 0, 0), Vec3(0, 0, 1), Vec3(0, 1, 0));
  cam.projection = Mat4::Perspective(60.0f, 1280.0f / 720.0f, 0.1f, 256.0f);
  cam.valid = true;
  return cam;
}

Aabb BoxAt(f32 x, f32 y, f32 z, f32 half = 0.5f) {
  return Aabb(Vec3(x - half, y - half, z - half),
              Vec3(x + half, y + half, z + half));
}

/// Records the batch the backend seam hands it, so the seam is tested without
/// any GL.
class RecordingBackend final : public IRenderBackend {
 public:
  bool Init() override { return true; }
  bool Start() override { return true; }
  void Stop() override {}
  void PresentBatch(const DrawBatch& b, const Vec2& screen) override {
    batch = b;
    lastScreen = screen;
    ++calls;
  }
  bool running() const override { return true; }
  const char* name() const override { return "recording"; }

  void SetRenderScale(f32 scale) override {
    scaleCalls.push_back(scale);
  }

  DrawBatch batch;
  Vec2 lastScreen;
  int calls = 0;
  std::vector<f32> scaleCalls;
};

}  // namespace

//----------------------------------------------------------------------------
// Frustum
//----------------------------------------------------------------------------

static void TestFrustum() {
  const CameraState cam = TestCamera();
  Frustum f;
  CHECK(f.SetFromCamera(cam.view, cam.projection));
  CHECK(f.valid());

  // In front of the camera and inside the 60-degree cone.
  CHECK(f.ContainsPoint(Vec3(0, 0, 10)));
  CHECK(f.Contains(BoxAt(0, 0, 10)));
  CHECK(f.Intersects(BoxAt(0, 0, 10)));
  CHECK(f.IntersectsSphere(Vec3(0, 0, 10), 1.0f));

  // Behind the camera.
  CHECK(!f.ContainsPoint(Vec3(0, 0, -10)));
  CHECK(!f.Intersects(BoxAt(0, 0, -10)));
  CHECK(!f.IntersectsSphere(Vec3(0, 0, -10), 1.0f));

  // Far off to the side, outside the cone.
  CHECK(!f.ContainsPoint(Vec3(200, 0, 10)));
  CHECK(!f.Intersects(BoxAt(200, 0, 10)));

  // Beyond the far plane.
  CHECK(!f.ContainsPoint(Vec3(0, 0, 1000)));

  // A box straddling the near plane still intersects even though it is not
  // fully inside: the cull test must not drop a visible box.
  const Aabb straddle(Vec3(-0.5f, -0.5f, -0.5f), Vec3(0.5f, 0.5f, 0.5f));
  CHECK(!f.Contains(straddle));
  CHECK(f.Intersects(straddle));

  // A big box containing the camera intersects.
  CHECK(f.Intersects(Aabb(Vec3(-50, -50, -50), Vec3(50, 50, 50))));

  // The plane distances are signed with "inside" positive.
  CHECK(f.Distance(Frustum::Plane::Near, Vec3(0, 0, 10)) > 0);
  CHECK(f.Distance(Frustum::Plane::Near, Vec3(0, 0, -10)) < 0);

  // A degenerate matrix is refused and culls nothing.
  Frustum bad;
  CHECK(!bad.SetFromViewProjection(Mat4()));
  CHECK(!bad.valid());
  CHECK(bad.Intersects(BoxAt(200, 0, 10)));
  CHECK(bad.ContainsPoint(Vec3(0, 0, -10)));
}

static void TestFrustumNoCameraCullsNothing() {
  RenderPipeline p;
  RenderSettings s;
  p.BeginFrame(CameraState(), s);   // invalid camera
  CHECK(!p.frustum().valid());
  CHECK(p.SubmitEntity(BoxAt(0, 0, -100)));
  CHECK(p.SubmitEntity(BoxAt(5000, 0, 0)));
  CHECK(p.stats().entitiesCulled == 0);
  CHECK(p.stats().entitiesDrawn == 2);
}

//----------------------------------------------------------------------------
// LOD
//----------------------------------------------------------------------------

static void TestLodPolicy() {
  RenderSettings s;
  s.animationDistance = 32.0f;
  s.entityDistance = 64.0f;
  s.cullFarEntities = true;
  const LodPolicy p = LodPolicy::FromSettings(s);

  CHECK(Near(p.fullDistance, 16.0f));
  CHECK(Near(p.reducedDistance, 32.0f));
  CHECK(Near(p.staticDistance, 64.0f));
  CHECK(p.cullBeyondStatic);

  CHECK(p.TierFor(0.0f) == LodTier::Full);
  CHECK(p.TierFor(16.0f) == LodTier::Full);
  CHECK(p.TierFor(16.1f) == LodTier::Reduced);
  CHECK(p.TierFor(32.0f) == LodTier::Reduced);
  CHECK(p.TierFor(32.1f) == LodTier::Static);
  CHECK(p.TierFor(64.0f) == LodTier::Static);
  CHECK(p.TierFor(64.1f) == LodTier::Culled);

  CHECK(p.Animates(LodTier::Full));
  CHECK(p.Animates(LodTier::Reduced));
  CHECK(!p.Animates(LodTier::Static));
  CHECK(p.TicksPhysics(LodTier::Full));
  CHECK(!p.TicksPhysics(LodTier::Reduced));

  // Without culling, the far tier is Static, never Culled.
  RenderSettings noCull = s;
  noCull.cullFarEntities = false;
  const LodPolicy p2 = LodPolicy::FromSettings(noCull);
  CHECK(!p2.cullBeyondStatic);
  CHECK(p2.TierFor(1000.0f) == LodTier::Static);

  CHECK(std::string(LodTierName(LodTier::Reduced)) == "reduced");
}

/// The software occlusion buffer: a conservative depth grid. It must cull a box
/// genuinely behind a wall and never cull one in front, off to the side of the
/// wall's coverage, or with no camera/occluder at all.
static void TestOcclusionBuffer() {
  OcclusionBuffer buf;

  // No camera: nothing is ever occluded.
  buf.BeginFrame(CameraState{});
  CHECK(!buf.valid());
  CHECK(!buf.IsOccluded(BoxAt(0, 0, 20)));

  buf.BeginFrame(TestCamera());   // 1280x720 at the origin looking +Z
  CHECK(buf.valid());
  // No occluder rasterised: nothing is culled.
  CHECK(buf.filledCells() == 0);
  CHECK(!buf.IsOccluded(BoxAt(0, 0, 20)));

  // A wall at z=10 covering the middle of the view. Cells it fully covers
  // become opaque at depth ~10; the screen edges stay empty.
  buf.AddOccluder(Aabb(Vec3(-3, -3, 9), Vec3(3, 3, 11)));
  CHECK(buf.filledCells() > 0);

  // A box behind the wall, at screen centre: occluded.
  CHECK(buf.IsOccluded(BoxAt(0, 0, 20)));
  // A box in front of the wall: never occluded (it is nearer than the depth).
  CHECK(!buf.IsOccluded(BoxAt(0, 0, 5)));
  // A box behind the wall but off to the side, outside the wall's projected
  // coverage: an empty cell in its span keeps it visible.
  CHECK(!buf.IsOccluded(BoxAt(9, 0, 20)));
  // A box behind the eye is never culled (it cannot be placed).
  CHECK(!buf.IsOccluded(BoxAt(0, 0, -20)));

  // A nearer occluder only ever makes a cell shallower: re-rasterising the same
  // wall from a farther box cannot un-occlude it.
  buf.AddOccluder(Aabb(Vec3(-3, -3, 30), Vec3(3, 3, 32)));
  CHECK(buf.IsOccluded(BoxAt(0, 0, 20)));
}

/// The pipeline's `CullEntity` runs frustum first, then occlusion, and accounts
/// each drop separately.
static void TestPipelineOcclusionCull() {
  RenderPipeline p;
  RenderSettings s;
  s.cullFarEntities = false;   // keep the test about occlusion, not distance
  p.BeginFrame(TestCamera(), s);

  // Rasterise a wall, then a box behind it at screen centre.
  p.occlusion().AddOccluder(Aabb(Vec3(-3, -3, 9), Vec3(3, 3, 11)));
  CHECK(!p.CullEntity(BoxAt(0, 0, 20)));   // occluded
  CHECK(p.stats().entitiesOccluded == 1);
  CHECK(p.stats().entitiesCulled == 0);
  CHECK(p.stats().entitiesDrawn == 0);

  // A box in front of the wall is drawn.
  CHECK(p.CullEntity(BoxAt(0, 0, 5)));
  CHECK(p.stats().entitiesDrawn == 1);

  // A box behind the camera is frustum-culled, not occlusion-culled.
  CHECK(!p.CullEntity(BoxAt(0, 0, -20)));
  CHECK(p.stats().entitiesCulled == 1);
  CHECK(p.stats().entitiesOccluded == 1);

  // A fresh frame clears the grid: the same box is now drawn.
  p.BeginFrame(TestCamera(), s);
  CHECK(p.occlusion().valid());           // valid camera -> valid buffer
  CHECK(p.occlusion().filledCells() == 0);
  CHECK(p.CullEntity(BoxAt(0, 0, 20)));
}

/// LOD's tick-rate half: a distant entity is *throttled*, not just reported as a
/// lower tier. Full ticks every tick; Reduced one tick in the interval with the
/// phase spreading a crowd; Static and Culled never tick.
static void TestLodShouldTick() {
  LodPolicy p;
  p.reducedTickInterval = 4;

  // Full ticks every tick regardless of phase.
  for (u64 t = 0; t < 10; ++t) {
    CHECK(p.ShouldTick(LodTier::Full, t, 0));
    CHECK(p.ShouldTick(LodTier::Full, t, 7));
  }

  // Reduced ticks once every 4 ticks, offset by the phase.
  int ticks0 = 0, ticks3 = 0;
  for (u64 t = 0; t < 40; ++t) {
    if (p.ShouldTick(LodTier::Reduced, t, 0)) ++ticks0;
    if (p.ShouldTick(LodTier::Reduced, t, 3)) ++ticks3;
  }
  CHECK(ticks0 == 10);
  CHECK(ticks3 == 10);
  // Different phases must not land on the same tick: the whole point.
  CHECK(p.ShouldTick(LodTier::Reduced, 0, 0));
  CHECK(!p.ShouldTick(LodTier::Reduced, 0, 1));

  // An interval below 1 is clamped to 1 (every tick), never a divide-by-zero.
  LodPolicy q;
  q.reducedTickInterval = 0;
  CHECK(q.ShouldTick(LodTier::Reduced, 5, 0));
  CHECK(q.ShouldTick(LodTier::Reduced, 6, 0));

  // Static and Culled never tick.
  CHECK(!p.ShouldTick(LodTier::Static, 0, 0));
  CHECK(!p.ShouldTick(LodTier::Culled, 0, 0));
}

/// The pipeline's `ShouldTickEntity` records the skipped ticks so a caller can
/// read what the throttle saved.
static void TestPipelineTickThrottleAccounting() {
  RenderPipeline p;
  RenderSettings s;
  s.animationDistance = 32.0f;
  s.entityDistance = 64.0f;
  p.BeginFrame(TestCamera(), s);
  p.SetLodPolicy(LodPolicy::Default());   // reducedTickInterval == 4

  CHECK(p.stats().entitiesTickThrottled == 0);
  // Full distance: always ticks, never counted.
  CHECK(p.ShouldTickEntity(5.0f, 0, 0));
  CHECK(p.stats().entitiesTickThrottled == 0);
  // Reduced band is (16, 32]: ticks on tick 0 (0+0 % 4 == 0), skipped on tick 1.
  CHECK(p.ShouldTickEntity(24.0f, 0, 0));
  CHECK(!p.ShouldTickEntity(24.0f, 1, 0));
  CHECK(p.stats().entitiesTickThrottled == 1);
  // Static: never ticks, counted.
  CHECK(!p.ShouldTickEntity(100.0f, 0, 0));
  CHECK(p.stats().entitiesTickThrottled == 2);
}

static void TestPipelineLodAccounting() {
  RenderPipeline p;
  RenderSettings s;
  s.animationDistance = 32.0f;
  s.entityDistance = 64.0f;
  p.BeginFrame(TestCamera(), s);

  CHECK(p.LodFor(5.0f) == LodTier::Full);
  CHECK(p.stats().entitiesLodReduced == 0);
  CHECK(p.LodFor(40.0f) == LodTier::Static);
  CHECK(p.stats().entitiesLodReduced == 1);
  CHECK(p.LodFor(80.0f) == LodTier::Static);   // no cull by default
  CHECK(p.stats().entitiesLodReduced == 2);
}

/// `SubmitCounts` must account a frame exactly as `SubmitBatch` does, because
/// the Renderer measures its frame without materialising the vertices.
static void TestSubmitCountsMatchesSubmitBatch() {
  std::vector<Line2D> lines;
  Line2D l;
  l.a = Vec2(0, 0);
  l.b = Vec2(4, 4);
  lines.push_back(l);
  std::vector<Rect2D> rects;
  Rect2D r;
  r.min = Vec2(0, 0);
  r.max = Vec2(8, 8);
  r.filled = true;
  r.fill = Rgba::White();
  r.color = Rgba::Black();   // outline too: a fill and four lines
  rects.push_back(r);
  std::vector<Circle2D> circles;
  Circle2D c;
  c.center = Vec2(2, 2);
  c.radius = 3;
  c.segments = 10;
  circles.push_back(c);

  DrawBatch batch;
  BatchBuilder::Build(lines, rects, circles, &batch);

  RenderPipeline fromBatch;
  fromBatch.BeginFrame(TestCamera(), RenderSettings{});
  const u64 callsBatch = fromBatch.SubmitBatch(batch);

  RenderPipeline fromCounts;
  fromCounts.BeginFrame(TestCamera(), RenderSettings{});
  const u64 callsCounts =
      fromCounts.SubmitCounts(BatchBuilder::Count(lines, rects, circles));

  CHECK(callsBatch == callsCounts);
  CHECK(fromBatch.stats().drawCalls == fromCounts.stats().drawCalls);
  CHECK(fromBatch.stats().vertices == fromCounts.stats().vertices);
  CHECK(fromBatch.stats().batches == fromCounts.stats().batches);
  CHECK(fromBatch.stats().vertices == (u64)batch.vertexCount());

  // An empty batch and an empty count are both a no-op.
  CHECK(fromBatch.SubmitBatch(DrawBatch{}) == 0);
  CHECK(fromCounts.SubmitCounts(DrawCounts{}) == 0);
}

//----------------------------------------------------------------------------
// Dynamic resolution
//----------------------------------------------------------------------------

static void TestDynamicResolution() {
  DynamicResolution dr;
  DynamicResolution::Config cfg;
  cfg.targetMs = 16.67f;
  cfg.framesToReact = 4;
  cfg.stepDown = 0.1f;
  cfg.stepUp = 0.05f;
  cfg.minScale = 0.5f;
  cfg.maxScale = 1.0f;
  dr.Configure(cfg);
  CHECK(Near(dr.scale(), 1.0f));

  // A single slow frame is not enough: the streak has to reach 4.
  CHECK(Near(dr.OnFrame(30.0f), 1.0f));
  CHECK(Near(dr.OnFrame(30.0f), 1.0f));
  CHECK(Near(dr.OnFrame(30.0f), 1.0f));
  CHECK(!dr.dirty());
  CHECK(Near(dr.OnFrame(30.0f), 0.9f));   // 4th frame reacts
  CHECK(dr.dirty());

  // A good frame resets the slow streak, so the next slow run starts over.
  dr.OnFrame(16.0f);
  CHECK(Near(dr.OnFrame(30.0f), 0.9f));   // only 1 slow frame so far
  CHECK(Near(dr.scale(), 0.9f));

  // Sustained fast frames restore, slower than they were cut.
  for (int i = 0; i < 4; ++i) dr.OnFrame(10.0f);
  CHECK(Near(dr.scale(), 0.95f));

  // The floor holds.
  dr.SetScale(0.5f);
  for (int i = 0; i < 40; ++i) dr.OnFrame(100.0f);
  CHECK(Near(dr.scale(), 0.5f));

  // The ceiling holds.
  dr.SetScale(1.0f);
  for (int i = 0; i < 40; ++i) dr.OnFrame(5.0f);
  CHECK(Near(dr.scale(), 1.0f));

  // A non-finite frame time is ignored rather than crashing the streak.
  const f32 before = dr.scale();
  CHECK(Near(dr.OnFrame(std::nanf("")), before));
  CHECK(Near(dr.OnFrame(-1.0f), before));

  // Disabled: no reaction at all.
  cfg.enabled = false;
  dr.Configure(cfg);
  dr.Reset();
  for (int i = 0; i < 20; ++i) dr.OnFrame(100.0f);
  CHECK(Near(dr.scale(), 1.0f));

  // Configure swaps an inverted range into order instead of misbehaving.
  DynamicResolution::Config inverted;
  inverted.minScale = 0.9f;
  inverted.maxScale = 0.4f;
  DynamicResolution d2;
  d2.Configure(inverted);
  CHECK(Near(d2.config().minScale, 0.4f));
  CHECK(Near(d2.config().maxScale, 0.9f));
}

static void TestPipelineResolution() {
  RenderPipeline p;
  DynamicResolution::Config cfg;
  cfg.framesToReact = 2;
  p.SetResolutionConfig(cfg);
  p.BeginFrame(TestCamera(), RenderSettings{});

  p.OnFrameTime(30.0f);
  p.OnFrameTime(30.0f);
  CHECK(p.resolution().scale() < 1.0f);
  p.EndFrame();
  CHECK(Near(p.stats().resolutionScale, p.resolution().scale()));
  CHECK(p.stats().frames == 1);
}

//----------------------------------------------------------------------------
// Batch building
//----------------------------------------------------------------------------

static void TestBatchBuilder() {
  std::vector<Line2D> lines;
  Line2D l;
  l.a = Vec2(0, 0);
  l.b = Vec2(10, 10);
  l.color = Rgba(255, 0, 0, 255);
  lines.push_back(l);

  std::vector<Rect2D> rects;
  Rect2D filled;
  filled.min = Vec2(0, 0);
  filled.max = Vec2(4, 4);
  filled.fill = Rgba(0, 255, 0, 255);
  filled.color = Rgba(0, 0, 0, 0);   // invisible outline
  filled.filled = true;
  rects.push_back(filled);

  std::vector<Circle2D> circles;
  Circle2D c;
  c.center = Vec2(5, 5);
  c.radius = 2;
  c.color = Rgba(0, 0, 255, 255);
  c.segments = 8;
  circles.push_back(c);

  DrawBatch batch;
  BatchBuilder::Build(lines, rects, circles, &batch);

  // One line -> two line vertices.
  CHECK(batch.lineCount() == 1 + 4);   // the segment plus the rect outline
  // The filled rect is two triangles; the 8-segment circle is an 8-triangle fan.
  CHECK(batch.triangleCount() == 2 + 8);
  CHECK(batch.vertexCount() == (2 + 8) * 3 + (1 + 4) * 2);
  CHECK(!batch.empty());

  // Colours are normalised to 0..1.
  CHECK(Near(batch.triangles[0].r, 0.0f));
  CHECK(Near(batch.triangles[0].g, 1.0f));

  // Determinism: the same input yields the identical stream.
  DrawBatch again;
  BatchBuilder::Build(lines, rects, circles, &again);
  CHECK(again.triangles.size() == batch.triangles.size());
  CHECK(again.lines.size() == batch.lines.size());
  bool same = true;
  for (size_t i = 0; i < batch.triangles.size(); ++i) {
    if (!Near(again.triangles[i].x, batch.triangles[i].x) ||
        !Near(again.triangles[i].y, batch.triangles[i].y)) same = false;
  }
  CHECK(same);

  // A circle segment count is clamped into the sane range.
  Circle2D tiny;
  tiny.center = Vec2(0, 0);
  tiny.radius = 1;
  tiny.segments = 1;
  std::vector<Circle2D> one{tiny};
  DrawBatch clamped;
  BatchBuilder::Build({}, {}, one, &clamped);
  CHECK(clamped.triangleCount() == (size_t)BatchBuilder::kMinCircleSegments);

  Circle2D huge = tiny;
  huge.segments = 100000;
  std::vector<Circle2D> two{huge};
  DrawBatch capped;
  BatchBuilder::Build({}, {}, two, &capped);
  CHECK(capped.triangleCount() == (size_t)BatchBuilder::kMaxCircleSegments);

  // Empty input yields an empty batch.
  DrawBatch empty;
  BatchBuilder::Build({}, {}, {}, &empty);
  CHECK(empty.empty());
}

static void TestPipelineBatchAccounting() {
  RenderPipeline p;
  p.BeginFrame(TestCamera(), RenderSettings{});

  DrawBatch batch;
  std::vector<Rect2D> rects;
  Rect2D r;
  r.min = Vec2(0, 0);
  r.max = Vec2(2, 2);
  r.filled = true;
  r.fill = Rgba::White();
  r.color = Rgba::Black();
  rects.push_back(r);
  std::vector<Line2D> lines;
  Line2D l;
  l.a = Vec2(0, 0);
  l.b = Vec2(1, 1);
  lines.push_back(l);
  BatchBuilder::Build(lines, rects, {}, &batch);

  // Triangles and lines each cost one draw call.
  CHECK(p.SubmitBatch(batch) == 2);
  CHECK(p.stats().drawCalls == 2);
  CHECK(p.stats().batches == 1);
  CHECK(p.stats().vertices == (u64)batch.vertexCount());

  // An empty batch costs nothing.
  DrawBatch none;
  CHECK(p.SubmitBatch(none) == 0);
  CHECK(p.stats().batches == 1);

  // EndFrame records the frame and the current scale.
  p.EndFrame();
  CHECK(p.stats().frames == 1);
}

//----------------------------------------------------------------------------
// Backend seam
//----------------------------------------------------------------------------

static void TestBackendSeam() {
  RecordingBackend backend;
  RenderSeam* seam = CreateBackendSeam(&backend);
  CHECK(seam != nullptr);

  Renderer r;
  r.Attach(seam);
  r.BeginFrame(Vec2(800, 600));

  // A filled rect and a line, then present: the backend sees the coalesced
  // batch, not the primitive list.
  Rect2D box;
  box.min = Vec2(10, 10);
  box.max = Vec2(50, 50);
  box.filled = true;
  box.fill = Rgba::Red();
  box.color = Rgba::White();
  r.DrawRect(box);
  r.DrawLine(Vec2(0, 0), Vec2(100, 100), Rgba::Blue());
  r.EndFrame();

  CHECK(backend.calls == 1);
  CHECK(Near(backend.lastScreen.x, 800.0f));
  CHECK(backend.batch.triangleCount() == 2);   // the fill
  CHECK(backend.batch.lineCount() == 5);       // the outline plus the line
  CHECK(!backend.batch.empty());

  // A frame with nothing to draw presents an empty batch, and the backend is
  // still called (the seam does not second-guess the frame).
  r.BeginFrame(Vec2(800, 600));
  r.EndFrame();
  CHECK(backend.calls == 2);
  CHECK(backend.batch.empty());

  // A null backend yields no seam.
  CHECK(CreateBackendSeam(nullptr) == nullptr);
  delete seam;
}

/// Dynamic resolution is only real if its output reaches the presenter. This
/// pins that link: a sustained slow frame time must push a reduced scale to the
/// seam exactly once per change, and a restored frame time must push it back.
static void TestRenderScaleReachesSeam() {
  RecordingBackend backend;
  RenderSeam* seam = CreateBackendSeam(&backend);
  Renderer r;
  r.Attach(seam);

  // Native scale is never pushed (it is the default the seam already assumes).
  CHECK(backend.scaleCalls.empty());

  // Six consecutive slow frames (default framesToReact) drop the scale by one
  // step, 1.0 -> 0.9, and the seam is told once.
  f32 last = 1.0f;
  for (int i = 0; i < 6; ++i) last = r.OnFrameTime(40.0f);
  CHECK(Near(last, 0.9f));
  CHECK(backend.scaleCalls.size() == 1);
  CHECK(Near(backend.scaleCalls.back(), 0.9f));

  // More slow frames keep stepping down; each change is pushed once.
  for (int i = 0; i < 6; ++i) r.OnFrameTime(40.0f);
  CHECK(backend.scaleCalls.size() == 2);
  CHECK(Near(backend.scaleCalls.back(), 0.8f));

  // A steady fast run restores the scale, pushing the new value.
  for (int i = 0; i < 6; ++i) r.OnFrameTime(4.0f);
  CHECK(backend.scaleCalls.size() == 3);
  CHECK(backend.scaleCalls.back() > 0.8f);

  // No change, no push: repeating the same fast frame pushes nothing new.
  const size_t pushes = backend.scaleCalls.size();
  r.OnFrameTime(4.0f);
  CHECK(backend.scaleCalls.size() == pushes);

  delete seam;
}

static void TestBackendIsNullOnHost() {
  // The host build's backend is the null stub: it reports cleanly rather than
  // pretending to have GL.
  IRenderBackend* b = CreateRenderBackend();
  CHECK(b != nullptr);
  CHECK(!b->running());
  CHECK(!b->Start());
  std::string name = b->name();
  CHECK(name == "null" || name == "GLES-overlay");
  DrawBatch batch;
  b->PresentBatch(batch, Vec2(1, 1));   // must not crash on either backend
  b->Stop();
  delete b;
}

//----------------------------------------------------------------------------
// Reset
//----------------------------------------------------------------------------

static void TestPipelineReset() {
  RenderPipeline p;
  RenderSettings s;
  s.cullFarEntities = true;
  s.entityDistance = 10.0f;
  p.BeginFrame(TestCamera(), s);
  p.SubmitEntity(BoxAt(0, 0, 10));
  p.OnFrameTime(30.0f);
  p.EndFrame();
  CHECK(p.stats().frames == 1);

  p.Reset();
  CHECK(p.stats().frames == 0);
  CHECK(p.stats().entitiesSubmitted == 0);
  CHECK(Near(p.resolution().scale(), 1.0f));
  CHECK(!p.frustum().valid());
  CHECK(Near(p.settings().entityDistance, 64.0f));   // back to the default
}

int main() {
  TestFrustum();
  TestFrustumNoCameraCullsNothing();
  TestLodPolicy();
  TestOcclusionBuffer();
  TestPipelineOcclusionCull();
  TestLodShouldTick();
  TestPipelineTickThrottleAccounting();
  TestPipelineLodAccounting();
  TestSubmitCountsMatchesSubmitBatch();
  TestDynamicResolution();
  TestPipelineResolution();
  TestBatchBuilder();
  TestPipelineBatchAccounting();
  TestBackendSeam();
  TestRenderScaleReachesSeam();
  TestBackendIsNullOnHost();
  TestPipelineReset();

  if (g_fail) {
    printf("test_pipeline: %d failure(s)\n", g_fail);
    return 1;
  }
  printf("test_pipeline: all OK\n");
  return 0;
}
