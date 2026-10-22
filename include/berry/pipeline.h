#pragma once
// BerryAPI render pipeline: the host-testable half of the frame path.
//
// Everything the frame does between "the world wants to draw this" and "the
// backend uploads vertices" lives here, with no GPU and no clock:
//
//   * Frustum      — the six clip planes of a view-projection, for an AABB
//                    cull that never drops a box the camera can see,
//   * LodPolicy    — a distance tier that decides whether an entity animates,
//                    updates physics, or is drawn statically,
//   * DynamicResolution — a scale controller that trades pixels for frame time
//                    inside a floor/ceiling, with hysteresis so it cannot
//                    oscillate,
//   * BatchBuilder — coalesces the Renderer's 2D primitive lists into one
//                    vertex stream (triangles + lines) the GLES backend uploads
//                    in two draw calls instead of one per primitive.
//
// The module stays strict: it knows no device API. The backend consumes the
// batch; the host test drives the same code with synthetic frames.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/draw.h"
#include "berry/perf.h"

#include <array>
#include <string>
#include <vector>

namespace Berry {

//----------------------------------------------------------------------------
// Frustum
//----------------------------------------------------------------------------

/// The six clip planes of a camera's view-projection, extracted with the
/// Gribb-Hartmann method. Planes point inward, so a point is visible when it is
/// on the positive side of all six.
class BERRY_API Frustum {
 public:
  enum class Plane : u8 {
    Left = 0, Right, Bottom, Top, Near, Far, Count,
  };

  /// Extracts the planes from a column-major view-projection matrix. Returns
  /// false (and leaves the frustum empty) when the matrix is degenerate, so a
  /// caller with no camera culls nothing rather than everything.
  bool SetFromViewProjection(const Mat4& viewProj);
  /// Convenience: view * projection.
  bool SetFromCamera(const Mat4& view, const Mat4& projection);

  /// The frustum has usable planes.
  bool valid() const { return valid_; }

  /// A point is inside when it is on the positive side of every plane.
  bool ContainsPoint(const Vec3& p) const;

  /// Fully inside: the box's negative vertex is on the positive side of every
  /// plane. This is the conservative test for "do not bother clipping it".
  bool Contains(const Aabb& box) const;

  /// Intersecting or inside. The cull test: only a box entirely outside every
  /// one of whose planes the positive vertex fails is rejected.
  bool Intersects(const Aabb& box) const;

  /// Sphere test (centre, radius).
  bool IntersectsSphere(const Vec3& center, f32 radius) const;

  /// Signed distance from the plane to a point (positive = inside).
  f32 Distance(Plane p, const Vec3& point) const;
  /// The plane's normal (xyz) and offset (w).
  Vec4 plane(Plane p) const { return planes_[(size_t)p]; }

 private:
  std::array<Vec4, (size_t)Plane::Count> planes_{};
  bool valid_ = false;
};

//----------------------------------------------------------------------------
// LOD
//----------------------------------------------------------------------------

/// What a distance tier allows for one entity.
enum class LodTier : u8 {
  /// Near: full animation and physics.
  Full = 0,
  /// Mid: animate, but throttle physics.
  Reduced,
  /// Far: draw the model statically, no animation or physics.
  Static,
  /// Beyond the draw distance: not drawn at all.
  Culled,
};

const char* LodTierName(LodTier t);

/// Distance tiers derived from the governor's `RenderSettings`, so LOD follows
/// the same quality level culling and particle caps do.
struct BERRY_API LodPolicy {
  f32 fullDistance = 16.0f;
  f32 reducedDistance = 32.0f;
  f32 staticDistance = 64.0f;
  /// When false, nothing is ever `Culled` (the tier still reports `Static`).
  bool cullBeyondStatic = true;

  static LodPolicy Default();
  /// Derives the tiers from render settings: full at half the animation
  /// distance, reduced at the animation distance, static at the entity
  /// distance, and culled past it when the settings cull far entities.
  static LodPolicy FromSettings(const RenderSettings& s);

  LodTier TierFor(f32 distance) const;
  bool Animates(LodTier t) const { return t == LodTier::Full || t == LodTier::Reduced; }
  bool TicksPhysics(LodTier t) const { return t == LodTier::Full; }
};

//----------------------------------------------------------------------------
// Dynamic resolution
//----------------------------------------------------------------------------

/// Chooses a render scale in [minScale, maxScale] from frame times.
///
/// Two rules keep it from oscillating: a change needs a sustained streak of
/// slow (or fast) frames, and the scale moves in steps rather than jumping.
/// The controller owns no clock: `OnFrame(frameMs)` is the whole input.
class BERRY_API DynamicResolution {
 public:
  struct Config {
    f32 targetMs = 16.67f;      // 60 fps
    f32 slowMargin = 0.10f;     // over target*1.10 counts as slow
    f32 fastMargin = 0.20f;     // under target*0.80 counts as fast
    i32 framesToReact = 6;      // consecutive frames before a change
    f32 stepDown = 0.10f;       // scale removed per reaction
    f32 stepUp = 0.05f;         // scale restored per reaction (slower)
    f32 minScale = 0.5f;
    f32 maxScale = 1.0f;
    bool enabled = true;
  };

  DynamicResolution() = default;

  void Configure(const Config& cfg);
  const Config& config() const { return cfg_; }

  /// Feeds one frame. Returns the scale after the update.
  f32 OnFrame(f32 frameMs);

  /// The current scale, clamped into the configured range.
  f32 scale() const { return scale_; }
  /// The scale the controller has settled on (== scale() once idle).
  f32 stableScale() const { return scale_; }
  /// True when the scale changed on the most recent `OnFrame`.
  bool dirty() const { return dirty_; }
  void ClearDirty() { dirty_ = false; }

  /// Forces a scale (a manual override, or a test). Clamped.
  void SetScale(f32 scale);

  void Reset();

 private:
  Config cfg_{};
  f32 scale_ = 1.0f;
  i32 slowStreak_ = 0;
  i32 fastStreak_ = 0;
  bool dirty_ = false;
};

//----------------------------------------------------------------------------
// Vertex batch
//----------------------------------------------------------------------------

/// One overlay vertex: pixel position plus RGBA colour. The layout is fixed so
/// the backend can describe it once and both draw calls share it.
struct BERRY_API PipelineVertex {
  f32 x = 0, y = 0;
  f32 r = 1, g = 1, b = 1, a = 1;
};

/// A frame's geometry, ready to upload. Triangles carry fills (rects, circles);
/// lines carry strokes. Text is rasterised by the backend and is not in here.
struct BERRY_API DrawBatch {
  std::vector<PipelineVertex> triangles;
  std::vector<PipelineVertex> lines;

  void Clear() {
    triangles.clear();
    lines.clear();
  }
  size_t triangleCount() const { return triangles.size() / 3; }
  size_t lineCount() const { return lines.size() / 2; }
  size_t vertexCount() const { return triangles.size() + lines.size(); }
  bool empty() const { return triangles.empty() && lines.empty(); }
};

/// The shape of a batch without its vertices: what a caller needs to account
/// for a frame (vertex count, draw calls) without allocating the stream.
struct BERRY_API DrawCounts {
  size_t triangleVertices = 0;
  size_t lineVertices = 0;

  size_t vertexCount() const { return triangleVertices + lineVertices; }
  size_t triangleCount() const { return triangleVertices / 3; }
  size_t lineCount() const { return lineVertices / 2; }
  /// Triangles and lines each cost one draw call.
  size_t drawCalls() const {
    return (triangleVertices ? 1u : 0u) + (lineVertices ? 1u : 0u);
  }
  bool empty() const { return triangleVertices == 0 && lineVertices == 0; }
};

/// Turns the Renderer's primitive lists into a `DrawBatch`.
///
/// The conversion is pure and deterministic: the same lists always produce the
/// same vertices in the same order, so a host test pins the stream exactly.
class BERRY_API BatchBuilder {
 public:
  /// Appends the batch for one frame's primitives. Circles are tessellated with
  /// the segment count each primitive carries (clamped to a sane range); rects
  /// become two triangles each.
  static void Build(const std::vector<Line2D>& lines,
                    const std::vector<Rect2D>& rects,
                    const std::vector<Circle2D>& circles,
                    DrawBatch* out);

  /// The same walk without emitting vertices, for a caller that only needs the
  /// frame's shape (pipeline accounting). Shares the one implementation with
  /// `Build`, so the two can never disagree.
  static DrawCounts Count(const std::vector<Line2D>& lines,
                          const std::vector<Rect2D>& rects,
                          const std::vector<Circle2D>& circles);

  /// The minimum/maximum segment count a circle is tessellated with.
  static constexpr i32 kMinCircleSegments = 3;
  static constexpr i32 kMaxCircleSegments = 128;
};

//----------------------------------------------------------------------------
// Pipeline statistics
//----------------------------------------------------------------------------

struct BERRY_API PipelineStats {
  u64 frames = 0;
  u64 entitiesSubmitted = 0;
  u64 entitiesDrawn = 0;
  u64 entitiesCulled = 0;
  u64 entitiesLodReduced = 0;    // drawn at Static or Reduced
  u64 batches = 0;               // frames that produced a non-empty batch
  u64 drawCalls = 0;             // triangles + lines draw calls submitted
  u64 vertices = 0;
  f32 resolutionScale = 1.0f;

  void Reset();
};

//----------------------------------------------------------------------------
// The pipeline itself
//----------------------------------------------------------------------------

/// Ties the pieces together for one frame. The device adapter owns one; a host
/// test owns one and drives it with synthetic frames. No globals, no clock.
class BERRY_API RenderPipeline {
 public:
  RenderPipeline() = default;

  /// Starts a frame: rebuilds the frustum from the camera and adopts the
  /// current settings. `settings` drives the LOD tiers; a null camera leaves
  /// the frustum empty so nothing is culled (fail open, never cull blindly).
  void BeginFrame(const CameraState& camera, const RenderSettings& settings);

  /// The entity cull decision: false when the box is outside the frustum.
  /// Updates the submitted/drawn/culled counters.
  bool SubmitEntity(const Aabb& box);
  /// The same decision from a snapshot, using its position and the shared
  /// `EntityHitbox`.
  bool SubmitEntity(const EntitySnapshot& snap);

  /// The LOD tier for an entity at `distance`, and the accounting.
  LodTier LodFor(f32 distance);

  /// Feeds a frame time to the resolution controller.
  f32 OnFrameTime(f32 frameMs);

  /// Records a built batch and returns the draw calls it implies.
  u64 SubmitBatch(const DrawBatch& batch);

  /// The same accounting from a count only, for a caller that measured the
  /// frame without materialising its vertices. Shares the draw-call rule with
  /// `SubmitBatch`.
  u64 SubmitCounts(const DrawCounts& counts);

  void EndFrame();

  const Frustum& frustum() const { return frustum_; }
  const LodPolicy& lod() const { return lod_; }
  const DynamicResolution& resolution() const { return resolution_; }
  /// A snapshot of the counters. `resolutionScale` is filled from the live
  /// controller, so it is correct even before the first frame ends.
  PipelineStats stats() const {
    PipelineStats s = stats_;
    s.resolutionScale = resolution_.scale();
    return s;
  }
  /// The settings adopted this frame.
  const RenderSettings& settings() const { return settings_; }

  void SetResolutionConfig(const DynamicResolution::Config& cfg) {
    resolution_.Configure(cfg);
  }
  /// Overrides the LOD tiers (tests, and a per-level tuning pass).
  void SetLodPolicy(const LodPolicy& p) { lod_ = p; }

  void Reset();

 private:
  Frustum frustum_;
  LodPolicy lod_ = LodPolicy::Default();
  DynamicResolution resolution_;
  RenderSettings settings_{};
  PipelineStats stats_{};
  bool inFrame_ = false;
};

}  // namespace Berry
