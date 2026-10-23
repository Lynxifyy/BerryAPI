#pragma once
// BerryAPI immediate-mode UI.
//
// The retained-mode Hud (berry/hud.h) describes a widget tree once and lets the
// Hud own it. This is the other half of the surface, and the one Flarial-shaped
// script UIs expect: an imperative API where the script builds the interface
// every frame and reads the result of the interaction inline:
//
//   if ui.begin_window("Combat", x, y, w, h) then
//     if ui.button("Reset").clicked then tracker:reset() end
//     local r = ui.slider_float("Reach", reach, 3.0, 6.0)
//     if r.changed then reach = r.value end
//     ui.end_window()
//   end
//
// Threading is the hard part. The script runs on the script worker; the pointer
// and mouse live on the render thread, and the draws must happen on the render
// thread. An immediate-mode call has to return an answer *now*, so the answer
// cannot be "ask the render thread and wait". The layer therefore splits each
// call in two:
//
//   * the worker's call *builds* the frame — it lays the widget out, records it,
//     and reads the interaction result the render thread computed for that
//     widget id on the previous frame;
//   * the render thread hit-tests the built widget rectangles against the real
//     pointer/click state, recolours the hovered/active ones, draws them, and
//     publishes one result per id.
//
// The widget id — a hash of the window and the label, or an explicit id — is the
// contract tying the two halves together. The result is a one-frame-lag
// immediate-mode UI, which is exactly how ImGui itself behaves: a click is acted
// on the frame after the press. Nothing assumes the two threads tick in
// lockstep; each direction swaps its buffers under one lock, so a slow script or
// a fast renderer only changes the latency, never the correctness.
//
// The build half runs entirely under one lock and takes no renderer; the present
// half runs entirely under the same lock, so a caller never observes a
// half-built frame. The two halves are separately callable and separately
// testable: a test can Build() without a Renderer and Present() with one.
//
// This is deliberately NOT a new strict Berry:: module (the strict set stays
// Combat/Render/World/Network). It is a client-side service that draws through
// Berry::Render like the Hud and the PvP overlay do, and it holds no engine
// pointers of its own: it reads the pointer from the InputObserver snapshot and
// draws through the Renderer seam.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/draw.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {

class Renderer;

/// Stable id for a widget in the immediate-mode layer. 0 means "no widget".
using UiId = u64;

/// Semantic colour slots, so a theme swap restyles the whole interface at once.
enum class UiColorRole {
  WindowBg, TitleBg, TitleText, Text, Muted, Accent,
  FrameBg, FrameHover, FrameActive, Border, Good, Warn, Bad,
};

/// A UI theme. The default is a flat dark theme; `Glass` is a translucent,
/// iOS-style theme. The layer cannot blur the framebuffer, so the glass look is
/// faked with a low-alpha fill, a light hairline edge and rounded corners.
struct BERRY_API UiTheme {
  Rgba windowBg;
  Rgba titleBg;
  Rgba titleText;
  Rgba text;
  Rgba muted;
  Rgba accent;
  Rgba frameBg;
  Rgba frameHover;
  Rgba frameActive;
  Rgba border;
  Rgba good;
  Rgba warn;
  Rgba bad;

  f32 fontScale = 1.0f;
  f32 charWidth = 8.0f;    // advance per character, at fontScale 1
  f32 rowHeight = 18.0f;   // height of one interactive row
  f32 padding = 6.0f;
  f32 spacing = 4.0f;
  f32 indent = 12.0f;
  f32 sliderWidth = 120.0f;
  f32 buttonWidth = 90.0f;
  f32 scrollSpeed = 24.0f; // pixels per wheel notch
  f32 rounding = 0.0f;     // corner radius in pixels (0 = square)

  static UiTheme Dark();
  /// A glassmorphic, iOS-like theme: translucent panels, a bright hairline top
  /// edge, rounded corners and a saturated accent.
  static UiTheme Glass();
  Rgba Role(UiColorRole r) const;
};

/// What happened to one widget this frame, as computed by the render thread and
/// read by the script on the next frame.
struct BERRY_API UiResult {
  bool hovered = false;   // pointer is over the widget
  bool active = false;    // the widget is being held/dragged
  bool clicked = false;   // pressed and released inside the widget
  bool changed = false;   // a slider/text value changed this interaction
  bool toggled = false;   // a checkbox flipped
  f32 value = 0.0f;       // the new value for a slider (unchanged otherwise)
  std::string text;       // the new text for an input field
  /// True for any of the above, so a script can write `if r then ... end`.
  explicit operator bool() const {
    return hovered || active || clicked || changed || toggled;
  }
};

/// What a widget is. The kind drives both the draw and the hit-test behaviour,
/// so the two halves agree on what a widget does without a second description.
enum class UiKind : u8 {
  Rect,       // a plain filled rectangle (window background, etc.)
  Text,       // non-interactive text
  Button,     // press and release inside
  Checkbox,   // a box plus label; a click toggles
  Slider,     // a horizontal drag sets a value
  InputText,  // a click focuses; typing edits
  Header,     // a collapsing header or a window title bar; a click toggles
  Tab,        // a tab item; a click selects
  Progress,   // a 0..1 fill
  Separator,  // a horizontal rule
  Bullet,     // a bullet plus text
};

/// One widget laid out during the build half. The build records everything the
/// render half needs: where it is, what it is, and the state a draw or a
/// hit-test depends on. The render half never re-derives layout.
struct BERRY_API UiWidget {
  UiId id = 0;
  UiId window = 0;         // the window the widget belongs to (0 = none)
  UiKind kind = UiKind::Rect;
  Vec2 min, max;
  std::string label;
  Rgba color;              // the base colour the build chose
  f32 fontScale = 1.0f;

  // Interactive state carried from the build.
  bool interactive = false;   // participates in hover/click
  bool toggleOn = false;      // checkbox/header current state
  bool windowBar = false;     // a Header that is a window title bar (draggable)
  f32 rangeLo = 0.0f;         // slider range
  f32 rangeHi = 1.0f;
  f32 value = 0.0f;           // slider current value
  std::string text;           // input-field current text
  f32 fraction = 0.0f;        // progress 0..1
  f32 rounding = 0.0f;        // corner radius the build chose
  i32 clipIndex = -1;         // the clip rect in effect, or -1 for none
};

/// A clip rectangle in effect for the widgets that name it.
struct BERRY_API UiClipRect {
  Vec2 min, max;
};

/// One window's geometry for a frame, so the render half can hit-test a wheel
/// scroll against it without scanning the widget list.
struct BERRY_API UiWindowRect {
  UiId id = 0;
  Vec2 min, max;             // the whole window
  Vec2 contentMin, contentMax;  // the scrollable content region
};

struct BERRY_API UiStats {
  u64 frames = 0;
  u64 windows = 0;
  u64 widgets = 0;
  u64 hovered = 0;
  u64 clicked = 0;
};

/// One window's persistent state: where a drag has moved it, whether it is
/// collapsed, and how far its content is scrolled.
struct BERRY_API UiWindowState {
  UiId id = 0;
  Vec2 dragOffset;         // added to the position the script passes in
  f32 scrollY = 0.0f;
  f32 contentHeight = 0.0f;
  bool collapsed = false;
  bool dragging = false;
};

/// The immediate-mode UI layer. Thread-safe: the build half runs on the script
/// worker, the present half on the render thread.
class BERRY_API UiLayer {
 public:
  static constexpr size_t kMaxWidgetsPerFrame = 512;
  static constexpr size_t kMaxClips = 64;
  static constexpr size_t kMaxTextBytes = 256;
  static constexpr size_t kMaxIndent = 8;
  static constexpr size_t kMaxNestedWindows = 8;
  static constexpr size_t kMaxTabBars = 8;

  UiLayer();

  //--- worker half: build the frame ----------------------------------------

  /// Starts a new frame: clears the widget and clip lists the previous frame
  /// left behind. The script calls this once per frame before the first
  /// `begin_window`.
  void BeginFrame();

  /// Publishes the frame the worker just built so the render thread can draw it
  /// and hit-test it. Called once per frame after the last `end_window`.
  void EndFrame();

  /// Discards an in-progress frame without publishing it. The binding layer
  /// calls this when a script faults between `begin_window` and `end_window`,
  /// so the half-built frame is never presented and the next frame starts
  /// clean rather than inheriting a stale layout stack.
  void AbortFrame();

  /// True while a frame is being built (between BeginFrame and EndFrame).
  bool building() const;

  /// Opens a window at `pos` (a drag offset from a previous frame is applied)
  /// and returns whether its content should be built: false when the window is
  /// collapsed (a collapsed window draws only its title bar). `size` is the
  /// initial content size; the window grows to fit its widgets.
  bool BeginWindow(const std::string& title, const Vec2& pos, const Vec2& size);
  void EndWindow();

  /// A push button. `result.clicked` is true on the frame the press is released
  /// inside it.
  UiResult Button(const std::string& label);
  /// A checkbox. Pass the current value; `result.toggled` and `result.value`
  /// (0 or 1) describe the new one.
  UiResult Checkbox(const std::string& label, bool value);
  /// A horizontal slider. Pass the current value; a drag returns the new one in
  /// `result.value` with `result.changed` set.
  UiResult SliderFloat(const std::string& label, f32 value, f32 lo, f32 hi);
  /// An integer slider, rounding the drag to whole steps.
  UiResult SliderInt(const std::string& label, i32 value, i32 lo, i32 hi);
  /// A single-line text field. Pass the current text; typed characters arrive
  /// in `result.text` with `result.changed` set. Backspace and Enter are
  /// handled; the field stops accepting input at kMaxTextBytes.
  UiResult InputText(const std::string& label, const std::string& value);

  /// A collapsible section header. Pass the current open state; the returned
  /// value is the new one. While open the caller builds the children, indented.
  bool CollapsingHeader(const std::string& label, bool open);

  /// A tab bar. Returns false when the bar could not be opened (too many
  /// nested bars), in which case the caller must not call EndTabBar.
  bool BeginTabBar(const std::string& id);
  void EndTabBar();
  /// One tab. Returns true when this tab is the selected one, so the caller
  /// builds its content. Every BeginTabItem must be paired with EndTabItem.
  bool BeginTabItem(const std::string& label);
  void EndTabItem();

  /// Non-interactive text at the current cursor.
  void Text(const std::string& text);
  /// Coloured text using a semantic role.
  void TextColored(UiColorRole role, const std::string& text);
  /// A 0..1 progress bar with an optional centred overlay.
  void ProgressBar(f32 fraction, const std::string& overlay = std::string());
  /// A horizontal rule across the content width.
  void Separator();
  /// A blank row.
  void Spacing();
  /// Keeps the next widget on the same line as the previous one.
  void SameLine();
  /// Reserves an empty box of `size`.
  void Dummy(const Vec2& size);
  /// Indents subsequent widgets by one level; Unindent() reverses one level.
  void Indent();
  void Unindent();
  /// A bulleted line, drawn as a bullet plus text.
  void BulletText(const std::string& text);

  /// The window's content width, or 0 when no window is open.
  f32 contentWidth() const;

  /// An explicit widget id from a string, for widgets whose label repeats.
  static UiId HashId(const std::string& s);

  //--- render half: present and hit-test -----------------------------------

  /// The pointer and mouse state for this frame, supplied by the render thread
  /// from the InputObserver snapshot.
  struct Input {
    Vec2 pointer;
    bool mouseDown = false;
    f32 wheel = 0.0f;
    /// Characters typed since the last frame (UTF-8 bytes), for text fields.
    std::string typed;
    /// True when Backspace was pressed since the last frame.
    bool backspace = false;
    /// True when Enter was pressed since the last frame.
    bool enter = false;
  };

  /// Presents the frame the worker published: hit-tests its widgets against
  /// `input`, draws them through `r`, and publishes the interaction results for
  /// the worker to read next frame. Must be called between Renderer::BeginFrame
  /// and Renderer::EndFrame. Returns the number of widgets drawn.
  size_t Present(Renderer& r, const Input& input);

  /// Hit-tests the published frame without drawing, publishing the results.
  /// Split out so the interaction model is testable without a renderer.
  size_t Update(const Input& input);

  //--- shared state --------------------------------------------------------

  void SetTheme(const UiTheme& theme);
  UiTheme theme() const;
  UiStats stats() const;
  u64 revision() const;

  /// The result the render thread published for a widget id, or a default (all
  /// false) result when the id is unknown. Read by the worker during a build.
  UiResult Result(UiId id) const;

  /// True while any window is being dragged, so a caller can suppress camera
  /// input. Published by the render half.
  bool anyWindowDragging() const;

  /// Drops every window's persistent state (drag offsets, scroll, collapse).
  void Reset();

  /// The windows the render half knows about, for tests and the inspector.
  std::vector<UiWindowState> Windows() const;

  /// The widgets of the frame currently being presented (or the last presented
  /// frame), for tests.
  std::vector<UiWidget> Widgets() const;

 private:
  struct Frame {
    std::vector<UiWidget> widgets;
    std::vector<UiClipRect> clips;
    std::vector<UiWindowRect> windows;
    u64 revision = 0;
  };

  /// A window on the build stack. It carries the layout cursor and the clip.
  struct WindowFrame {
    UiId id = 0;
    Vec2 pos;           // window top-left, after the drag offset
    Vec2 size;          // content size the script asked for
    Vec2 contentMin;    // content region min (clip top-left)
    Vec2 contentMax;    // content region max
    Vec2 cursor;        // pen: where the next fresh line starts
    Vec2 lineStart;     // the current line's left edge
    f32 lineY = 0;      // the current line's y (for SameLine)
    f32 prevMaxX = 0;   // the previous widget's right edge (for SameLine)
    f32 contentW = 0;
    f32 maxY = 0;       // furthest the content has reached
    f32 scrollY = 0;
    bool open = false;  // false when collapsed
    bool sameLine = false;
    u32 indent = 0;
    i32 clipIndex = -1;
  };

  struct TabBar {
    UiId id = 0;
    i32 selected = 0;
    i32 current = -1;
  };

  // Worker-half helpers. Each is called with mutex_ held.
  UiId NextId(const std::string& label);
  WindowFrame* Top();
  /// Places a widget of `size` at the pen, honouring SameLine(), and advances
  /// the pen. Returns the widget's top-left corner. This is the single place
  /// layout is computed, so every widget agrees on the cursor rules.
  Vec2 AllocBox(const Vec2& size);
  void PushWidget(const UiWidget& w);
  f32 TextWidth(const std::string& s) const;
  void EmitText(const Vec2& pos, const std::string& s, const Rgba& c, f32 scale);
  /// Reads a published result without taking the lock. Every public builder
  /// holds the lock and calls this, so the interaction read cannot race the
  /// render half's publish.
  UiResult ResultLocked(UiId id) const;
  /// Takes a one-shot toggle for `id`: returns true once and clears the flag, so
  /// a collapse toggle is applied on exactly one build. A published result
  /// persists until the next present, so reading `toggled` twice would flip the
  /// state back.
  bool ConsumeToggleLocked(UiId id);
  /// Lays out one labelled row: the label at the row start and a control box of
  /// `boxSize` right-aligned in the content width. Returns the box rectangle.
  void RowBox(const std::string& label, const Vec2& boxSize, Vec2* outMin,
              Vec2* outMax);

  // Render-half helpers. Each is called with mutex_ held.
  void HitTest(Frame& frame, const Input& input,
               std::unordered_map<UiId, UiResult>* out);
  void DrawWidgets(Renderer& r, const Frame& frame,
                   const std::unordered_map<UiId, UiResult>& results);

  mutable std::mutex mutex_;
  UiTheme theme_ = UiTheme::Dark();
  UiStats stats_;
  u64 revision_ = 0;

  // Build half (worker thread).
  Frame build_;
  std::vector<WindowFrame> windows_;
  std::unordered_map<UiId, UiWindowState> windowState_;
  std::vector<TabBar> tabBars_;
  bool building_ = false;

  // Published from the render half for the worker to read.
  std::unordered_map<UiId, UiResult> results_;
  bool anyWindowDragging_ = false;

  // The frame handed from the worker to the renderer, and the one being built.
  Frame ready_;
  bool hasReady_ = false;

  // Render-half transient state, carried across frames (a drag spans frames).
  UiId activeWidget_ = 0;   // the widget a press started on
  UiId dragWindow_ = 0;     // the window whose title bar is being dragged
  UiId activeTextField_ = 0;
  Vec2 dragStartPointer_;
  Vec2 dragStartOffset_;
  std::string editBuffer_;  // the focused text field's in-progress value
};

}  // namespace Berry
