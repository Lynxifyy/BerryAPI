// Retained-mode HUD implementation. See include/berry/hud.h.
#include "berry/hud.h"
#include "berry/logger.h"

#include <algorithm>
#include <cmath>

namespace Berry {

//----------------------------------------------------------------------------
// HudTheme
//----------------------------------------------------------------------------

HudTheme HudTheme::Default() {
  HudTheme t;
  t.text = Rgba(235, 235, 235);
  t.muted = Rgba(150, 150, 160);
  t.accent = Rgba(90, 170, 255);
  t.background = Rgba(15, 15, 20, 170);
  t.good = Rgba(80, 220, 120);
  t.warn = Rgba(250, 200, 70);
  t.bad = Rgba(240, 80, 80);
  return t;
}

HudTheme HudTheme::Compact() {
  HudTheme t = Default();
  t.scale = 0.85f;
  t.padding = 3.0f;
  t.rowHeight = 12.0f;
  t.spacing = 1.0f;
  t.barHeight = 5.0f;
  t.charWidth = 7.0f;
  t.background = Rgba(10, 10, 14, 150);
  return t;
}

Rgba HudTheme::Role(HudColorRole r) const {
  switch (r) {
    case HudColorRole::Text: return text;
    case HudColorRole::Muted: return muted;
    case HudColorRole::Accent: return accent;
    case HudColorRole::Background: return background;
    case HudColorRole::Good: return good;
    case HudColorRole::Warn: return warn;
    case HudColorRole::Bad: return bad;
  }
  return text;
}

//----------------------------------------------------------------------------
// Easing + Tween
//----------------------------------------------------------------------------

f32 Ease(Easing e, f32 t) {
  if (!(t > 0.0f)) return 0.0f;   // also catches NaN
  if (t >= 1.0f) return 1.0f;
  switch (e) {
    case Easing::Linear: return t;
    case Easing::EaseIn: return t * t;
    case Easing::EaseOut: return 1.0f - (1.0f - t) * (1.0f - t);
    case Easing::EaseInOut:
      return t < 0.5f ? 2.0f * t * t
                      : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
  }
  return t;
}

f32 Tween::Value() const {
  if (!active || durationMs <= 0.0f) return to;
  const f32 t = elapsedMs / durationMs;
  return from + (to - from) * Ease(easing, t);
}

bool Tween::Advance(f32 dtMs) {
  if (!active) return false;
  if (dtMs > 0.0f) elapsedMs += dtMs;
  if (elapsedMs >= durationMs) {
    elapsedMs = durationMs;
    active = false;
    return false;
  }
  return true;
}

void Tween::Retarget(f32 target, f32 ms, Easing e) {
  const f32 current = Value();
  if (ms <= 0.0f || target == current) {
    from = to = target;
    elapsedMs = durationMs = 0.0f;
    active = false;
    return;
  }
  from = current;
  to = target;
  durationMs = ms;
  elapsedMs = 0.0f;
  easing = e;
  active = true;
}

//----------------------------------------------------------------------------
// Hud
//----------------------------------------------------------------------------

WidgetId Hud::AllocId() {
  while (nextId_ == 0 || widgets_.count(nextId_)) ++nextId_;
  return nextId_++;
}

WidgetId Hud::Create(WidgetKind kind, const std::string& text, Anchor anchor) {
  Widget w;
  w.id = AllocId();
  w.kind = kind;
  w.text = text;
  w.anchor = anchor;
  w.opacity = Tween::Constant(1.0f);
  const WidgetId id = w.id;
  widgets_.emplace(id, std::move(w));
  ++revision_;
  return id;
}

WidgetId Hud::CreateChild(WidgetId parent, WidgetKind kind,
                          const std::string& text) {
  Widget* p = Get(parent);
  if (!p) return 0;
  WidgetId id = Create(kind, text, p->anchor);
  widgets_[id].parent = parent;
  widgets_[id].anchor = Anchor::TopLeft;   // flow children ignore the anchor
  p = Get(parent);
  if (p) {
    p->children.push_back(id);
    MarkSubtreeDirty(parent);
  }
  return id;
}

Widget* Hud::Get(WidgetId id) {
  auto it = widgets_.find(id);
  return it == widgets_.end() ? nullptr : &it->second;
}

const Widget* Hud::Get(WidgetId id) const {
  auto it = widgets_.find(id);
  return it == widgets_.end() ? nullptr : &it->second;
}

bool Hud::Remove(WidgetId id) {
  auto it = widgets_.find(id);
  if (it == widgets_.end()) return false;
  // Remove from the parent's child list.
  Widget* p = Get(it->second.parent);
  if (p) {
    p->children.erase(std::remove(p->children.begin(), p->children.end(), id),
                      p->children.end());
  }
  // Remove the subtree iteratively (children are keyed by id, no ownership).
  std::vector<WidgetId> stack{id};
  while (!stack.empty()) {
    WidgetId cur = stack.back();
    stack.pop_back();
    Widget* w = Get(cur);
    if (!w) continue;
    for (WidgetId c : w->children) stack.push_back(c);
    widgets_.erase(cur);
  }
  if (p) MarkSubtreeDirty(p->id);
  ++revision_;
  return true;
}

void Hud::Clear() {
  widgets_.clear();
  ++revision_;
}

void Hud::MarkAncestorsDirty(WidgetId id) {
  // Layout runs from the root, so invalidating a descendant must also
  // invalidate every ancestor on the path or the pass would never reach it.
  Widget* w = Get(id);
  WidgetId p = w ? w->parent : 0;
  while (p != 0) {
    Widget* pw = Get(p);
    if (!pw) break;
    pw->layoutValid = false;
    p = pw->parent;
  }
}

void Hud::MarkLayoutDirty(WidgetId id) {
  Widget* w = Get(id);
  if (!w) return;
  w->layoutValid = false;
  MarkAncestorsDirty(id);
}

void Hud::MarkSubtreeDirty(WidgetId id) {
  if (!Get(id)) return;
  std::vector<WidgetId> stack{id};
  while (!stack.empty()) {
    WidgetId cur = stack.back();
    stack.pop_back();
    Widget* w = Get(cur);
    if (!w) continue;
    w->layoutValid = false;
    for (WidgetId c : w->children) stack.push_back(c);
  }
  MarkAncestorsDirty(id);
}

bool Hud::SetText(WidgetId id, const std::string& text) {
  Widget* w = Get(id);
  if (!w || w->text == text) return w != nullptr;
  w->text = text;
  MarkSubtreeDirty(id);   // text changes the measured size
  ++revision_;
  return true;
}

bool Hud::SetValue(WidgetId id, f32 value) {
  Widget* w = Get(id);
  if (!w) return false;
  // A direct set cancels any in-flight tween, so the value lands where asked.
  w->valueTween = Tween::Constant(value);
  w->value = value;
  ++revision_;
  return true;
}

bool Hud::SetVisible(WidgetId id, bool visible) {
  Widget* w = Get(id);
  if (!w) return false;
  w->visible = visible;
  ++revision_;
  return true;
}

bool Hud::SetColor(WidgetId id, HudColorRole role) {
  Widget* w = Get(id);
  if (!w) return false;
  w->color = role;
  ++revision_;
  return true;
}

bool Hud::SetOffset(WidgetId id, const Vec2& offset) {
  Widget* w = Get(id);
  if (!w) return false;
  w->offset = offset;
  MarkLayoutDirty(id);
  ++revision_;
  return true;
}

bool Hud::SetSize(WidgetId id, const Vec2& size) {
  Widget* w = Get(id);
  if (!w) return false;
  w->size = size;
  MarkSubtreeDirty(id);
  ++revision_;
  return true;
}

bool Hud::TweenValue(WidgetId id, f32 to, f32 ms, Easing e) {
  Widget* w = Get(id);
  if (!w) return false;
  // Retargeting starts from the current animated value, so a rapidly changing
  // target (DPS, health) never jumps.
  w->valueTween.Retarget(to, ms, e);
  if (!w->valueTween.Animating()) w->value = w->valueTween.Value();
  ++revision_;
  return true;
}

bool Hud::TweenOpacity(WidgetId id, f32 to, f32 ms, Easing e) {
  Widget* w = Get(id);
  if (!w) return false;
  w->opacity.Retarget(to, ms, e);
  ++revision_;
  return true;
}

bool Hud::PushSample(WidgetId id, f32 sample) {
  Widget* w = Get(id);
  if (!w) return false;
  if (w->sampleCapacity == 0) return false;
  w->samples.push_back(sample);
  while (w->samples.size() > w->sampleCapacity) w->samples.pop_front();
  ++revision_;
  return true;
}

void Hud::Update(f32 dtMs) {
  for (auto& kv : widgets_) {
    Widget& w = kv.second;
    w.opacity.Advance(dtMs);
    w.slide.Advance(dtMs);
    if (w.valueTween.active) {
      // Advance then sample, so the final step lands exactly on the target
      // (Advance returns false once the tween completes).
      w.valueTween.Advance(dtMs);
      w.value = w.valueTween.Value();
    }
  }
}

Vec2 Hud::Measure(const Widget& w) const {
  if (w.size.x > 0.0f && w.size.y > 0.0f) return w.size;
  Vec2 content(0, 0);
  switch (w.kind) {
    case WidgetKind::Label:
      content = Vec2((f32)w.text.size() * theme_.charWidth * theme_.scale,
                     theme_.rowHeight * theme_.scale);
      break;
    case WidgetKind::Bar:
      content = Vec2(80.0f * theme_.scale, theme_.barHeight * theme_.scale);
      break;
    case WidgetKind::Graph:
      content = Vec2(80.0f * theme_.scale, 24.0f * theme_.scale);
      break;
    case WidgetKind::Panel:
    case WidgetKind::Row:
    case WidgetKind::Column: {
      f32 wmax = 0, hsum = 0, hmax = 0, wsum = 0;
      for (WidgetId c : w.children) {
        const Widget* cw = Get(c);
        if (!cw) continue;
        Vec2 cs = Measure(*cw);
        wmax = std::max(wmax, cs.x);
        hmax = std::max(hmax, cs.y);
        wsum += cs.x;
        hsum += cs.y;
      }
      const f32 gap = theme_.spacing * (w.children.empty() ? 0 : w.children.size() - 1);
      if (w.kind == WidgetKind::Row) content = Vec2(wsum + gap, hmax);
      else if (w.kind == WidgetKind::Column) content = Vec2(wmax, hsum + gap);
      else content = Vec2(wmax, hmax);
      content.x += theme_.padding * 2;
      content.y += theme_.padding * 2;
      break;
    }
  }
  Vec2 out = w.size;
  if (out.x <= 0.0f) out.x = content.x;
  if (out.y <= 0.0f) out.y = content.y;
  return out;
}

Vec2 Hud::AnchorBase(Anchor a, const Vec2& screen) const {
  switch (a) {
    case Anchor::TopLeft: return Vec2(0, 0);
    case Anchor::TopCenter: return Vec2(screen.x * 0.5f, 0);
    case Anchor::TopRight: return Vec2(screen.x, 0);
    case Anchor::CenterLeft: return Vec2(0, screen.y * 0.5f);
    case Anchor::Center: return Vec2(screen.x * 0.5f, screen.y * 0.5f);
    case Anchor::CenterRight: return Vec2(screen.x, screen.y * 0.5f);
    case Anchor::BottomLeft: return Vec2(0, screen.y);
    case Anchor::BottomCenter: return Vec2(screen.x * 0.5f, screen.y);
    case Anchor::BottomRight: return Vec2(screen.x, screen.y);
  }
  return Vec2(0, 0);
}

Vec2 Hud::AlignIn(Anchor a, const Vec2& base, const Vec2& size) const {
  // Horizontally: left anchors grow right, right anchors grow left, centred
  // anchors straddle the base point.
  f32 x = base.x;
  switch (a) {
    case Anchor::TopRight: case Anchor::CenterRight: case Anchor::BottomRight:
      x -= size.x; break;
    case Anchor::TopCenter: case Anchor::Center: case Anchor::BottomCenter:
      x -= size.x * 0.5f; break;
    default: break;
  }
  f32 y = base.y;
  switch (a) {
    case Anchor::CenterLeft: case Anchor::Center: case Anchor::CenterRight:
      y -= size.y * 0.5f; break;
    case Anchor::BottomLeft: case Anchor::BottomCenter: case Anchor::BottomRight:
      y -= size.y; break;
    default: break;
  }
  return Vec2(x, y);
}

void Hud::LayoutWidget(Widget& w, const Vec2& screen, const Vec2& origin) {
  Vec2 size = Measure(w);
  Vec2 pos;
  if (w.parent == 0) {
    // Root widget: positioned by its anchor.
    const Vec2 base = AnchorBase(w.anchor, screen) + w.offset;
    pos = AlignIn(w.anchor, base, size);
  } else {
    // Flow child: positioned by the parent at `origin`.
    pos = origin;
  }
  w.resolvedPos = pos;
  w.resolvedSize = size;
  w.layoutValid = true;

  if (w.children.empty()) return;
  const f32 pad = theme_.padding;
  Vec2 cursor(pos.x + pad, pos.y + pad);
  for (WidgetId c : w.children) {
    Widget* cw = Get(c);
    if (!cw) continue;
    Vec2 cs = Measure(*cw);
    LayoutWidget(*cw, screen, cursor);
    if (w.kind == WidgetKind::Row) {
      cursor.x += cs.x + theme_.spacing;
    } else if (w.kind == WidgetKind::Column) {
      cursor.y += cs.y + theme_.spacing;
    } else {
      // Panel: children are placed at the top-left inside the padding.
      cursor.x += cs.x + theme_.spacing;
    }
  }
}

void Hud::DrawWidget(const Widget& w, Renderer& r) {
  const f32 alpha = w.opacity.Value();
  if (!w.visible || alpha <= 0.01f) return;

  const f32 slide = w.slide.Value();
  const Vec2 pos(w.resolvedPos.x, w.resolvedPos.y + slide);
  const Vec2 size = w.resolvedSize;

  const Rgba color = theme_.Role(w.color).WithAlpha(
      (u8)std::clamp(alpha * 255.0f, 0.0f, 255.0f));
  const Rgba bg = theme_.Role(HudColorRole::Background).WithAlpha(
      (u8)std::clamp(alpha * 170.0f, 0.0f, 255.0f));

  switch (w.kind) {
    case WidgetKind::Label:
      r.DrawText(pos, w.text.c_str(), color, theme_.scale);
      ++stats_.widgetsDrawn;
      ++stats_.primitives;
      break;

    case WidgetKind::Bar: {
      Rect2D frame;
      frame.min = pos;
      frame.max = Vec2(pos.x + size.x, pos.y + size.y);
      frame.color = theme_.Role(HudColorRole::Muted).WithAlpha(
          (u8)std::clamp(alpha * 255.0f, 0.0f, 255.0f));
      frame.filled = false;
      frame.thickness = w.thickness;
      r.DrawRect(frame);

      f32 frac = 1.0f;
      if (w.maxValue > w.minValue) {
        frac = (w.value - w.minValue) / (w.maxValue - w.minValue);
      } else {
        frac = w.value;
      }
      frac = std::clamp(frac, 0.0f, 1.0f);
      if (frac > 0.0f) {
        Rect2D fill;
        fill.min = Vec2(pos.x + 1, pos.y + 1);
        fill.max = Vec2(pos.x + (size.x - 2) * frac + 1, pos.y + size.y - 1);
        fill.fill = color;
        fill.filled = true;
        r.DrawRect(fill);
        ++stats_.primitives;
      }
      ++stats_.widgetsDrawn;
      break;
    }

    case WidgetKind::Graph: {
      Rect2D frame;
      frame.min = pos;
      frame.max = Vec2(pos.x + size.x, pos.y + size.y);
      frame.color = theme_.Role(HudColorRole::Muted).WithAlpha(
          (u8)std::clamp(alpha * 200.0f, 0.0f, 255.0f));
      frame.filled = false;
      r.DrawRect(frame);

      if (w.samples.size() >= 2) {
        f32 lo = w.samples[0], hi = w.samples[0];
        for (f32 s : w.samples) {
          lo = std::min(lo, s);
          hi = std::max(hi, s);
        }
        if (hi - lo < 1e-4f) hi = lo + 1.0f;
        const f32 n = (f32)(w.samples.size() - 1);
        Vec2 prev;
        for (size_t i = 0; i < w.samples.size(); ++i) {
          const f32 tx = (f32)i / n;
          const f32 ty = (w.samples[i] - lo) / (hi - lo);
          Vec2 p(pos.x + tx * size.x, pos.y + (1.0f - ty) * size.y);
          if (i > 0) {
            r.DrawLine(prev, p, color, w.thickness);
            ++stats_.primitives;
          }
          prev = p;
        }
      }
      ++stats_.widgetsDrawn;
      break;
    }

    case WidgetKind::Panel:
    case WidgetKind::Row:
    case WidgetKind::Column: {
      if (w.background || w.kind == WidgetKind::Panel) {
        Rect2D rr;
        rr.min = pos;
        rr.max = Vec2(pos.x + size.x, pos.y + size.y);
        rr.fill = bg;
        rr.filled = true;
        r.DrawRect(rr);
        ++stats_.primitives;
      }
      ++stats_.widgetsDrawn;
      break;
    }
  }
}

void Hud::Present(Renderer& r, const Vec2& screen) {
  ++stats_.frames;
  stats_.layoutPasses = 0;
  stats_.widgetsDrawn = 0;
  stats_.widgetsCulled = 0;
  stats_.primitives = 0;

  // Layout only the roots whose geometry changed; a dirty subtree re-lays out
  // from its root. Undisturbed widgets keep last frame's resolved geometry.
  for (auto& kv : widgets_) {
    Widget& w = kv.second;
    if (w.parent != 0 || w.layoutValid) continue;
    LayoutWidget(w, screen, Vec2(0, 0));
    ++stats_.layoutPasses;
  }

  // Draw roots in a depth-first order so a parent paints under its children.
  std::vector<WidgetId> roots;
  for (const auto& kv : widgets_) {
    if (kv.second.parent == 0) roots.push_back(kv.first);
  }
  std::vector<WidgetId> stack(roots.rbegin(), roots.rend());
  while (!stack.empty()) {
    const WidgetId id = stack.back();
    stack.pop_back();
    const Widget* w = Get(id);
    if (!w) continue;
    // An invisible widget hides its whole subtree: a hidden container must not
    // leave its children painted on screen.
    if (!w->visible) continue;
    // An off-screen widget and its subtree cost nothing to skip: children are
    // laid out relative to the parent, so if the parent is off-screen they are
    // too (barring a slide tween, which the bounds below account for).
    if (!OnScreen(*w, screen)) {
      stats_.widgetsCulled += 1 + CountSubtree(*w);
      continue;
    }
    DrawWidget(*w, r);
    for (auto it = w->children.rbegin(); it != w->children.rend(); ++it) {
      stack.push_back(*it);
    }
  }
}

bool Hud::OnScreen(const Widget& w, const Vec2& screen) const {
  // The slide tween shifts a widget vertically, so include it in the box or a
  // widget sliding back into view would be culled while it is still visible.
  const f32 slide = w.slide.Value();
  const f32 minX = w.resolvedPos.x;
  const f32 minY = w.resolvedPos.y + slide;
  const f32 maxX = minX + w.resolvedSize.x;
  const f32 maxY = minY + w.resolvedSize.y;
  // A widget with no resolved size yet (never laid out) is kept: culling it
  // would hide a widget whose geometry is simply not known this frame.
  if (w.resolvedSize.x <= 0.0f || w.resolvedSize.y <= 0.0f) return true;
  // A small margin keeps a widget that is exactly on the edge from flickering.
  const f32 margin = 1.0f;
  return maxX >= -margin && maxY >= -margin &&
         minX <= screen.x + margin && minY <= screen.y + margin;
}

size_t Hud::CountSubtree(const Widget& w) const {
  size_t n = 0;
  std::vector<WidgetId> stack(w.children.begin(), w.children.end());
  while (!stack.empty()) {
    const WidgetId id = stack.back();
    stack.pop_back();
    const Widget* c = Get(id);
    if (!c) continue;
    ++n;
    for (WidgetId g : c->children) stack.push_back(g);
  }
  return n;
}

}  // namespace Berry
