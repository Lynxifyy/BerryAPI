// Immediate-mode UI layer. See include/berry/ui.h for the threading contract.
#include "berry/ui.h"

#include "berry/render.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Berry {
namespace {

f32 Clampf(f32 v, f32 lo, f32 hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

bool InRect(const Vec2& p, const Vec2& min, const Vec2& max) {
  return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
}

/// Truncates a string to `maxChars` characters, appending an ellipsis when it
/// was cut, so a long label cannot draw across the whole window.
std::string ClipText(const std::string& s, size_t maxChars) {
  if (s.size() <= maxChars) return s;
  if (maxChars <= 1) return s.substr(0, maxChars);
  return s.substr(0, maxChars - 1) + "\xE2\x80\xA6";  // U+2026
}

/// A filled rounded rectangle drawn as a centre band plus four edge bands and
/// four corner chords. The batch has no arc primitive, so a corner is
/// approximated by `kCornerSteps` short thick lines cut across it. With rounding
/// 0 the whole thing collapses to the single rectangle, so a flat theme pays
/// nothing. Outlines stay sharp (`DrawRect`): a rounded outline would need the
/// same chord walk on all four sides for no visual gain at these sizes.
constexpr int kCornerSteps = 3;

void FillRoundedRect(Renderer& r, const Vec2& min, const Vec2& max,
                     const Rgba& fill, f32 radius) {
  const f32 w = max.x - min.x;
  const f32 h = max.y - min.y;
  if (w <= 0 || h <= 0) return;
  f32 rad = radius;
  rad = std::min(rad, std::min(w, h) * 0.5f);
  if (rad <= 0.5f) {
    Rect2D rr;
    rr.min = min;
    rr.max = max;
    rr.fill = fill;
    rr.filled = true;
    r.DrawRect(rr);
    return;
  }

  // The cross: a full-width band between the corner rows, plus a full-height
  // band in the middle. The four corner squares are then filled by the chords.
  Rect2D mid;
  mid.min = Vec2(min.x, min.y + rad);
  mid.max = Vec2(max.x, max.y - rad);
  mid.fill = fill;
  mid.filled = true;
  r.DrawRect(mid);

  Rect2D top;
  top.min = Vec2(min.x + rad, min.y);
  top.max = Vec2(max.x - rad, min.y + rad);
  top.fill = fill;
  top.filled = true;
  r.DrawRect(top);

  Rect2D bot;
  bot.min = Vec2(min.x + rad, max.y - rad);
  bot.max = Vec2(max.x - rad, max.y);
  bot.fill = fill;
  bot.filled = true;
  r.DrawRect(bot);

  // The four corner chords. A quarter circle from `baseAngle` sweeping 90
  // degrees; the thick line joins consecutive points so the corner reads solid.
  const Vec2 centre[4] = {
      Vec2(min.x + rad, min.y + rad),  // top-left
      Vec2(max.x - rad, min.y + rad),  // top-right
      Vec2(max.x - rad, max.y - rad),  // bottom-right
      Vec2(min.x + rad, max.y - rad),  // bottom-left
  };
  const f32 baseAngle[4] = {180.0f, 270.0f, 0.0f, 90.0f};
  const f32 kPi = 3.14159265358979f;
  for (int c = 0; c < 4; ++c) {
    Vec2 prev;
    for (int i = 0; i <= kCornerSteps; ++i) {
      const f32 a = (baseAngle[c] + 90.0f * (f32)i / (f32)kCornerSteps) * kPi / 180.0f;
      const Vec2 p(centre[c].x + std::cos(a) * rad, centre[c].y + std::sin(a) * rad);
      if (i > 0) r.DrawLine(prev, p, fill, rad * 0.7f);
      prev = p;
    }
  }
}

}  // namespace

//----------------------------------------------------------------------------
// Theme
//----------------------------------------------------------------------------

UiTheme UiTheme::Dark() {
  UiTheme t;
  t.windowBg = Rgba(18, 20, 26, 235);
  t.titleBg = Rgba(30, 34, 44, 245);
  t.titleText = Rgba(235, 238, 245);
  t.text = Rgba(220, 224, 232);
  t.muted = Rgba(150, 156, 170);
  t.accent = Rgba(88, 140, 255);
  t.frameBg = Rgba(38, 42, 54, 235);
  t.frameHover = Rgba(52, 58, 74, 245);
  t.frameActive = Rgba(70, 110, 200, 255);
  t.border = Rgba(70, 76, 92);
  t.good = Rgba(70, 210, 120);
  t.warn = Rgba(240, 190, 60);
  t.bad = Rgba(240, 80, 80);
  t.rounding = 3.0f;
  return t;
}

UiTheme UiTheme::Glass() {
  UiTheme t;
  // A frosted look: low-alpha panels (so the world shows through), a brighter
  // hairline border standing in for the specular edge, generous rounding and a
  // saturated accent. There is no blur in the batch, so the translucency is what
  // sells the material.
  t.windowBg = Rgba(24, 26, 34, 150);
  t.titleBg = Rgba(255, 255, 255, 28);
  t.titleText = Rgba(245, 247, 252);
  t.text = Rgba(238, 240, 248);
  t.muted = Rgba(178, 184, 198);
  t.accent = Rgba(10, 132, 255);
  t.frameBg = Rgba(255, 255, 255, 22);
  t.frameHover = Rgba(255, 255, 255, 40);
  t.frameActive = Rgba(10, 132, 255, 200);
  t.border = Rgba(255, 255, 255, 90);
  t.good = Rgba(48, 209, 88);
  t.warn = Rgba(255, 214, 10);
  t.bad = Rgba(255, 69, 58);
  t.padding = 10.0f;
  t.spacing = 8.0f;
  t.rowHeight = 22.0f;
  t.rounding = 10.0f;
  return t;
}

Rgba UiTheme::Role(UiColorRole r) const {
  switch (r) {
    case UiColorRole::WindowBg: return windowBg;
    case UiColorRole::TitleBg: return titleBg;
    case UiColorRole::TitleText: return titleText;
    case UiColorRole::Text: return text;
    case UiColorRole::Muted: return muted;
    case UiColorRole::Accent: return accent;
    case UiColorRole::FrameBg: return frameBg;
    case UiColorRole::FrameHover: return frameHover;
    case UiColorRole::FrameActive: return frameActive;
    case UiColorRole::Border: return border;
    case UiColorRole::Good: return good;
    case UiColorRole::Warn: return warn;
    case UiColorRole::Bad: return bad;
  }
  return text;
}

//----------------------------------------------------------------------------
// Construction
//----------------------------------------------------------------------------

UiLayer::UiLayer() = default;

UiId UiLayer::HashId(const std::string& s) {
  // FNV-1a: cheap, stable, and good enough that two labels in one window do not
  // collide in practice. The high bit is forced on so a real id is never 0
  // (which means "no widget").
  u64 h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= (u64)c;
    h *= 1099511628211ULL;
  }
  return h | (1ULL << 63);
}

UiId UiLayer::NextId(const std::string& label) {
  const WindowFrame* w = Top();
  const UiId win = w ? w->id : 0;
  // The id is (window, label): the same label in two windows is two widgets, and
  // the same label twice in one window is one widget, so a repeated label shares
  // interaction state (the ImGui contract).
  return HashId(std::to_string(win) + "|" + label);
}

//----------------------------------------------------------------------------
// Frame lifecycle (worker half)
//----------------------------------------------------------------------------

void UiLayer::BeginFrame() {
  std::lock_guard<std::mutex> lk(mutex_);
  build_.widgets.clear();
  build_.clips.clear();
  build_.windows.clear();
  windows_.clear();
  tabBars_.clear();
  building_ = true;
}

void UiLayer::EndFrame() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!building_) return;
  building_ = false;
  // A window the script forgot to close is dropped here rather than carried into
  // the next frame; the layout stack is per-frame by construction.
  windows_.clear();
  ++revision_;
  build_.revision = revision_;
  ready_ = build_;
  hasReady_ = true;
  ++stats_.frames;
  stats_.windows = build_.windows.size();
  stats_.widgets = build_.widgets.size();
}

void UiLayer::AbortFrame() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!building_) return;
  building_ = false;
  // Drop the half-built frame; the previously published one stays as the last
  // good frame, so a fault mid-build shows the last complete UI rather than a
  // torn one.
  windows_.clear();
  tabBars_.clear();
  build_.widgets.clear();
  build_.clips.clear();
  build_.windows.clear();
}

bool UiLayer::building() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return building_;
}

//----------------------------------------------------------------------------
// Windows
//----------------------------------------------------------------------------

UiLayer::WindowFrame* UiLayer::Top() {
  return windows_.empty() ? nullptr : &windows_.back();
}

bool UiLayer::BeginWindow(const std::string& title, const Vec2& pos,
                          const Vec2& size) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!building_) return false;
  if (windows_.size() >= kMaxNestedWindows) return false;
  if (build_.widgets.size() >= kMaxWidgetsPerFrame) return false;

  const UiId id = HashId("window:" + title);
  UiWindowState& ws = windowState_[id];
  ws.id = id;
  // The title-bar result from the last frame toggles the collapse here, at the
  // top of the build, so a collapsed window that builds no content still honours
  // the click that expanded it. Consumed, not merely read: a published result
  // lives until the next present, and toggling on every build in between would
  // flip the state back and forth.
  if (ConsumeToggleLocked(id)) ws.collapsed = !ws.collapsed;

  WindowFrame w;
  w.id = id;
  w.size = size;
  w.pos = Vec2(pos.x + ws.dragOffset.x, pos.y + ws.dragOffset.y);
  w.scrollY = ws.scrollY;

  const f32 titleH = theme_.rowHeight + theme_.padding * 0.5f;
  // `size` is the visible content viewport: content taller than it scrolls. The
  // window's drawn height is the title bar plus that viewport.
  const f32 viewW = std::max(size.x, theme_.buttonWidth);
  const f32 viewH = std::max(size.y, theme_.rowHeight);
  w.contentMin = Vec2(w.pos.x + theme_.padding, w.pos.y + titleH + theme_.spacing);
  w.contentMax = Vec2(w.contentMin.x + viewW - theme_.padding * 2.0f,
                      w.contentMin.y + viewH - theme_.padding);
  // The scroll offset shifts the content up as the user scrolls down.
  const Vec2 viewOrigin = w.contentMin;
  w.contentMin.y -= w.scrollY;
  w.contentMax.y -= w.scrollY;
  w.cursor = w.contentMin;
  w.lineStart = w.cursor;
  w.lineY = w.cursor.y;
  w.prevMaxX = w.contentMin.x;
  w.contentW = w.contentMax.x - w.contentMin.x;
  w.open = !ws.collapsed;
  if (build_.clips.size() < kMaxClips) {
    build_.clips.push_back(UiClipRect{w.contentMin, w.contentMax});
    w.clipIndex = (i32)(build_.clips.size() - 1);
  }

  // The window's own rectangle: fixed to the viewport, so it does not move as
  // content scrolls. Collapsed, it is just the title bar.
  const Vec2 winMax(w.pos.x + viewW, w.pos.y + titleH + viewH);
  UiWindowRect wr;
  wr.id = id;
  wr.min = w.pos;
  wr.max = w.open ? winMax : Vec2(winMax.x, w.pos.y + titleH);
  wr.contentMin = viewOrigin;
  wr.contentMax = Vec2(viewOrigin.x + viewW - theme_.padding * 2.0f,
                       viewOrigin.y + viewH - theme_.padding);
  build_.windows.push_back(wr);

  // The window background, drawn first so every widget lands on top.
  UiWidget bg;
  bg.kind = UiKind::Rect;
  bg.window = id;
  bg.min = w.pos;
  bg.max = wr.max;
  bg.color = theme_.Role(UiColorRole::WindowBg);
  bg.rounding = theme_.rounding;
  bg.clipIndex = -1;
  PushWidget(bg);

  // The title bar: always built (a collapsed window still shows it).
  UiWidget bar;
  bar.id = id;  // the window id doubles as the title-bar id
  bar.window = id;
  bar.kind = UiKind::Header;
  bar.min = w.pos;
  bar.max = Vec2(wr.max.x, w.pos.y + titleH);
  bar.label = title;
  bar.color = theme_.Role(UiColorRole::TitleBg);
  bar.fontScale = theme_.fontScale;
  bar.interactive = true;
  bar.toggleOn = !ws.collapsed;
  bar.windowBar = true;
  bar.rounding = theme_.rounding;
  bar.clipIndex = -1;
  PushWidget(bar);

  windows_.push_back(w);
  ++stats_.windows;

  if (!w.open) {
    // Collapsed: the caller must still call EndWindow, so report false and park
    // the pen so no widget lands inside the content region.
    windows_.back().cursor = Vec2(0, 0);
    windows_.back().lineStart = Vec2(0, 0);
    windows_.back().lineY = 0;
    windows_.back().prevMaxX = 0;
    windows_.back().contentW = 0;
  }
  return w.open;
}

void UiLayer::EndWindow() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (windows_.empty()) return;
  WindowFrame w = windows_.back();
  windows_.pop_back();

  UiWindowState& ws = windowState_[w.id];
  if (!w.open) return;
  ws.contentHeight = w.maxY - w.contentMin.y;
  ws.scrollY = w.scrollY;
}

Vec2 UiLayer::AllocBox(const Vec2& size) {
  WindowFrame* w = Top();
  if (!w || !w->open) return Vec2(0, 0);
  Vec2 pos;
  if (w->sameLine) {
    // Continue the current line: same y, just after the previous widget.
    pos = Vec2(w->prevMaxX + theme_.spacing, w->lineY);
    w->sameLine = false;
  } else {
    // A fresh line below the previous one.
    pos = Vec2(w->lineStart.x, w->cursor.y);
  }
  w->prevMaxX = pos.x + size.x;
  w->lineY = pos.y;
  w->cursor = Vec2(w->lineStart.x, pos.y + size.y + theme_.spacing);
  w->maxY = std::max(w->maxY, pos.y + size.y);
  return pos;
}

f32 UiLayer::TextWidth(const std::string& s) const {
  return (f32)s.size() * theme_.charWidth * theme_.fontScale;
}

void UiLayer::PushWidget(const UiWidget& w) {
  if (build_.widgets.size() >= kMaxWidgetsPerFrame) return;
  build_.widgets.push_back(w);
}

void UiLayer::EmitText(const Vec2& pos, const std::string& s, const Rgba& c,
                       f32 scale) {
  UiWidget w;
  w.kind = UiKind::Text;
  w.min = pos;
  w.max = Vec2(pos.x + TextWidth(s), pos.y + theme_.rowHeight);
  w.label = s;
  w.color = c;
  w.fontScale = scale;
  const WindowFrame* win = Top();
  w.window = win ? win->id : 0;
  w.clipIndex = win ? win->clipIndex : -1;
  PushWidget(w);
}

void UiLayer::RowBox(const std::string& label, const Vec2& boxSize, Vec2* outMin,
                     Vec2* outMax) {
  WindowFrame* w = Top();
  if (!w) {
    if (outMin) *outMin = Vec2(0, 0);
    if (outMax) *outMax = Vec2(0, 0);
    return;
  }
  const f32 h = std::max(boxSize.y, theme_.rowHeight);
  const f32 indentX = (f32)w->indent * theme_.indent;
  // Reserve the whole row with the layout allocator, then place the control box
  // at its right edge so sliders and fields line up down the column.
  const Vec2 rowPos = AllocBox(Vec2(w->contentW, h));
  const f32 rowX = rowPos.x + indentX;
  const f32 right = w->contentMin.x + w->contentW;
  const f32 boxX = std::max(rowX + TextWidth(label) + theme_.spacing,
                            right - boxSize.x - indentX);
  if (outMin) *outMin = Vec2(boxX, rowPos.y);
  if (outMax) *outMax = Vec2(boxX + boxSize.x, rowPos.y + h);
}

//----------------------------------------------------------------------------
// Widgets (worker half)
//----------------------------------------------------------------------------

UiResult UiLayer::Button(const std::string& label) {
  std::lock_guard<std::mutex> lk(mutex_);
  UiResult r;
  WindowFrame* w = Top();
  if (!w || !w->open) return r;

  const UiId id = NextId("button:" + label);
  r = ResultLocked(id);

  const f32 indentX = (f32)w->indent * theme_.indent;
  const f32 width = std::max(theme_.buttonWidth, TextWidth(label) + theme_.padding * 2);
  const Vec2 alloc = AllocBox(Vec2(indentX + width, theme_.rowHeight));
  const Vec2 min(alloc.x + indentX, alloc.y);
  const Vec2 max(min.x + width, min.y + theme_.rowHeight);

  UiWidget wd;
  wd.id = id;
  wd.window = w->id;
  wd.kind = UiKind::Button;
  wd.min = min;
  wd.max = max;
  wd.label = label;
  wd.color = theme_.Role(UiColorRole::FrameBg);
  wd.fontScale = theme_.fontScale;
  wd.interactive = true;
  wd.rounding = theme_.rounding;
  wd.clipIndex = w->clipIndex;
  PushWidget(wd);
  return r;
}

UiResult UiLayer::Checkbox(const std::string& label, bool value) {
  std::lock_guard<std::mutex> lk(mutex_);
  UiResult r;
  WindowFrame* w = Top();
  if (!w || !w->open) return r;

  const UiId id = NextId("check:" + label);
  r = ResultLocked(id);
  // One-shot: the toggle is taken once, so a checkbox does not flip on every
  // build until the next present.
  r.toggled = ConsumeToggleLocked(id);
  r.changed = r.toggled;

  const f32 box = theme_.rowHeight - 4.0f;
  const f32 indentX = (f32)w->indent * theme_.indent;
  const f32 width = box + theme_.spacing + TextWidth(label);
  const Vec2 alloc = AllocBox(Vec2(indentX + width, theme_.rowHeight));
  const Vec2 min(alloc.x + indentX, alloc.y);
  const Vec2 max(min.x + width, min.y + theme_.rowHeight);

  UiWidget wd;
  wd.id = id;
  wd.window = w->id;
  wd.kind = UiKind::Checkbox;
  wd.min = min;
  wd.max = max;
  wd.label = label;
  wd.color = theme_.Role(UiColorRole::Text);
  wd.fontScale = theme_.fontScale;
  wd.interactive = true;
  wd.toggleOn = value;
  wd.clipIndex = w->clipIndex;
  PushWidget(wd);

  if (r.toggled) r.value = value ? 0.0f : 1.0f;
  return r;
}

UiResult UiLayer::SliderFloat(const std::string& label, f32 value, f32 lo, f32 hi) {
  std::lock_guard<std::mutex> lk(mutex_);
  UiResult r;
  WindowFrame* w = Top();
  if (!w || !w->open) return r;
  // A degenerate range cannot be dragged; fail to a fixed value rather than
  // dividing by zero.
  if (!(hi > lo) || !std::isfinite(lo) || !std::isfinite(hi)) {
    r.value = value;
    return r;
  }

  const UiId id = NextId("slider:" + label);
  r = ResultLocked(id);
  if (r.changed) r.value = Clampf(r.value, lo, hi);
  else r.value = Clampf(value, lo, hi);

  Vec2 min, max;
  RowBox(label, Vec2(theme_.sliderWidth, theme_.rowHeight), &min, &max);

  UiWidget wd;
  wd.id = id;
  wd.window = w->id;
  wd.kind = UiKind::Slider;
  wd.min = min;
  wd.max = max;
  wd.label = label;
  wd.color = theme_.Role(UiColorRole::FrameBg);
  wd.fontScale = theme_.fontScale;
  wd.interactive = true;
  wd.rangeLo = lo;
  wd.rangeHi = hi;
  wd.value = r.value;
  wd.rounding = theme_.rounding;
  wd.clipIndex = w->clipIndex;
  PushWidget(wd);

  // The label is drawn to the left of the box.
  UiWidget lab;
  lab.kind = UiKind::Text;
  lab.window = w->id;
  lab.min = Vec2(min.x - TextWidth(label) - theme_.spacing, min.y);
  lab.max = Vec2(min.x, min.y + theme_.rowHeight);
  lab.label = label;
  lab.color = theme_.Role(UiColorRole::Text);
  lab.fontScale = theme_.fontScale;
  lab.clipIndex = w->clipIndex;
  PushWidget(lab);
  return r;
}

UiResult UiLayer::SliderInt(const std::string& label, i32 value, i32 lo, i32 hi) {
  UiResult r = SliderFloat(label, (f32)value, (f32)lo, (f32)hi);
  if (r.changed) {
    // Snap the drag to whole steps; the render half computes a float fraction,
    // so rounding here keeps the script's value integral.
    r.value = std::round(r.value);
  } else {
    r.value = (f32)value;
  }
  return r;
}

UiResult UiLayer::InputText(const std::string& label, const std::string& value) {
  std::lock_guard<std::mutex> lk(mutex_);
  UiResult r;
  WindowFrame* w = Top();
  if (!w || !w->open) return r;

  const UiId id = NextId("input:" + label);
  r = ResultLocked(id);
  // The render half owns the in-progress buffer; until it reports an edit, the
  // script's value is authoritative.
  if (!r.changed) r.text = value;

  Vec2 min, max;
  RowBox(label, Vec2(theme_.sliderWidth, theme_.rowHeight), &min, &max);

  UiWidget wd;
  wd.id = id;
  wd.window = w->id;
  wd.kind = UiKind::InputText;
  wd.min = min;
  wd.max = max;
  wd.label = label;
  wd.color = theme_.Role(UiColorRole::FrameBg);
  wd.fontScale = theme_.fontScale;
  wd.interactive = true;
  wd.text = value;
  wd.rounding = theme_.rounding;
  wd.clipIndex = w->clipIndex;
  PushWidget(wd);

  UiWidget lab;
  lab.kind = UiKind::Text;
  lab.window = w->id;
  lab.min = Vec2(min.x - TextWidth(label) - theme_.spacing, min.y);
  lab.max = Vec2(min.x, min.y + theme_.rowHeight);
  lab.label = label;
  lab.color = theme_.Role(UiColorRole::Text);
  lab.fontScale = theme_.fontScale;
  lab.clipIndex = w->clipIndex;
  PushWidget(lab);
  return r;
}

bool UiLayer::CollapsingHeader(const std::string& label, bool open) {
  std::lock_guard<std::mutex> lk(mutex_);
  WindowFrame* w = Top();
  if (!w || !w->open) return open;

  const UiId id = NextId("header:" + label);
  const bool toggled = ConsumeToggleLocked(id);
  const bool nowOpen = toggled ? !open : open;

  const f32 indentX = (f32)w->indent * theme_.indent;
  const f32 width = std::max(w->contentW - indentX, theme_.buttonWidth);
  const Vec2 alloc = AllocBox(Vec2(indentX + width, theme_.rowHeight));
  const Vec2 min(alloc.x + indentX, alloc.y);
  const Vec2 max(min.x + width, min.y + theme_.rowHeight);

  UiWidget wd;
  wd.id = id;
  wd.window = w->id;
  wd.kind = UiKind::Header;
  wd.min = min;
  wd.max = max;
  wd.label = label;
  wd.color = theme_.Role(UiColorRole::FrameBg);
  wd.fontScale = theme_.fontScale;
  wd.interactive = true;
  wd.toggleOn = nowOpen;
  wd.rounding = theme_.rounding;
  wd.clipIndex = w->clipIndex;
  PushWidget(wd);
  return nowOpen;
}

bool UiLayer::BeginTabBar(const std::string& id) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!building_) return false;
  if (tabBars_.size() >= kMaxTabBars) return false;
  TabBar tb;
  tb.id = NextId("tabbar:" + id);
  tabBars_.push_back(tb);
  return true;
}

void UiLayer::EndTabBar() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!tabBars_.empty()) tabBars_.pop_back();
}

bool UiLayer::BeginTabItem(const std::string& label) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (tabBars_.empty()) return false;
  WindowFrame* w = Top();
  if (!w || !w->open) return false;

  TabBar& tb = tabBars_.back();
  const UiId id = HashId(std::to_string(tb.id) + "|tab:" + label);
  // Advance to this tab's index first, so a click selects the tab that was
  // actually clicked rather than the one before it.
  ++tb.current;
  if (ConsumeToggleLocked(id)) tb.selected = tb.current;
  const bool selected = (tb.current == tb.selected);

  const f32 indentX = (f32)w->indent * theme_.indent;
  const f32 tabW = TextWidth(label) + theme_.padding * 2;
  // Tabs flow horizontally: the first opens the line, the rest continue it.
  if (tb.current > 0) w->sameLine = true;
  const Vec2 alloc = AllocBox(Vec2(indentX + tabW, theme_.rowHeight));
  const Vec2 min(alloc.x + indentX, alloc.y);
  const Vec2 max(min.x + tabW, min.y + theme_.rowHeight);

  UiWidget wd;
  wd.id = id;
  wd.window = w->id;
  wd.kind = UiKind::Tab;
  wd.min = min;
  wd.max = max;
  wd.label = label;
  wd.color = selected ? theme_.Role(UiColorRole::FrameActive)
                      : theme_.Role(UiColorRole::FrameBg);
  wd.fontScale = theme_.fontScale;
  wd.interactive = true;
  wd.toggleOn = selected;
  wd.rounding = theme_.rounding;
  wd.clipIndex = w->clipIndex;
  PushWidget(wd);
  return selected;
}

void UiLayer::EndTabItem() {
  std::lock_guard<std::mutex> lk(mutex_);
  // Tabs already advanced the cursor horizontally; the next widget opens a new
  // line because sameLine is false.
}

void UiLayer::Text(const std::string& text) {
  // Not delegating to TextColored(): that takes the same non-recursive lock.
  std::lock_guard<std::mutex> lk(mutex_);
  WindowFrame* w = Top();
  if (!w || !w->open) return;
  EmitText(AllocBox(Vec2(w->contentW, theme_.rowHeight)), text,
           theme_.Role(UiColorRole::Text), theme_.fontScale);
}

void UiLayer::TextColored(UiColorRole role, const std::string& text) {
  std::lock_guard<std::mutex> lk(mutex_);
  WindowFrame* w = Top();
  if (!w || !w->open) return;
  EmitText(AllocBox(Vec2(w->contentW, theme_.rowHeight)), text,
           theme_.Role(role), theme_.fontScale);
}

void UiLayer::ProgressBar(f32 fraction, const std::string& overlay) {
  std::lock_guard<std::mutex> lk(mutex_);
  WindowFrame* w = Top();
  if (!w || !w->open) return;
  const f32 f = Clampf(fraction, 0.0f, 1.0f);
  const f32 indentX = (f32)w->indent * theme_.indent;
  const Vec2 alloc = AllocBox(Vec2(w->contentW, theme_.rowHeight));
  const Vec2 min(alloc.x + indentX, alloc.y);
  const Vec2 max(w->contentMin.x + w->contentW, alloc.y + theme_.rowHeight);

  UiWidget wd;
  wd.window = w->id;
  wd.kind = UiKind::Progress;
  wd.min = min;
  wd.max = max;
  wd.label = overlay;
  wd.color = theme_.Role(UiColorRole::Accent);
  wd.fontScale = theme_.fontScale;
  wd.fraction = f;
  wd.rounding = theme_.rounding;
  wd.clipIndex = w->clipIndex;
  PushWidget(wd);
}

void UiLayer::Separator() {
  std::lock_guard<std::mutex> lk(mutex_);
  WindowFrame* w = Top();
  if (!w || !w->open) return;
  const f32 indentX = (f32)w->indent * theme_.indent;
  const Vec2 alloc = AllocBox(Vec2(w->contentW, theme_.spacing));

  UiWidget wd;
  wd.kind = UiKind::Separator;
  wd.window = w->id;
  wd.min = Vec2(alloc.x + indentX, alloc.y);
  wd.max = Vec2(w->contentMin.x + w->contentW, alloc.y);
  wd.color = theme_.Role(UiColorRole::Border);
  wd.clipIndex = w->clipIndex;
  PushWidget(wd);
}

void UiLayer::Spacing() {
  std::lock_guard<std::mutex> lk(mutex_);
  WindowFrame* w = Top();
  if (!w || !w->open) return;
  AllocBox(Vec2(0, theme_.spacing));
}

void UiLayer::SameLine() {
  std::lock_guard<std::mutex> lk(mutex_);
  WindowFrame* w = Top();
  if (!w || !w->open) return;
  w->sameLine = true;
}

void UiLayer::Dummy(const Vec2& size) {
  std::lock_guard<std::mutex> lk(mutex_);
  WindowFrame* w = Top();
  if (!w || !w->open) return;
  AllocBox(size);
}

void UiLayer::Indent() {
  std::lock_guard<std::mutex> lk(mutex_);
  WindowFrame* w = Top();
  if (!w || !w->open) return;
  if (w->indent < kMaxIndent) ++w->indent;
}

void UiLayer::Unindent() {
  std::lock_guard<std::mutex> lk(mutex_);
  WindowFrame* w = Top();
  if (!w || !w->open) return;
  if (w->indent > 0) --w->indent;
}

void UiLayer::BulletText(const std::string& text) {
  std::lock_guard<std::mutex> lk(mutex_);
  WindowFrame* w = Top();
  if (!w || !w->open) return;
  const f32 indentX = (f32)w->indent * theme_.indent;
  const Vec2 alloc = AllocBox(Vec2(w->contentW, theme_.rowHeight));

  UiWidget wd;
  wd.kind = UiKind::Bullet;
  wd.window = w->id;
  wd.min = Vec2(alloc.x + indentX, alloc.y);
  wd.max = Vec2(w->contentMin.x + w->contentW, alloc.y + theme_.rowHeight);
  wd.label = text;
  wd.color = theme_.Role(UiColorRole::Text);
  wd.fontScale = theme_.fontScale;
  wd.clipIndex = w->clipIndex;
  PushWidget(wd);
}

f32 UiLayer::contentWidth() const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (windows_.empty()) return 0.0f;
  return windows_.back().contentW;
}

//----------------------------------------------------------------------------
// Shared state
//----------------------------------------------------------------------------

void UiLayer::SetTheme(const UiTheme& theme) {
  std::lock_guard<std::mutex> lk(mutex_);
  theme_ = theme;
}

UiTheme UiLayer::theme() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return theme_;
}

UiResult UiLayer::Result(UiId id) const {
  std::lock_guard<std::mutex> lk(mutex_);
  return ResultLocked(id);
}

UiResult UiLayer::ResultLocked(UiId id) const {
  auto it = results_.find(id);
  if (it == results_.end()) return UiResult{};
  return it->second;
}

bool UiLayer::ConsumeToggleLocked(UiId id) {
  auto it = results_.find(id);
  if (it == results_.end() || !it->second.toggled) return false;
  it->second.toggled = false;
  return true;
}

UiStats UiLayer::stats() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return stats_;
}

u64 UiLayer::revision() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return revision_;
}

bool UiLayer::anyWindowDragging() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return anyWindowDragging_;
}

void UiLayer::Reset() {
  std::lock_guard<std::mutex> lk(mutex_);
  windowState_.clear();
  results_.clear();
  activeWidget_ = 0;
  dragWindow_ = 0;
  activeTextField_ = 0;
  editBuffer_.clear();
}

std::vector<UiWindowState> UiLayer::Windows() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<UiWindowState> out;
  out.reserve(windowState_.size());
  for (const auto& kv : windowState_) out.push_back(kv.second);
  return out;
}

std::vector<UiWidget> UiLayer::Widgets() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return ready_.widgets;
}

//----------------------------------------------------------------------------
// Render half: hit-test
//----------------------------------------------------------------------------

void UiLayer::HitTest(Frame& frame, const Input& input,
                      std::unordered_map<UiId, UiResult>* out) {
  out->clear();
  bool dragging = false;

  // A press begins on the topmost interactive widget under the pointer. Widgets
  // are in build order, so scan in reverse and take the first hit.
  UiId hitId = 0;
  for (auto it = frame.widgets.rbegin(); it != frame.widgets.rend(); ++it) {
    if (!it->interactive || it->id == 0) continue;
    if (!InRect(input.pointer, it->min, it->max)) continue;
    hitId = it->id;
    break;
  }

  if (input.mouseDown && activeWidget_ == 0) {
    if (hitId != 0) {
      activeWidget_ = hitId;
      // Remember where the press began for every widget: the release uses it to
      // tell a click from a drag, and a collapsing header needs it as much as a
      // window title bar does.
      dragStartPointer_ = input.pointer;
      for (const UiWidget& w : frame.widgets) {
        if (w.id == hitId && w.windowBar) {
          dragWindow_ = w.window;
          dragStartOffset_ = windowState_[w.window].dragOffset;
        }
      }
    } else {
      // Clicking empty space drops text focus.
      activeTextField_ = 0;
    }
  }

  for (UiWidget& w : frame.widgets) {
    if (w.id == 0 || !w.interactive) continue;
    UiResult r;
    r.hovered = InRect(input.pointer, w.min, w.max);
    r.active = (activeWidget_ == w.id);

    if (w.kind == UiKind::Slider) {
      if (r.active && input.mouseDown) {
        const f32 span = std::max(w.max.x - w.min.x, 1.0f);
        const f32 frac = Clampf((input.pointer.x - w.min.x) / span, 0.0f, 1.0f);
        const f32 newValue = w.rangeLo + frac * (w.rangeHi - w.rangeLo);
        r.value = newValue;
        if (std::fabs(newValue - w.value) > 1e-6f) r.changed = true;
      } else {
        r.value = w.value;
      }
    }

    if (w.kind == UiKind::Checkbox && r.active && !input.mouseDown) {
      // Release inside a checkbox toggles it.
      r.toggled = true;
      r.changed = true;
    }

    (*out)[w.id] = r;
  }

  // Release: a widget the press started on reports `clicked` if the pointer is
  // still inside it (buttons and tabs). A release over a window title bar or a
  // collapsing header toggles it only if the pointer barely moved, so a drag
  // that moved the window does not also collapse it.
  if (!input.mouseDown && activeWidget_ != 0) {
    for (UiWidget& w : frame.widgets) {
      if (w.id != activeWidget_) continue;
      const bool inside = InRect(input.pointer, w.min, w.max);
      UiResult& r = (*out)[w.id];
      r.active = false;
      const Vec2 d = input.pointer - dragStartPointer_;
      const bool barelyMoved = std::fabs(d.x) < 4.0f && std::fabs(d.y) < 4.0f;
      if (inside && (w.kind == UiKind::Button || w.kind == UiKind::Tab)) {
        r.clicked = true;
      }
      // A tab selection is a one-shot toggle the build consumes; a header (a
      // collapsing section or a window title bar) toggles only on a click that
      // barely moved, so a drag that moved the window does not also collapse it.
      if (inside && w.kind == UiKind::Tab) r.toggled = true;
      if (inside && w.kind == UiKind::Header && barelyMoved) r.toggled = true;
      break;
    }
    activeWidget_ = 0;
    dragWindow_ = 0;
  }

  // The window title drag.
  if (dragWindow_ != 0 && input.mouseDown) {
    const Vec2 d = input.pointer - dragStartPointer_;
    if (std::fabs(d.x) > 3.0f || std::fabs(d.y) > 3.0f) {
      UiWindowState& ws = windowState_[dragWindow_];
      ws.dragOffset = Vec2(dragStartOffset_.x + d.x, dragStartOffset_.y + d.y);
      ws.dragging = true;
      dragging = true;
    }
  } else if (!input.mouseDown && dragWindow_ != 0) {
    windowState_[dragWindow_].dragging = false;
    dragWindow_ = 0;
  }
  anyWindowDragging_ = dragging;

  // The wheel scrolls the window under the pointer, clamped to the content.
  if (std::fabs(input.wheel) > 1e-4f) {
    for (const UiWindowRect& wr : frame.windows) {
      if (!InRect(input.pointer, wr.min, wr.max)) continue;
      UiWindowState& ws = windowState_[wr.id];
      const f32 viewH = wr.contentMax.y - wr.contentMin.y;
      const f32 maxScroll = std::max(0.0f, ws.contentHeight - viewH);
      ws.scrollY = Clampf(ws.scrollY - input.wheel * theme_.scrollSpeed, 0.0f, maxScroll);
      break;
    }
  }

  // Text fields: a press inside focuses one; the focused field consumes typed
  // characters, backspace and enter.
  if (input.mouseDown) {
    for (const UiWidget& w : frame.widgets) {
      if (w.kind == UiKind::InputText && InRect(input.pointer, w.min, w.max)) {
        if (activeTextField_ != w.id) {
          activeTextField_ = w.id;
          editBuffer_ = w.text;
        }
      }
    }
  }
  if (activeTextField_ != 0) {
    bool edited = false;
    if (!input.typed.empty() && editBuffer_.size() < kMaxTextBytes) {
      editBuffer_ += input.typed;
      if (editBuffer_.size() > kMaxTextBytes) editBuffer_.resize(kMaxTextBytes);
      edited = true;
    }
    if (input.backspace && !editBuffer_.empty()) {
      // Erase one UTF-8 character: drop continuation bytes then the lead byte.
      size_t n = editBuffer_.size();
      while (n > 0 && ((unsigned char)editBuffer_[n - 1] & 0xC0) == 0x80) --n;
      if (n > 0) --n;
      editBuffer_.resize(n);
      edited = true;
    }
    if (edited) {
      UiResult& r = (*out)[activeTextField_];
      r.changed = true;
      r.text = editBuffer_;
    }
    if (input.enter) {
      activeTextField_ = 0;
      editBuffer_.clear();
    }
  }
}

//----------------------------------------------------------------------------
// Render half: draw
//----------------------------------------------------------------------------

void UiLayer::DrawWidgets(Renderer& r, const Frame& frame,
                          const std::unordered_map<UiId, UiResult>& results) {
  const UiTheme& t = theme_;
  const UiClipRect full{Vec2(-1e9f, -1e9f), Vec2(1e9f, 1e9f)};

  for (const UiWidget& w : frame.widgets) {
    const UiClipRect clip =
        (w.clipIndex >= 0 && (size_t)w.clipIndex < frame.clips.size())
            ? frame.clips[(size_t)w.clipIndex]
            : full;
    Rect2D scissor;
    scissor.min = clip.min;
    scissor.max = clip.max;
    r.PushClipRect(scissor);

    UiResult res;
    auto it = results.find(w.id);
    if (it != results.end()) res = it->second;

    switch (w.kind) {
      case UiKind::Rect:
        FillRoundedRect(r, w.min, w.max, w.color, w.rounding);
        break;
      case UiKind::Text:
      case UiKind::Bullet: {
        Vec2 pos = w.min;
        if (w.kind == UiKind::Bullet) {
          r.DrawCircle(Vec2(pos.x + 3.0f, pos.y + theme_.rowHeight * 0.5f), 2.0f,
                       w.color, 8);
          pos.x += theme_.indent;
        }
        r.DrawText(pos, ClipText(w.label, 96).c_str(), w.color, w.fontScale);
        break;
      }
      case UiKind::Button: {
        const Rgba fill = res.active ? t.Role(UiColorRole::FrameActive)
                          : res.hovered ? t.Role(UiColorRole::FrameHover)
                                        : w.color;
        FillRoundedRect(r, w.min, w.max, fill, w.rounding);
        Rect2D border;
        border.min = w.min;
        border.max = w.max;
        border.color = t.Role(UiColorRole::Border);
        border.filled = false;
        r.DrawRect(border);
        r.DrawTextCentered(Vec2((w.min.x + w.max.x) * 0.5f, (w.min.y + w.max.y) * 0.5f),
                           ClipText(w.label, 48).c_str(), t.Role(UiColorRole::TitleText),
                           w.fontScale);
        break;
      }
      case UiKind::Checkbox: {
        const f32 box = w.max.y - w.min.y - 4.0f;
        Rect2D boxR;
        boxR.min = Vec2(w.min.x, w.min.y + 2.0f);
        boxR.max = Vec2(w.min.x + box, w.min.y + 2.0f + box);
        boxR.fill = res.hovered ? t.Role(UiColorRole::FrameHover)
                                : t.Role(UiColorRole::FrameBg);
        boxR.filled = true;
        r.DrawRect(boxR);
        Rect2D bb;
        bb.min = boxR.min;
        bb.max = boxR.max;
        bb.color = t.Role(UiColorRole::Border);
        bb.filled = false;
        r.DrawRect(bb);
        if (w.toggleOn) {
          const Vec2 c((boxR.min.x + boxR.max.x) * 0.5f,
                       (boxR.min.y + boxR.max.y) * 0.5f);
          const f32 s = box * 0.28f;
          r.DrawLine(Vec2(c.x - s, c.y), Vec2(c.x - s * 0.2f, c.y + s),
                     t.Role(UiColorRole::Good), 2.0f);
          r.DrawLine(Vec2(c.x - s * 0.2f, c.y + s), Vec2(c.x + s, c.y - s),
                     t.Role(UiColorRole::Good), 2.0f);
        }
        r.DrawText(Vec2(boxR.max.x + theme_.spacing, w.min.y),
                   ClipText(w.label, 64).c_str(), t.Role(UiColorRole::Text), w.fontScale);
        break;
      }
      case UiKind::Slider: {
        const Rgba fill = res.active ? t.Role(UiColorRole::FrameActive)
                          : res.hovered ? t.Role(UiColorRole::FrameHover) : w.color;
        FillRoundedRect(r, w.min, w.max, fill, w.rounding);
        f32 frac = 0.0f;
        if (w.rangeHi > w.rangeLo) frac = (w.value - w.rangeLo) / (w.rangeHi - w.rangeLo);
        frac = Clampf(frac, 0.0f, 1.0f);
        if (frac > 0.0f) {
          FillRoundedRect(r, w.min,
                          Vec2(w.min.x + (w.max.x - w.min.x) * frac, w.max.y),
                          t.Role(UiColorRole::Accent), w.rounding);
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", (double)w.value);
        r.DrawTextCentered(Vec2((w.min.x + w.max.x) * 0.5f, (w.min.y + w.max.y) * 0.5f),
                           buf, t.Role(UiColorRole::TitleText), w.fontScale);
        break;
      }
      case UiKind::InputText: {
        FillRoundedRect(r, w.min, w.max, w.color, w.rounding);
        Rect2D bb;
        bb.min = w.min;
        bb.max = w.max;
        bb.color = (activeTextField_ == w.id) ? t.Role(UiColorRole::Accent)
                                              : t.Role(UiColorRole::Border);
        bb.filled = false;
        r.DrawRect(bb);
        const std::string shown = (activeTextField_ == w.id) ? editBuffer_ : w.text;
        r.DrawText(Vec2(w.min.x + theme_.spacing, w.min.y + 2.0f),
                   ClipText(shown, 48).c_str(), t.Role(UiColorRole::Text), w.fontScale);
        break;
      }
      case UiKind::Header: {
        FillRoundedRect(r, w.min, w.max,
                        res.hovered ? t.Role(UiColorRole::FrameHover) : w.color,
                        w.rounding);
        // A collapse arrow: right when open, down when collapsed. A window title
        // bar carries the same arrow (the window toggles between open/collapsed).
        const Vec2 a(w.min.x + theme_.padding, (w.min.y + w.max.y) * 0.5f);
        if (w.toggleOn) {
          r.DrawLine(Vec2(a.x, a.y - 3.0f), Vec2(a.x + 4.0f, a.y + 1.0f),
                     t.Role(UiColorRole::TitleText), 2.0f);
          r.DrawLine(Vec2(a.x + 4.0f, a.y + 1.0f), Vec2(a.x + 8.0f, a.y - 3.0f),
                     t.Role(UiColorRole::TitleText), 2.0f);
        } else {
          r.DrawLine(Vec2(a.x + 1.0f, a.y - 3.0f), Vec2(a.x + 5.0f, a.y + 1.0f),
                     t.Role(UiColorRole::TitleText), 2.0f);
          r.DrawLine(Vec2(a.x + 5.0f, a.y + 1.0f), Vec2(a.x + 1.0f, a.y + 5.0f),
                     t.Role(UiColorRole::TitleText), 2.0f);
        }
        r.DrawText(Vec2(a.x + 12.0f, (w.min.y + w.max.y) * 0.5f - 6.0f),
                   ClipText(w.label, 64).c_str(), t.Role(UiColorRole::TitleText),
                   w.fontScale);
        break;
      }
      case UiKind::Tab: {
        FillRoundedRect(r, w.min, w.max,
                        w.toggleOn ? t.Role(UiColorRole::FrameActive)
                                   : (res.hovered ? t.Role(UiColorRole::FrameHover)
                                                  : w.color),
                        w.rounding);
        r.DrawTextCentered(Vec2((w.min.x + w.max.x) * 0.5f, (w.min.y + w.max.y) * 0.5f),
                           ClipText(w.label, 32).c_str(), t.Role(UiColorRole::TitleText),
                           w.fontScale);
        break;
      }
      case UiKind::Progress: {
        FillRoundedRect(r, w.min, w.max, t.Role(UiColorRole::FrameBg), w.rounding);
        if (w.fraction > 0.0f) {
          FillRoundedRect(r, w.min,
                          Vec2(w.min.x + (w.max.x - w.min.x) * w.fraction, w.max.y),
                          w.color, w.rounding);
        }
        if (!w.label.empty()) {
          r.DrawTextCentered(Vec2((w.min.x + w.max.x) * 0.5f, (w.min.y + w.max.y) * 0.5f),
                             ClipText(w.label, 48).c_str(), t.Role(UiColorRole::TitleText),
                             w.fontScale);
        }
        break;
      }
      case UiKind::Separator: {
        r.DrawLine(w.min, Vec2(w.max.x, w.min.y), w.color, 1.0f);
        break;
      }
    }
    r.PopClipRect();
  }
}

size_t UiLayer::Update(const Input& input) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!hasReady_) return 0;
  std::unordered_map<UiId, UiResult> results;
  HitTest(ready_, input, &results);
  results_ = results;
  stats_.hovered = 0;
  stats_.clicked = 0;
  for (const auto& kv : results) {
    if (kv.second.hovered) ++stats_.hovered;
    if (kv.second.clicked) ++stats_.clicked;
  }
  return ready_.widgets.size();
}

size_t UiLayer::Present(Renderer& r, const Input& input) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!hasReady_) return 0;
  std::unordered_map<UiId, UiResult> results;
  HitTest(ready_, input, &results);
  results_ = results;
  // Assign rather than accumulate: Present may run more than once for a single
  // built frame, and a counter a script reads should describe this frame, not a
  // running total that double-counts a repeated present. `frames` stays a true
  // cumulative frame counter.
  stats_.hovered = 0;
  stats_.clicked = 0;
  for (const auto& kv : results) {
    if (kv.second.hovered) ++stats_.hovered;
    if (kv.second.clicked) ++stats_.clicked;
  }
  DrawWidgets(r, ready_, results);
  return ready_.widgets.size();
}

}  // namespace Berry
