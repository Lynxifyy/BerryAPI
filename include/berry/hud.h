#pragma once
// BerryAPI retained-mode HUD.
//
// Scripts and client modules describe a widget TREE once; the HUD lays it out,
// animates it, and emits draw calls into the immediate-mode Renderer every
// frame. Widgets keep their identity across frames, so a value change tweens
// instead of popping and layout is only recomputed for the parts that changed
// (the "diff").
//
// This is deliberately NOT a new strict Berry:: module: the strict module set
// is fixed at Combat/Render/World/Network. The HUD is a client-side service
// that draws through Berry::Render like any other overlay consumer, and it
// holds no engine pointers of its own.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/render.h"

#include <deque>
#include <map>
#include <string>
#include <vector>

namespace Berry {

/// Stable handle for a widget in a Hud tree. 0 means "no widget".
using WidgetId = u64;

enum class Anchor {
  TopLeft, TopCenter, TopRight,
  CenterLeft, Center, CenterRight,
  BottomLeft, BottomCenter, BottomRight,
};

enum class WidgetKind {
  Panel,    // a filled rounded-ish background; a container
  Label,    // one line of text
  Bar,      // 0..1 fill inside a frame
  Graph,    // rolling sample history drawn as a polyline
  Row,      // container: children flow horizontally
  Column,   // container: children flow vertically
};

/// Semantic colour slots so a theme swap restyles the whole HUD at once.
enum class HudColorRole { Text, Muted, Accent, Background, Good, Warn, Bad };

struct BERRY_API HudTheme {
  Rgba text;
  Rgba muted;
  Rgba accent;
  Rgba background;
  Rgba good;
  Rgba warn;
  Rgba bad;

  f32 scale = 1.0f;
  f32 padding = 4.0f;
  f32 rowHeight = 14.0f;
  f32 spacing = 2.0f;
  f32 barHeight = 6.0f;
  f32 charWidth = 8.0f;

  static HudTheme Default();
  /// A compact dark theme tuned for a small phone screen.
  static HudTheme Compact();
  Rgba Role(HudColorRole r) const;
};

//----------------------------------------------------------------------------
// Animation.
//----------------------------------------------------------------------------

enum class Easing { Linear, EaseIn, EaseOut, EaseInOut };

/// Maps t in [0,1] through the easing curve; t is clamped.
BERRY_API f32 Ease(Easing e, f32 t);

/// A single scalar animation. Retargeting mid-flight restarts from the current
/// value, so a value that keeps changing never jumps.
struct BERRY_API Tween {
  f32 from = 0.0f;
  f32 to = 0.0f;
  f32 durationMs = 0.0f;
  f32 elapsedMs = 0.0f;
  Easing easing = Easing::EaseOut;
  bool active = false;

  static Tween Constant(f32 v) {
    Tween t;
    t.from = t.to = v;
    return t;
  }

  /// Current value. A finished or zero-length tween returns `to`.
  f32 Value() const;

  /// Advances by dtMs. Returns true while still animating.
  bool Advance(f32 dtMs);

  /// Starts a new animation to `target` over `ms`. A non-positive duration or a
  /// target equal to the current value snaps immediately.
  void Retarget(f32 target, f32 ms, Easing e = Easing::EaseOut);

  bool Animating() const { return active; }
};

//----------------------------------------------------------------------------
// Widget tree.
//----------------------------------------------------------------------------

struct BERRY_API Widget {
  WidgetId id = 0;
  WidgetId parent = 0;
  WidgetKind kind = WidgetKind::Label;
  std::string text;
  Anchor anchor = Anchor::TopLeft;
  Vec2 offset;             // from the anchor point, before alignment
  Vec2 size;               // zero components are measured from content
  HudColorRole color = HudColorRole::Text;
  bool visible = true;
  bool background = false; // Panel/Row/Column paint a background when true

  f32 value = 0.0f;        // Bar fill, 0..1 after Min/Max mapping
  f32 minValue = 0.0f;
  f32 maxValue = 1.0f;
  f32 thickness = 1.0f;

  std::vector<WidgetId> children;
  std::deque<f32> samples;    // Graph history (oldest first)
  size_t sampleCapacity = 60;

  Tween opacity = Tween::Constant(1.0f);
  Tween slide;                // optional y offset animation, in pixels
  Tween valueTween;           // Bar/Graph value animation

  // Layout results (owned by the Hud; read-only to callers).
  Vec2 resolvedPos;
  Vec2 resolvedSize;
  bool layoutValid = false;
};

struct BERRY_API HudStats {
  u64 frames = 0;
  u64 layoutPasses = 0;       // widgets laid out (recomputed) this frame
  u64 widgetsDrawn = 0;
  u64 primitives = 0;
};

/// Retained-mode HUD. Owns the widget tree and its animation state.
class BERRY_API Hud {
 public:
  Hud() = default;

  //--- tree construction ----------------------------------------------------
  /// Creates a root widget. The returned id is stable for the widget's life.
  WidgetId Create(WidgetKind kind, const std::string& text = std::string(),
                  Anchor anchor = Anchor::TopLeft);
  /// Creates a widget parented to `parent`, laid out by the parent's flow.
  WidgetId CreateChild(WidgetId parent, WidgetKind kind,
                       const std::string& text = std::string());

  Widget* Get(WidgetId id);
  const Widget* Get(WidgetId id) const;
  bool Remove(WidgetId id);
  void Clear();

  //--- mutation (marks layout dirty only when geometry is affected) ---------
  bool SetText(WidgetId id, const std::string& text);
  bool SetValue(WidgetId id, f32 value);
  bool SetVisible(WidgetId id, bool visible);
  bool SetColor(WidgetId id, HudColorRole role);
  bool SetOffset(WidgetId id, const Vec2& offset);
  bool SetSize(WidgetId id, const Vec2& size);

  /// Animates a Bar/Graph value to `to` over `ms`.
  bool TweenValue(WidgetId id, f32 to, f32 ms, Easing e = Easing::EaseOut);
  /// Animates a widget's opacity; 0 hides it without removing it.
  bool TweenOpacity(WidgetId id, f32 to, f32 ms, Easing e = Easing::EaseOut);
  /// Pushes one sample onto a Graph, dropping the oldest past capacity.
  bool PushSample(WidgetId id, f32 sample);

  //--- theme ---------------------------------------------------------------
  void SetTheme(const HudTheme& theme) { theme_ = theme; }
  const HudTheme& theme() const { return theme_; }

  //--- frame ---------------------------------------------------------------
  /// Advances every tween by dtMs.
  void Update(f32 dtMs);

  /// Lays out and draws the tree into `r` for a `screen`-sized surface.
  /// Only widgets whose geometry changed since the last frame are re-laid out.
  /// Must be called between Renderer::BeginFrame and EndFrame.
  void Present(Renderer& r, const Vec2& screen);

  size_t Count() const { return widgets_.size(); }
  const HudStats& stats() const { return stats_; }
  u64 revision() const { return revision_; }

  /// Read-only view of the whole tree, for the inspector and the tests.
  const std::map<WidgetId, Widget>& widgets() const { return widgets_; }

 private:
  WidgetId AllocId();
  void MarkLayoutDirty(WidgetId id);
  void MarkSubtreeDirty(WidgetId id);
  void MarkAncestorsDirty(WidgetId id);
  Vec2 Measure(const Widget& w) const;
  Vec2 AnchorBase(Anchor a, const Vec2& screen) const;
  Vec2 AlignIn(Anchor a, const Vec2& base, const Vec2& size) const;
  void LayoutWidget(Widget& w, const Vec2& screen, const Vec2& origin);
  void DrawWidget(const Widget& w, Renderer& r);

  std::map<WidgetId, Widget> widgets_;
  WidgetId nextId_ = 1;
  HudTheme theme_ = HudTheme::Default();
  HudStats stats_;
  u64 revision_ = 0;
};

}  // namespace Berry
