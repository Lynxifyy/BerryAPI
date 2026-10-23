// Fight-review overlay. See include/berry/replay_review.h for the model.
#include "berry/replay_review.h"

#include "berry/render.h"

#include <algorithm>
#include <cmath>

namespace Berry {

namespace {
constexpr f32 kTicksPerSecond = 20.0f;

std::string Label(const EntitySnapshot& s) {
  if (!s.name.empty()) return s.name;
  const char* t = EntityTypeName(s.type);
  return t ? std::string(t) : std::string("entity");
}
}  // namespace

ReplayReviewStyle ReplayReview::style() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return style_;
}

void ReplayReview::SetStyle(const ReplayReviewStyle& s) {
  std::lock_guard<std::mutex> lk(mutex_);
  style_ = s;
}

bool ReplayReview::active() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return active_;
}

void ReplayReview::SetActive(bool on) {
  std::lock_guard<std::mutex> lk(mutex_);
  active_ = on;
}

void ReplayReview::SetLocalPlayer(EntityId id) {
  std::lock_guard<std::mutex> lk(mutex_);
  localPlayer_ = id;
}

EntityId ReplayReview::localPlayer() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return localPlayer_;
}

bool ReplayReview::Play(ReplayWorld* world) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!world || !world->loaded()) return false;
  world_ = world;
  accumTicks_ = 0.0f;
  world_->Seek(world_->firstTick());
  playing_ = true;
  return true;
}

void ReplayReview::Pause() {
  std::lock_guard<std::mutex> lk(mutex_);
  playing_ = false;
}

bool ReplayReview::playing() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return playing_;
}

void ReplayReview::Stop() {
  std::lock_guard<std::mutex> lk(mutex_);
  playing_ = false;
  world_ = nullptr;
  accumTicks_ = 0.0f;
}

u64 ReplayReview::Advance(f32 dtSeconds) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!world_ || !playing_) return world_ ? world_->tick() : 0;
  if (!std::isfinite(dtSeconds) || dtSeconds <= 0.0f) return world_->tick();
  // Clamp a stall (a breakpoint, a pause) so the replay does not jump.
  const f32 dt = std::min(dtSeconds, 0.25f);
  accumTicks_ += dt * kTicksPerSecond * speed_;
  const u64 step = (u64)accumTicks_;
  if (step == 0) return world_->tick();
  accumTicks_ -= (f32)step;
  u64 next = world_->tick() + step;
  if (next >= world_->lastTick()) {
    next = world_->lastTick();
    playing_ = false;   // hold the final frame
  }
  world_->Seek(next);
  return next;
}

bool ReplayReview::Seek(u64 tick) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!world_) return false;
  accumTicks_ = 0.0f;
  return world_->Seek(tick);
}

u64 ReplayReview::tick() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return world_ ? world_->tick() : 0;
}

bool ReplayReview::activeWorld() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return world_ != nullptr && world_->loaded();
}

f32 ReplayReview::speed() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return speed_;
}

bool ReplayReview::SetSpeed(f32 speed) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!std::isfinite(speed) || speed <= 0.0f || speed > kMaxSpeed) return false;
  speed_ = speed;
  return true;
}

size_t ReplayReview::Present(const CameraState& cam, Renderer& r,
                             const Vec2& screen) {
  std::lock_guard<std::mutex> lk(mutex_);
  lastItems_.clear();
  if (!active_ || !world_ || !world_->loaded()) return 0;

  Vec2 localScreen;
  bool haveLocal = false;
  if (style_.lineToLocal && localPlayer_.raw != 0) {
    EntitySnapshot me;
    if (world_->GetEntity(localPlayer_, &me)) {
      haveLocal = cam.WorldToScreen(me.position, &localScreen);
    }
  }

  size_t drawn = 0;
  for (const auto& [raw, snap] : world_->entities()) {
    if (drawn >= kMaxDrawn) break;
    // Project the entity's hitbox corners; a point that will not project is
    // skipped, so a behind-camera entity draws nothing.
    const Aabb box = EntityHitbox(snap);
    const Vec3 corners[8] = {
        Vec3(box.min.x, box.min.y, box.min.z),
        Vec3(box.max.x, box.min.y, box.min.z),
        Vec3(box.min.x, box.max.y, box.min.z),
        Vec3(box.max.x, box.max.y, box.min.z),
        Vec3(box.min.x, box.min.y, box.max.z),
        Vec3(box.max.x, box.min.y, box.max.z),
        Vec3(box.min.x, box.max.y, box.max.z),
        Vec3(box.max.x, box.max.y, box.max.z),
    };
    Vec2 min(1e9f, 1e9f), max(-1e9f, -1e9f);
    bool any = false;
    for (const Vec3& c : corners) {
      Vec2 p;
      if (cam.WorldToScreen(c, &p)) {
        min.x = std::min(min.x, p.x);
        min.y = std::min(min.y, p.y);
        max.x = std::max(max.x, p.x);
        max.y = std::max(max.y, p.y);
        any = true;
      }
    }
    if (!any) continue;

    const bool isLocal = localPlayer_.raw != 0 && raw == localPlayer_.raw;
    const Rgba color = isLocal ? style_.localColor : style_.boxColor;

    if (style_.boxes) {
      Rect2D rect;
      rect.min = min;
      rect.max = max;
      rect.color = color;
      rect.thickness = style_.lineThickness;
      r.DrawRect(rect);
    }
    if (style_.names) {
      const std::string label = Label(snap);
      r.DrawText(Vec2(min.x, min.y - 12.0f), label.c_str(), color,
                 style_.nameScale);
    }
    if (haveLocal && !isLocal) {
      r.DrawLine(localScreen, Vec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f),
                 color, style_.lineThickness);
    }

    ReplayReviewItem item;
    item.id = snap.id;
    item.screenMin = min;
    item.screenMax = max;
    item.label = Label(snap);
    lastItems_.push_back(std::move(item));
    ++drawn;
  }

  // Marker notches along the bottom: one per event in the window the replay
  // covers, so the fight's beats are visible without a timeline widget.
  if (style_.markers && screen.x > 0 && world_->lastTick() > world_->firstTick()) {
    const f32 y = screen.y - style_.markerBandHeight;
    for (const ReplayMarker& m : world_->markers()) {
      const f32 t = (f32)(m.tick - world_->firstTick()) /
                    (f32)(world_->lastTick() - world_->firstTick());
      const f32 x = std::clamp(t, 0.0f, 1.0f) * screen.x;
      r.DrawLine(Vec2(x, y), Vec2(x, y + style_.markerBandHeight),
                 style_.markerColor, 1.0f);
    }
  }
  return lastItems_.size();
}

std::vector<ReplayReviewItem> ReplayReview::lastItems() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return lastItems_;
}

}  // namespace Berry
