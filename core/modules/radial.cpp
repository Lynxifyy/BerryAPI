#include "berry/radial.h"

#include "berry/event.h"
#include "berry/render.h"

#include <algorithm>
#include <cmath>

namespace Berry {

namespace {
constexpr f32 kPi = 3.14159265358979f;

/// Where a sector's mid-angle sits, in radians. Sector 0 is centred on
/// straight up (-Y in screen space) and the sectors run clockwise, which is the
/// layout a player expects from a radial menu.
f32 SectorAngle(size_t index, size_t count) {
  const f32 step = 2.0f * kPi / (f32)count;
  // Screen y grows downward, so "up" is -pi/2 in atan2 terms.
  return -kPi * 0.5f + step * ((f32)index + 0.5f);
}
}  // namespace

bool RadialMenu::ValidLabel(const std::string& label) {
  if (label.empty() || label.size() > kMaxLabelLen) return false;
  for (unsigned char c : label) {
    if (c < 0x20 || c == 0x7F) return false;
  }
  return true;
}

bool RadialMenu::AddItem(const std::string& label, bool enabled) {
  if (!ValidLabel(label)) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  if (items_.size() >= kMaxItems) return false;
  items_.push_back(RadialItem{label, enabled});
  return true;
}

bool RadialMenu::RemoveItem(size_t index) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (index >= items_.size()) return false;
  items_.erase(items_.begin() + (long)index);
  // A hover or a selection that pointed at or past the removed slot is stale.
  if (hovered_ >= (i32)items_.size()) hovered_ = -1;
  if (selected_ >= (i32)items_.size()) selected_ = -1;
  return true;
}

void RadialMenu::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  items_.clear();
  open_ = false;
  progress_ = 0.0f;
  hovered_ = -1;
  selected_ = -1;
}

size_t RadialMenu::itemCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return items_.size();
}

std::string RadialMenu::itemLabel(size_t index) const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (index >= items_.size()) return std::string();
  return items_[index].label;
}

bool RadialMenu::itemEnabled(size_t index) const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (index >= items_.size()) return false;
  return items_[index].enabled;
}

bool RadialMenu::SetItemEnabled(size_t index, bool enabled) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (index >= items_.size()) return false;
  items_[index].enabled = enabled;
  return true;
}

RadialConfig RadialMenu::config() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return cfg_;
}

bool RadialMenu::SetRadius(f32 radius) {
  if (!std::isfinite(radius) || radius <= 0.0f) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  if (radius <= cfg_.innerRadius) return false;
  cfg_.radius = radius;
  return true;
}

bool RadialMenu::SetInnerRadius(f32 radius) {
  if (!std::isfinite(radius) || radius < 0.0f) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  if (radius >= cfg_.radius) return false;
  cfg_.innerRadius = radius;
  return true;
}

bool RadialMenu::SetOpenMs(f32 ms) {
  if (!std::isfinite(ms) || ms <= 0.0f) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  cfg_.openMs = ms;
  return true;
}

bool RadialMenu::Open() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (open_) return false;
  if (items_.empty()) return false;
  open_ = true;
  hovered_ = -1;
  selected_ = -1;
  wasHeld_ = false;
  return true;
}

void RadialMenu::Close() {
  std::lock_guard<std::mutex> lk(mutex_);
  open_ = false;
  hovered_ = -1;
  selected_ = -1;
  wasHeld_ = false;
}

bool RadialMenu::open() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return open_;
}

i32 RadialMenu::SectorFor(const Vec2& pointer, const Vec2& center, f32 innerRadius,
                          f32 outerRadius, size_t count) {
  if (count == 0) return -1;
  if (!std::isfinite(innerRadius) || !std::isfinite(outerRadius)) return -1;
  const Vec2 d = pointer - center;
  const f32 dist = d.Length();
  if (dist < innerRadius || dist > outerRadius) return -1;
  // atan2 gives (-pi, pi] with 0 along +X. Sector 0 is centred straight up, and
  // the sectors run clockwise; normalise the angle into [0, 2pi) measured from
  // that first sector's start edge.
  f32 ang = std::atan2(d.y, d.x);
  const f32 step = 2.0f * kPi / (f32)count;
  // Offset so that sector boundaries fall on the half-step lines.
  f32 rel = ang - (-kPi * 0.5f - step * 0.5f);
  while (rel < 0.0f) rel += 2.0f * kPi;
  while (rel >= 2.0f * kPi) rel -= 2.0f * kPi;
  i32 idx = (i32)(rel / step);
  if (idx < 0) idx = 0;
  if (idx >= (i32)count) idx = (i32)count - 1;
  return idx;
}

void RadialMenu::Update(f32 dtSeconds, const Vec2& center, const Vec2& pointer,
                        bool held) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!std::isfinite(dtSeconds) || dtSeconds < 0.0f) dtSeconds = 0.0f;

  // Advance the open/close animation toward the target.
  const f32 durS = cfg_.openMs / 1000.0f;
  const f32 target = open_ ? 1.0f : 0.0f;
  if (durS > 0.0f) {
    const f32 step = dtSeconds / durS;
    if (progress_ < target) progress_ = std::min(target, progress_ + step);
    else if (progress_ > target) progress_ = std::max(target, progress_ - step);
  } else {
    progress_ = target;
  }

  selected_ = -1;
  if (!open_) {
    hovered_ = -1;
    wasHeld_ = held;
    return;
  }

  // The sectors only exist once the menu is meaningfully open; before then the
  // pointer is over nothing, so a fast release cannot select an invisible item.
  if (progress_ < 0.5f) {
    hovered_ = -1;
    wasHeld_ = held;
    return;
  }

  const i32 hit = SectorFor(pointer, center, cfg_.innerRadius, cfg_.radius,
                            items_.size());
  hovered_ = (hit >= 0 && (size_t)hit < items_.size() && items_[(size_t)hit].enabled)
                 ? hit
                 : -1;

  // Commit on the held -> released edge over an enabled sector, and publish it
  // once. The menu stays open until the script closes it, so the script owns
  // the lifetime and a commit is a single event, not a state. The payload
  // follows the bus convention: `data` points at a `const char*` the listener
  // reads as `*(const char**)e.data`, which is what `Events.once` decodes.
  if (wasHeld_ && !held && hovered_ >= 0) {
    selected_ = hovered_;
    const char* label = items_[(size_t)selected_].label.c_str();
    EventBus::Instance().Emit("radialSelected", &label, nullptr);
  }
  wasHeld_ = held;
}

void RadialMenu::Draw(Renderer& r, const Vec2& center) const {
  std::vector<RadialItem> items;
  RadialConfig cfg;
  f32 progress;
  i32 hovered;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!open_ || items_.empty()) return;
    if (progress_ <= 0.0f) return;
    items = items_;
    cfg = cfg_;
    progress = progress_;
    hovered = hovered_;
  }

  const f32 outer = cfg.radius * progress;
  const f32 inner = cfg.innerRadius * progress;
  const size_t n = items.size();
  const f32 step = 2.0f * kPi / (f32)n;

  const Rgba bg(18, 18, 22, (u8)(200 * progress));
  const Rgba sectorFill(40, 44, 56, (u8)(190 * progress));
  const Rgba hoverFill(80, 140, 255, (u8)(220 * progress));
  const Rgba disabledFill(30, 30, 34, (u8)(150 * progress));
  const Rgba labelColor(235, 235, 240, (u8)(255 * progress));
  const Rgba disabledLabel(120, 120, 130, (u8)(200 * progress));

  // The centre hub, so the dead zone reads as the menu's anchor.
  r.DrawCircle(center, std::max(inner, 2.0f), bg, 48);

  for (size_t i = 0; i < n; ++i) {
    const bool on = (i32)i == hovered;
    const Rgba fill = !items[i].enabled ? disabledFill : (on ? hoverFill : sectorFill);
    // The batch has no arbitrary-triangle or arc primitive, so a wedge is filled
    // with a run of radial spokes: each spoke joins the inner arc to the outer
    // arc at one angle, and its thickness covers the gap to the next spoke, so
    // adjacent spokes overlap and the sector reads solid. The step count scales
    // with the sector's angular width so a wide sector is not faceted.
    const int steps = std::max(3, (int)(24.0f / (f32)n) + 2);
    const f32 a0 = SectorAngle(i, n) - step * 0.5f;
    const f32 a1 = SectorAngle(i, n) + step * 0.5f;
    const f32 spoke = std::max(2.0f, outer * step / (f32)steps * 1.3f);
    for (int s = 0; s <= steps; ++s) {
      const f32 a = a0 + (a1 - a0) * (f32)s / (f32)steps;
      const Vec2 o(center.x + std::cos(a) * outer, center.y + std::sin(a) * outer);
      const Vec2 in(center.x + std::cos(a) * inner, center.y + std::sin(a) * inner);
      r.DrawLine(in, o, fill, spoke);
    }

    // The label, placed at the sector's mid-angle, a little past the inner arc.
    const f32 mid = SectorAngle(i, n);
    const f32 lr = (inner + outer) * 0.5f;
    const Vec2 lp(center.x + std::cos(mid) * lr, center.y + std::sin(mid) * lr);
    r.DrawTextCentered(lp, items[i].label.c_str(),
                       items[i].enabled ? labelColor : disabledLabel,
                       cfg.labelScale * progress);
  }
}

RadialState RadialMenu::state() const {
  std::lock_guard<std::mutex> lk(mutex_);
  RadialState s;
  s.open = open_;
  s.hovered = hovered_;
  s.selected = selected_;
  s.progress = progress_;
  s.items = items_.size();
  return s;
}

}  // namespace Berry
