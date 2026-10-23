#pragma once
#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/game.h"
#include "berry/event.h"
#include "berry/perf.h"
#include "berry/draw.h"
#include "berry/pipeline.h"
#include <functional>
#include <string>
#include <vector>
#include <mutex>

namespace Berry {

//----------------------------------------------------------------------------
// The strict module.
//----------------------------------------------------------------------------

struct RenderStats {
  u64 frameCount = 0;
  f32 frameMs = 0;
  u64 drawCalls = 0;
  size_t primitives = 0;
  /// Primitives dropped this frame because they fell entirely outside the
  /// active clip rectangle.
  u64 clipped = 0;
};

class BERRY_API Renderer {
 public:
  /// Deepest scissor nesting the renderer accepts. A script that pushes more
  /// is refused rather than allowed to grow the stack without bound.
  static constexpr size_t kMaxClipDepth = 32;

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

  /// Projects a world-space billboard with the current camera and queues it for
  /// this frame. Returns false (and draws nothing) when the camera is invalid
  /// or the billboard is off-screen or behind the camera, so a caller can skip
  /// the work. Must be called between BeginFrame and EndFrame.
  bool DrawBillboard(const Billboard3D& bb);
  void DrawTextCentered(const Vec2& center, const char* text, const Rgba& color, f32 scale);

  //--- world-space (3D) debug geometry --------------------------------------
  //
  // Every 3D helper projects through the current camera and then queues the
  // same 2D primitives as the immediate-mode API, so 3D debug geometry needs no
  // new backend state: it is a line or a quad in the one batch.

  /// Projects a world point to pixels. Returns false when no valid camera is
  /// set or the point is behind the camera; an off-screen point still projects
  /// (so a line crossing the viewport can be clipped rather than dropped).
  bool ProjectWorld(const Vec3& world, Vec2* screen) const;

  /// Draws a world-space line. The segment is clipped against the active clip
  /// rectangle (and dropped when an endpoint is behind the camera), so a line
  /// that runs off-screen is trimmed instead of vanishing.
  void Draw3DLine(const Vec3& a, const Vec3& b, const Rgba& color,
                  f32 thickness = 1.0f);

  /// Draws the 12 edges of an axis-aligned world box (`min`..`max`).
  void Draw3DBox(const Vec3& min, const Vec3& max, const Rgba& color,
                 f32 thickness = 1.0f);

  /// Draws text at a world position (anchored at the projected point).
  /// Nothing is drawn when the point is behind the camera or outside the clip.
  void Draw3DText(const Vec3& world, const char* text, const Rgba& color,
                  f32 scale = 1.0f);

  //--- scissor stack --------------------------------------------------------
  //
  // A nested clip rectangle. Pushing intersects with the current top, so a
  // child clip can only ever narrow its parent. Clipping is applied on the CPU
  // as primitives are queued:
  //   * rects and lines are clipped exactly (a rect is intersected, a line is
  //     trimmed with Liang-Barsky);
  //   * text and circles have no exact clip representation in the batch, so
  //     they are kept whenever they intersect the clip at all and dropped only
  //     when they fall entirely outside — conservative in the safe direction.
  // The stack resets each frame, and an unmatched Pop is a no-op.
  void PushClipRect(const Rect2D& rect);
  void PopClipRect();
  /// The active (intersected) clip rectangle, or false when the stack is empty.
  bool clipRect(Rect2D* out) const;
  size_t clipDepth() const;

  // ESP helpers (need camera)
  void SetCamera(const CameraState& cam);
  /// Snapshot of the camera the last frame was drawn with.
  CameraState camera() const;
  bool EspBoxForEntity(const EntitySnapshot& ent, const CameraState& cam,
                       EspBox* out) const;

  // Debug overlay state
  bool showOverlay() const { return showOverlay_; }
  void SetShowOverlay(bool v) { showOverlay_ = v; }
  const RenderStats& stats() const { return stats_; }

  //--- frame pipeline -------------------------------------------------------
  //
  // The frame path that decides what is worth drawing: a frustum cull, a LOD
  // tier per entity, a dynamic-resolution scale, and the coalesced vertex batch
  // the backend uploads. It is driven from BeginFrame/EndFrame, so a caller
  // only has to bind a camera and feed the governor's settings.
  //
  // `pipeline()` hands out the object itself and takes no lock: it is for a
  // caller that already owns the frame (a test, or the render thread inside a
  // frame). Every cross-thread use goes through the locked methods below.
  RenderPipeline& pipeline() { return pipeline_; }
  const RenderPipeline& pipeline() const { return pipeline_; }

  /// The governor's settings, adopted at the next BeginFrame. Default (unset)
  /// means no culling and the default LOD tiers. Thread-safe.
  void SetRenderSettings(const RenderSettings& s);
  RenderSettings renderSettings() const;
  /// Feeds one frame time to the resolution controller. Thread-safe; called
  /// from the frame publisher, which may not be inside a frame. When the scale
  /// changes, the seam is told so it can render at the new fraction.
  f32 OnFrameTime(f32 frameMs);
  /// A snapshot of the pipeline counters. Thread-safe.
  PipelineStats pipelineStats() const;
  /// The shape of the batch the last frame produced (vertex count, draw calls),
  /// without the vertices. Lets a script read what the frame actually cost.
  DrawCounts lastBatchCounts() const;
  /// The LOD tier for an entity at `distance` under the frame's policy.
  LodTier LodFor(f32 distance) const;
  /// Whether an entity at `distance` should advance its animation/physics on
  /// `tick` under the frame's LOD policy, keyed by `phase` (its id). A caller
  /// throttles a distant entity by not advancing it on a false result, which is
  /// the tick-rate half of LOD; `LodFor` alone only reports the tier. Not const:
  /// it records the skipped-tick accounting.
  bool ShouldTickEntity(f32 distance, u64 tick, u64 phase);
  /// True when `box` is inside the last frame's frustum. Entities are drawn
  /// through this so a caller never queues geometry the camera cannot see.
  bool Visible(const Aabb& box) const;

  /// Rasterises a world box into this frame's occlusion grid as an opaque
  /// occluder. A caller submits large near geometry (terrain, walls) front to
  /// back; only boxes the grid proves hidden are then culled. Thread-safe.
  void AddOccluder(const Aabb& box);
  /// True when `box` is entirely behind the occluders submitted this frame.
  /// False with no camera, no occluders, or any doubt — it never over-culls.
  bool IsOccluded(const Aabb& box) const;
  /// The full cull decision: false when `box` is outside the frustum or hidden
  /// behind an occluder. Records the drop in the pipeline counters. Thread-safe.
  bool CullEntity(const Aabb& box);

 private:
  /// True when `box` intersects the active clip (or no clip is active). When
  /// `exact` is set, `box` is narrowed to the intersection.
  bool ClipBox(Rect2D* box, bool exact) const;

  RenderSeam* seam_ = nullptr;
  bool inFrame_ = false;
  bool showOverlay_ = true;

  std::vector<Line2D> lines_;
  std::vector<Text2D> texts_;
  std::vector<Rect2D> rects_;
  std::vector<Circle2D> circles_;
  mutable std::mutex mutex_;
  CameraState cam_;
  std::vector<Rect2D> clips_;
  RenderStats stats_;
  /// The frame pipeline: cull, LOD, resolution, batch accounting. Guarded by
  /// the same mutex as the draw lists, because BeginFrame/EndFrame drive both.
  RenderPipeline pipeline_;
  RenderSettings settings_{};
  DrawCounts lastCounts_{};
  /// The render scale last pushed to the seam, so it is told only on a change.
  f32 lastPushedScale_ = 1.0f;

  void PresentLocked();
};

}  // namespace Berry