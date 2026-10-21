// Client input observation. See include/berry/input.h.
#include "berry/input.h"

#include <algorithm>
#include <cctype>

namespace Berry {
namespace {

std::string Lower(const char* s) {
  std::string out;
  if (!s) return out;
  for (; *s; ++s) out.push_back((char)std::tolower((unsigned char)*s));
  return out;
}

bool IsMovementKey(const std::string& k) {
  return k == "w" || k == "a" || k == "s" || k == "d";
}

bool IsTrackedKey(const std::string& k) {
  return IsMovementKey(k) || k == "space" || k == "shift" || k == "ctrl";
}

}  // namespace

void InputObserver::OnKey(const char* key, bool down) {
  const std::string k = Lower(key);
  // Fail closed: an unrecognised key is ignored rather than stored, so the
  // state cannot be polluted by a stray name.
  if (!IsTrackedKey(k)) return;

  if (k == "w") state_.forward = down;
  else if (k == "s") state_.back = down;
  else if (k == "a") state_.left = down;
  else if (k == "d") state_.right = down;
  else if (k == "space") state_.jump = down;
  else if (k == "shift") state_.sneak = down;
  else if (k == "ctrl") state_.sprint = down;

  // Maintain held-since timestamps for the tracked keys.
  for (auto& h : held_) {
    if (h.key == k) {
      if (down) {
        if (h.sinceMs == 0) h.sinceMs = nowMs_ ? nowMs_ : 1;
      } else {
        h.sinceMs = 0;
      }
      return;
    }
  }
  if (down) {
    for (auto& h : held_) {
      if (h.key.empty()) {
        h.key = k;
        h.sinceMs = nowMs_ ? nowMs_ : 1;
        return;
      }
    }
  }
}

void InputObserver::OnMouse(MouseButton button, bool down) {
  const size_t idx = (size_t)button;
  if (idx >= state_.mouse.size()) return;
  state_.mouse[idx] = down;
  if (button == MouseButton::Left) state_.attack = down;
  if (button == MouseButton::Right) state_.use = down;

  if (down) {
    ClickWindow& cw = clicks_[idx];
    cw.stamps.push_back(nowMs_);
    cw.total++;
  }
}

void InputObserver::Tick(u64 nowMs) {
  nowMs_ = nowMs;
  const u64 cutoff = nowMs > kWindowMs ? nowMs - kWindowMs : 0;
  for (auto& cw : clicks_) {
    while (!cw.stamps.empty() && cw.stamps.front() < cutoff) {
      cw.stamps.pop_front();
    }
  }
}

f64 InputObserver::clicksPerSecond(MouseButton button) const {
  const size_t idx = (size_t)button;
  if (idx >= clicks_.size()) return 0;
  const ClickWindow& cw = clicks_[idx];
  const u64 cutoff = nowMs_ > kWindowMs ? nowMs_ - kWindowMs : 0;
  size_t n = 0;
  for (u64 stamp : cw.stamps) {
    if (stamp >= cutoff) ++n;
  }
  return (f64)n * 1000.0 / (f64)kWindowMs;
}

u64 InputObserver::totalClicks(MouseButton button) const {
  const size_t idx = (size_t)button;
  if (idx >= clicks_.size()) return 0;
  return clicks_[idx].total;
}

u64 InputObserver::heldMs(const char* key, u64 nowMs) const {
  const std::string k = Lower(key);
  for (const auto& h : held_) {
    if (h.key == k) {
      if (h.sinceMs == 0 || nowMs < h.sinceMs) return 0;
      return nowMs - h.sinceMs;
    }
  }
  return 0;
}

void InputObserver::Reset() {
  state_ = InputState{};
  for (auto& cw : clicks_) cw = ClickWindow{};
  for (auto& h : held_) h = Held{};
  nowMs_ = 0;
}

}  // namespace Berry
