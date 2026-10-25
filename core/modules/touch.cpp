// Client-side touch, gesture and on-screen button API. See berry/touch.h.
#include "berry/touch.h"

#include "berry/event.h"
#include "berry/render.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace Berry {

namespace {

constexpr f32 kPi = 3.14159265358979f;

/// The single channel every recognised gesture is published on. A script
/// filters by the gesture kind in the payload.
const char* kGestureChannel = "touchGesture";

bool FiniteVec(const Vec2& v) {
  return std::isfinite(v.x) && std::isfinite(v.y);
}

/// True when `p` is inside a button's rectangle (edges inclusive).
bool InsideRect(const TouchButton& b, const Vec2& p) {
  return p.x >= b.min.x && p.x <= b.max.x && p.y >= b.min.y && p.y <= b.max.y;
}

/// A printable, bounded identifier: letters, digits and `_`. The button's
/// intent name is matched against the intent table, so it shares that shape.
bool IsIdent(const std::string& s, size_t maxLen) {
  if (s.empty() || s.size() > maxLen) return false;
  for (unsigned char c : s) {
    if (!(std::isalnum(c) || c == '_')) return false;
  }
  return true;
}

}  // namespace

const char* GestureKindName(GestureKind k) {
  switch (k) {
    case GestureKind::None: return "none";
    case GestureKind::Tap: return "tap";
    case GestureKind::DoubleTap: return "double_tap";
    case GestureKind::LongPress: return "long_press";
    case GestureKind::Drag: return "drag";
    case GestureKind::Swipe: return "swipe";
    case GestureKind::Pinch: return "pinch";
    case GestureKind::Rotate: return "rotate";
  }
  return "none";
}

GestureKind GestureKindFromName(const std::string& name) {
  static const GestureKind kAll[] = {
      GestureKind::None,    GestureKind::Tap,    GestureKind::DoubleTap,
      GestureKind::LongPress, GestureKind::Drag, GestureKind::Swipe,
      GestureKind::Pinch,   GestureKind::Rotate};
  for (GestureKind k : kAll) {
    if (name == GestureKindName(k)) return k;
  }
  return GestureKind::None;
}

const char* SwipeDirName(SwipeDir d) {
  switch (d) {
    case SwipeDir::None: return "none";
    case SwipeDir::Up: return "up";
    case SwipeDir::Down: return "down";
    case SwipeDir::Left: return "left";
    case SwipeDir::Right: return "right";
  }
  return "none";
}

u32 GestureBit(GestureKind k) {
  switch (k) {
    case GestureKind::Tap: return (u32)GestureMask::Tap;
    case GestureKind::DoubleTap: return (u32)GestureMask::DoubleTap;
    case GestureKind::LongPress: return (u32)GestureMask::LongPress;
    case GestureKind::Drag: return (u32)GestureMask::Drag;
    case GestureKind::Swipe: return (u32)GestureMask::Swipe;
    case GestureKind::Pinch: return (u32)GestureMask::Pinch;
    case GestureKind::Rotate: return (u32)GestureMask::Rotate;
    case GestureKind::None: return 0;
  }
  return 0;
}

const char* TouchService::GestureChannel(GestureKind k) {
  return GestureBit(k) == 0 ? "" : kGestureChannel;
}

SwipeDir TouchService::SwipeDirectionOf(const Vec2& delta, f32 minDist) {
  if (!FiniteVec(delta) || !std::isfinite(minDist) || minDist < 0.0f) {
    return SwipeDir::None;
  }
  const f32 dx = delta.x;
  const f32 dy = delta.y;
  const f32 adx = std::fabs(dx);
  const f32 ady = std::fabs(dy);
  // Screen y grows downward, so a negative dy is an upward swipe.
  if (adx >= ady) {
    if (adx < minDist) return SwipeDir::None;
    return dx < 0.0f ? SwipeDir::Left : SwipeDir::Right;
  }
  if (ady < minDist) return SwipeDir::None;
  return dy < 0.0f ? SwipeDir::Up : SwipeDir::Down;
}

bool TouchService::ValidLabel(const std::string& label) {
  if (label.empty() || label.size() > kMaxLabelLen) return false;
  for (unsigned char c : label) {
    if (c < 0x20 || c == 0x7F) return false;
  }
  return true;
}

bool TouchService::ValidIntentName(const std::string& intent) {
  if (intent.empty()) return true;   // no intent is valid
  return IsIdent(intent, kMaxIntentLen);
}

i32 TouchService::SlotFor(u32 id) const {
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i].used && slots_[i].p.id == id) return (i32)i;
  }
  return -1;
}

i32 TouchService::PrimarySlot() const {
  // The primary finger is the one that has been down longest: the earliest
  // downMs, with the lowest id breaking a tie so the choice is deterministic.
  i32 best = -1;
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (!slots_[i].used) continue;
    if (best < 0) { best = (i32)i; continue; }
    const TouchPointer& a = slots_[i].p;
    const TouchPointer& b = slots_[(size_t)best].p;
    if (a.downMs < b.downMs || (a.downMs == b.downMs && a.id < b.id)) {
      best = (i32)i;
    }
  }
  return best;
}

void TouchService::PublishLocked(const TouchGesture& g) {
  last_ = g;
  const char* channel = GestureChannel(g.kind);
  if (!channel || !*channel) return;
  // The payload follows the bus convention: `data` points at a `const char*`.
  char buf[160];
  std::snprintf(buf, sizeof(buf), "%s,%s,%.2f,%.2f,%.2f,%u",
                GestureKindName(g.kind), SwipeDirName(g.dir), (double)g.at.x,
                (double)g.at.y, (double)g.magnitude, g.pointers);
  std::string payload(buf);
  const char* p = payload.c_str();
  EventBus::Instance().Emit(channel, &p, nullptr);
}

bool TouchService::OnPointerDown(u32 id, const Vec2& pos, u64 nowMs) {
  if (!FiniteVec(pos)) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  const i32 existing = SlotFor(id);
  if (existing >= 0) {
    // A repeated down for a live id updates its position rather than adding a
    // second slot for the same finger.
    slots_[(size_t)existing].p.pos = pos;
    return true;
  }
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i].used) continue;
    slots_[i].used = true;
    slots_[i].p = TouchPointer{};
    slots_[i].p.id = id;
    slots_[i].p.pos = pos;
    slots_[i].p.start = pos;
    slots_[i].p.down = true;
    slots_[i].p.downMs = nowMs;

    size_t down = 0;
    for (const auto& t : slots_) if (t.used) ++down;
    if (down == 1) {
      // This is the only finger, so it is the primary press: re-arm the
      // long-press, drop any stale pinch baseline, and hit-test the buttons.
      // The hit-test is event-driven (not left to the next Update) so a press
      // and release that both land between two frames still counts.
      longFired_ = false;
      havePinchBase_ = false;
      primaryOnButton_ = false;
      for (auto& b : buttons_) {
        if (b.enabled && InsideRect(b, pos)) {
          b.held = true;
          b.allowed = true;   // resolved against the policy in Update
          ++b.presses;
          primaryOnButton_ = true;
        }
      }
    } else if (down == 2) {
      // The second finger lands: capture the two-finger baseline.
      std::vector<Vec2> pts;
      for (const auto& t : slots_) if (t.used) pts.push_back(t.p.pos);
      const Vec2 d = pts[1] - pts[0];
      pinchBaseDist_ = d.Length();
      pinchBaseAngle_ = std::atan2(d.y, d.x);
      lastPinchScale_ = 1.0f;
      lastRotateDeg_ = 0.0f;
      havePinchBase_ = pinchBaseDist_ > 0.0f;
    }
    return true;
  }
  return false;   // table full
}

void TouchService::OnPointerMove(u32 id, const Vec2& pos, u64 nowMs) {
  (void)nowMs;
  if (!FiniteVec(pos)) return;
  std::lock_guard<std::mutex> lk(mutex_);
  const i32 slot = SlotFor(id);
  if (slot < 0) return;
  slots_[(size_t)slot].p.pos = pos;
}

void TouchService::OnPointerUp(u32 id, const Vec2& pos, u64 nowMs) {
  std::lock_guard<std::mutex> lk(mutex_);
  const i32 slot = SlotFor(id);
  if (slot < 0) return;
  TouchPointer p = slots_[(size_t)slot].p;
  if (FiniteVec(pos)) p.pos = pos;
  const bool wasPrimary = (PrimarySlot() == slot);
  slots_[(size_t)slot] = Slot{};
  p.down = false;

  // Release every button the primary finger was holding, before the gesture is
  // resolved, so a tap on a button does not also drive a gesture. The release
  // toggles the button only when the finger comes up still inside it.
  if (wasPrimary) {
    for (auto& b : buttons_) {
      if (!b.held) continue;
      b.held = false;
      ++b.releases;
      if (b.enabled && InsideRect(b, p.pos)) b.toggled = !b.toggled;
    }
  }

  // Only the primary finger resolves a single-finger gesture; a secondary
  // lift just shrinks the pointer count (and ends any pinch).
  size_t down = 0;
  for (const auto& t : slots_) if (t.used) ++down;
  if (down < 2) havePinchBase_ = false;
  if (!wasPrimary) return;
  // A press that began on a button belongs to the button, not the gesture
  // recogniser: it was consumed as a press/release, so it must not also read as
  // a tap or swipe.
  if (primaryOnButton_) {
    primaryOnButton_ = false;
    return;
  }

  const Vec2 delta = p.pos - p.start;
  const f32 dist = delta.Length();
  const u64 dur = nowMs > p.downMs ? nowMs - p.downMs : 0;

  TouchGesture g;
  g.at = p.pos;
  g.delta = delta;
  g.pointers = 1;
  g.atMs = nowMs;

  const bool canSwipe = (mask_ & (u32)GestureMask::Swipe) != 0;
  const bool canTap = (mask_ & (u32)GestureMask::Tap) != 0;
  const bool canDouble = (mask_ & (u32)GestureMask::DoubleTap) != 0;

  if (canSwipe && dist >= cfg_.swipeMinDist && (f32)dur <= cfg_.swipeMaxMs) {
    g.kind = GestureKind::Swipe;
    g.dir = SwipeDirectionOf(delta, cfg_.swipeMinDist);
    g.magnitude = dist;
    PublishLocked(g);
    haveLastTap_ = false;   // a swipe is not the first half of a double tap
    return;
  }

  if (dist <= cfg_.tapMaxMove && (f32)dur <= cfg_.tapMaxMs) {
    // A tap: it may complete a double tap when a recent tap is close by.
    if (canDouble && haveLastTap_ && (f32)(nowMs - lastTapMs_) <= cfg_.doubleTapMaxMs &&
        (p.pos - lastTapPos_).Length() <= cfg_.tapMaxMove * 2.0f) {
      g.kind = GestureKind::DoubleTap;
      g.magnitude = dist;
      PublishLocked(g);
      haveLastTap_ = false;   // a double tap consumes the pair
      return;
    }
    if (canTap) {
      g.kind = GestureKind::Tap;
      g.magnitude = dist;
      PublishLocked(g);
    }
    // Remember this tap so a following one can be a double tap. Recorded even
    // when Tap is disabled, so enabling DoubleTap alone still works.
    lastTapMs_ = nowMs;
    lastTapPos_ = p.pos;
    haveLastTap_ = true;
    return;
  }

  // A slow, long press that has already fired its LongPress is not also a drag.
  if ((mask_ & (u32)GestureMask::Drag) != 0 && dist > cfg_.tapMaxMove) {
    g.kind = GestureKind::Drag;
    g.magnitude = dist;
    PublishLocked(g);
  }
}

void TouchService::ClearPointers() {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto& s : slots_) s = Slot{};
  havePinchBase_ = false;
  longFired_ = false;
  primaryOnButton_ = false;
}

void TouchService::Update(u64 nowMs, const IntentValidator* intents) {
  std::lock_guard<std::mutex> lk(mutex_);

  // Long press: the primary finger held past the threshold without moving. A
  // press that began on a button is the button's, so it never long-presses.
  const i32 primary = PrimarySlot();
  if (primary >= 0 && !primaryOnButton_ &&
      (mask_ & (u32)GestureMask::LongPress) != 0 && !longFired_) {
    const TouchPointer& p = slots_[(size_t)primary].p;
    const Vec2 delta = p.pos - p.start;
    if (delta.Length() <= cfg_.tapMaxMove &&
        (f32)(nowMs - p.downMs) >= cfg_.longPressMs) {
      TouchGesture g;
      g.kind = GestureKind::LongPress;
      g.at = p.pos;
      g.delta = delta;
      g.magnitude = delta.Length();
      g.pointers = 1;
      g.atMs = nowMs;
      PublishLocked(g);
      longFired_ = true;
    }
  }

  // Pinch / rotate: two live fingers whose separation or angle changed.
  size_t down = 0;
  for (const auto& t : slots_) if (t.used) ++down;
  if (down == 2 && havePinchBase_ && pinchBaseDist_ > 0.0f) {
    std::vector<Vec2> pts;
    for (const auto& t : slots_) if (t.used) pts.push_back(t.p.pos);
    const Vec2 d = pts[1] - pts[0];
    const f32 dist = d.Length();
    const f32 angle = std::atan2(d.y, d.x);
    const f32 scale = dist / pinchBaseDist_;
    f32 ddeg = (angle - pinchBaseAngle_) * 180.0f / kPi;
    while (ddeg > 180.0f) ddeg -= 360.0f;
    while (ddeg < -180.0f) ddeg += 360.0f;
    if ((mask_ & (u32)GestureMask::Pinch) != 0 &&
        std::fabs(scale - 1.0f) >= cfg_.pinchMinScale &&
        std::fabs(scale - lastPinchScale_) > 0.0f) {
      TouchGesture g;
      g.kind = GestureKind::Pinch;
      g.at = (pts[0] + pts[1]) * 0.5f;
      g.magnitude = scale;
      g.pointers = 2;
      g.atMs = nowMs;
      PublishLocked(g);
      lastPinchScale_ = scale;
    }
    if ((mask_ & (u32)GestureMask::Rotate) != 0 &&
        std::fabs(ddeg - lastRotateDeg_) > 0.0f) {
      TouchGesture g;
      g.kind = GestureKind::Rotate;
      g.at = (pts[0] + pts[1]) * 0.5f;
      g.magnitude = ddeg;
      g.pointers = 2;
      g.atMs = nowMs;
      PublishLocked(g);
      lastRotateDeg_ = ddeg;
    }
  }

  // Buttons: resolve each held button's intent against this session's policy.
  // The press/release edge itself is handled in OnPointerDown/Up, so a fast
  // tap that never coincides with a frame still registers; here we only decide
  // whether the button's intent is allowed, which the draw and the script read.
  for (auto& b : buttons_) {
    if (!b.held) continue;
    b.allowed = true;
    if (!b.intent.empty()) {
      const IntentKind ik = IntentKindFromName(b.intent);
      b.allowed = intents ? (intents->Validate(ik) == IntentResult::Ok)
                          : (ik != IntentKind::Unknown &&
                             ClassifyIntent(ik) == IntentClass::ClientSide);
    }
  }
}

void TouchService::Draw(Renderer& r) const {
  std::vector<TouchButton> buttons;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    buttons = buttons_;
  }
  for (const auto& b : buttons) {
    const bool active = b.held || b.toggled;
    Rgba fill = active ? Rgba(80, 140, 255, 210) : Rgba(24, 26, 32, 170);
    if (!b.enabled) fill = Rgba(30, 30, 34, 140);
    else if (!b.allowed) fill = Rgba(120, 60, 60, 190);
    Rect2D rect;
    rect.min = b.min;
    rect.max = b.max;
    rect.filled = true;
    rect.fill = fill;
    rect.color = Rgba(200, 200, 210, 200);
    rect.thickness = 1.5f;
    r.DrawRect(rect);
    const Vec2 center((b.min.x + b.max.x) * 0.5f, (b.min.y + b.max.y) * 0.5f);
    Rgba textColor = b.enabled ? Rgba(235, 235, 240) : Rgba(120, 120, 130);
    r.DrawTextCentered(center, b.label.c_str(), textColor, 1.0f);
  }
}

bool TouchService::SetGestureEnabled(GestureKind kind, bool enabled) {
  const u32 bit = GestureBit(kind);
  if (bit == 0) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  if (enabled) mask_ |= bit;
  else mask_ &= ~bit;
  return true;
}

bool TouchService::gestureEnabled(GestureKind kind) const {
  const u32 bit = GestureBit(kind);
  if (bit == 0) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  return (mask_ & bit) != 0;
}

u32 TouchService::gestureMask() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return mask_;
}

TouchConfig TouchService::config() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return cfg_;
}

bool TouchService::SetTapMaxMove(f32 px) {
  if (!std::isfinite(px) || px <= 0.0f || px > kMaxButtonSpan) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  cfg_.tapMaxMove = px;
  return true;
}

bool TouchService::SetTapMaxMs(f32 ms) {
  if (!std::isfinite(ms) || ms <= 0.0f) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  cfg_.tapMaxMs = ms;
  return true;
}

bool TouchService::SetLongPressMs(f32 ms) {
  if (!std::isfinite(ms) || ms <= 0.0f) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  cfg_.longPressMs = ms;
  return true;
}

bool TouchService::SetDoubleTapMaxMs(f32 ms) {
  if (!std::isfinite(ms) || ms <= 0.0f) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  cfg_.doubleTapMaxMs = ms;
  return true;
}

bool TouchService::SetSwipeMinDist(f32 px) {
  if (!std::isfinite(px) || px <= 0.0f || px > kMaxButtonSpan) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  cfg_.swipeMinDist = px;
  return true;
}

bool TouchService::SetSwipeMaxMs(f32 ms) {
  if (!std::isfinite(ms) || ms <= 0.0f) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  cfg_.swipeMaxMs = ms;
  return true;
}

bool TouchService::SetPinchMinScale(f32 scale) {
  if (!std::isfinite(scale) || scale <= 0.0f || scale > 1.0f) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  cfg_.pinchMinScale = scale;
  return true;
}

TouchGesture TouchService::lastGesture() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return last_;
}

bool TouchService::AddButton(const std::string& label, const Vec2& min,
                             const Vec2& max) {
  if (!ValidLabel(label)) return false;
  if (!FiniteVec(min) || !FiniteVec(max)) return false;
  const Vec2 span = max - min;
  if (span.x <= 0.0f || span.y <= 0.0f) return false;
  if (span.x > kMaxButtonSpan || span.y > kMaxButtonSpan) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  if (buttons_.size() >= kMaxButtons) return false;
  TouchButton b;
  b.label = label;
  b.min = min;
  b.max = max;
  buttons_.push_back(std::move(b));
  return true;
}

bool TouchService::RemoveButton(size_t index) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (index >= buttons_.size()) return false;
  buttons_.erase(buttons_.begin() + (long)index);
  return true;
}

void TouchService::ClearButtons() {
  std::lock_guard<std::mutex> lk(mutex_);
  buttons_.clear();
}

size_t TouchService::buttonCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return buttons_.size();
}

std::string TouchService::buttonLabel(size_t index) const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (index >= buttons_.size()) return std::string();
  return buttons_[index].label;
}

bool TouchService::buttonEnabled(size_t index) const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (index >= buttons_.size()) return false;
  return buttons_[index].enabled;
}

bool TouchService::SetButtonEnabled(size_t index, bool enabled) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (index >= buttons_.size()) return false;
  buttons_[index].enabled = enabled;
  if (!enabled) buttons_[index].held = false;
  return true;
}

bool TouchService::SetButtonIntent(size_t index, const std::string& intent) {
  if (!ValidIntentName(intent)) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  if (index >= buttons_.size()) return false;
  buttons_[index].intent = intent;
  return true;
}

std::string TouchService::buttonIntent(size_t index) const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (index >= buttons_.size()) return std::string();
  return buttons_[index].intent;
}

TouchButton TouchService::buttonState(size_t index) const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (index >= buttons_.size()) return TouchButton{};
  return buttons_[index];
}

std::vector<TouchPointer> TouchService::pointers() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<TouchPointer> out;
  // Oldest first, so the primary finger leads.
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i].used) out.push_back(slots_[i].p);
  }
  std::sort(out.begin(), out.end(), [](const TouchPointer& a, const TouchPointer& b) {
    if (a.downMs != b.downMs) return a.downMs < b.downMs;
    return a.id < b.id;
  });
  return out;
}

TouchState TouchService::state() const {
  std::lock_guard<std::mutex> lk(mutex_);
  TouchState s;
  for (const auto& t : slots_) if (t.used) ++s.pointers;
  s.gesture = last_;
  s.buttons = buttons_.size();
  return s;
}

}  // namespace Berry
