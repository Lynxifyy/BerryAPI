// Client input observation. See include/berry/input.h.
#include "berry/input.h"

#include <algorithm>
#include <cctype>
#include <cmath>

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

/// Keys a UI acts on once per press rather than while held. They carry no
/// movement state; they only produce a press edge. Only keys with a consumer
/// belong here: an edge that is never taken would fill the pending list and
/// start dropping the edges a UI actually reads.
bool IsOneShotKey(const std::string& k) {
  return k == "backspace" || k == "enter";
}

bool IsTrackedKey(const std::string& k) {
  return IsMovementKey(k) || k == "space" || k == "shift" || k == "ctrl" ||
         IsOneShotKey(k);
}

}  // namespace

void InputObserver::OnKey(const char* key, bool down) {
  const std::string k = Lower(key);
  // Fail closed: an unrecognised key is ignored rather than stored, so the
  // state cannot be polluted by a stray name.
  if (!IsTrackedKey(k)) return;

  // A one-shot key records a press edge and nothing else.
  if (IsOneShotKey(k)) {
    if (down && pressedCount_ < pressed_.size()) {
      pressed_[pressedCount_++] = k;
    }
    return;
  }

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

void InputObserver::OnPointerMove(f32 x, f32 y) {
  // A non-finite coordinate would poison every hit-test that follows, so it is
  // refused rather than stored.
  if (!std::isfinite(x) || !std::isfinite(y)) return;
  state_.pointer = Vec2(x, y);
}

void InputObserver::OnWheel(f32 notches) {
  if (!std::isfinite(notches)) return;
  state_.wheel += notches;
}

void InputObserver::OnTextInput(const char* utf8) {
  if (!utf8) return;
  // Control bytes are rejected: a text field wants printable characters, and a
  // stray newline or escape would otherwise land in the buffer.
  for (const char* p = utf8; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    if (c >= 0x20 && c != 0x7f) typed_.push_back((char)c);
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

f32 InputObserver::ConsumeWheel() {
  const f32 w = state_.wheel;
  state_.wheel = 0.0f;
  return w;
}

std::string InputObserver::ConsumeText() {
  std::string out = std::move(typed_);
  typed_.clear();
  return out;
}

bool InputObserver::ConsumeKeyPress(const char* key) {
  const std::string k = Lower(key);
  for (size_t i = 0; i < pressedCount_; ++i) {
    if (pressed_[i] == k) {
      // Compact the array so the press is seen exactly once.
      for (size_t j = i + 1; j < pressedCount_; ++j) {
        pressed_[j - 1] = pressed_[j];
      }
      --pressedCount_;
      pressed_[pressedCount_].clear();
      return true;
    }
  }
  return false;
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
  typed_.clear();
  for (auto& p : pressed_) p.clear();
  pressedCount_ = 0;
  nowMs_ = 0;
}

}  // namespace Berry
