#include "berry/replay_camera.h"

#include <algorithm>
#include <cmath>

namespace Berry {

namespace {

/// True when every component of a pose is a usable finite number.
bool FinitePose(const CameraKey& k) {
  return std::isfinite(k.position.x) && std::isfinite(k.position.y) &&
         std::isfinite(k.position.z) && std::isfinite(k.pitch) &&
         std::isfinite(k.yaw) && std::isfinite(k.roll) && std::isfinite(k.fov);
}

/// Wraps an angle into (-180, 180], so interpolating yaw 350 -> 10 goes the
/// short way across 0 instead of the long way back through 180.
f32 WrapDeg(f32 deg) {
  f32 d = std::fmod(deg, 360.0f);
  if (d <= -180.0f) d += 360.0f;
  if (d > 180.0f) d -= 360.0f;
  return d;
}

/// Interpolates two angles along the shorter arc.
f32 LerpAngle(f32 a, f32 b, f32 t) { return a + WrapDeg(b - a) * t; }

f32 LerpF(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

/// Catmull-Rom through p1 with neighbours p0/p2/p3, evaluated at t in [0,1]
/// between p1 and p2.
Vec3 CatmullRom(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3,
                f32 t) {
  const f32 t2 = t * t;
  const f32 t3 = t2 * t;
  const f32 m0 = -0.5f * t3 + t2 - 0.5f * t;
  const f32 m1 = 1.5f * t3 - 2.5f * t2 + 1.0f;
  const f32 m2 = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
  const f32 m3 = 0.5f * t3 - 0.5f * t2;
  return p0 * m0 + p1 * m1 + p2 * m2 + p3 * m3;
}

/// Ease in/out, for angles and FOV under `Smooth`. A spline can overshoot a
/// key; an eased lerp cannot, so an aim direction never swings past its target.
f32 Ease(f32 t) { return t * t * (3.0f - 2.0f * t); }

/// The direction (pitch, yaw) points at, in the MCPE convention (yaw about +Y,
/// pitch up positive): matches LookDirection in api_bindings.cpp and Vec3::Angle.
Vec3 DirectionFrom(f32 pitchDeg, f32 yawDeg) {
  const f32 p = pitchDeg * Mathf::DEG2RAD;
  const f32 y = yawDeg * Mathf::DEG2RAD;
  const f32 cp = std::cos(p);
  return Vec3(-std::sin(y) * cp, -std::sin(p), std::cos(y) * cp).Normalized();
}

}  // namespace

const char* CameraSmoothingName(CameraSmoothing s) {
  switch (s) {
    case CameraSmoothing::Linear: return "linear";
    case CameraSmoothing::Smooth: return "smooth";
  }
  return "linear";
}

bool CameraSmoothingFromName(const std::string& name, CameraSmoothing* out) {
  if (!out) return false;
  if (name == "linear") { *out = CameraSmoothing::Linear; return true; }
  if (name == "smooth") { *out = CameraSmoothing::Smooth; return true; }
  return false;
}

//----------------------------------------------------------------------------
// Authoring
//----------------------------------------------------------------------------

bool ReplayCamera::AddKey(const CameraKey& key) {
  if (!FinitePose(key)) return false;
  if (key.fov < kMinFov || key.fov > kMaxFov) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  if (keys_.size() >= kMaxKeys) return false;
  // Sorted by tick, and a tick is unique, so a sample always has one answer.
  auto it = std::lower_bound(keys_.begin(), keys_.end(), key.tick,
                             [](const CameraKey& a, u64 t) { return a.tick < t; });
  if (it != keys_.end() && it->tick == key.tick) return false;
  keys_.insert(it, key);
  return true;
}

bool ReplayCamera::RemoveKey(u64 tick) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = std::lower_bound(keys_.begin(), keys_.end(), tick,
                             [](const CameraKey& a, u64 t) { return a.tick < t; });
  if (it == keys_.end() || it->tick != tick) return false;
  keys_.erase(it);
  return true;
}

void ReplayCamera::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  keys_.clear();
  playing_ = false;
  playhead_ = 0.0f;
  exporting_ = false;
  exportFps_ = 0.0f;
  exportTick_ = 0.0f;
  exportFrame_ = 0;
}

size_t ReplayCamera::keyCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return keys_.size();
}

bool ReplayCamera::empty() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return keys_.empty();
}

bool ReplayCamera::hasKeyAt(u64 tick) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = std::lower_bound(keys_.begin(), keys_.end(), tick,
                             [](const CameraKey& a, u64 t) { return a.tick < t; });
  return it != keys_.end() && it->tick == tick;
}

u64 ReplayCamera::firstTick() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return keys_.empty() ? 0 : keys_.front().tick;
}

u64 ReplayCamera::lastTick() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return keys_.empty() ? 0 : keys_.back().tick;
}

u64 ReplayCamera::durationTicks() const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (keys_.size() < 2) return 0;
  return keys_.back().tick - keys_.front().tick;
}

//----------------------------------------------------------------------------
// Smoothing
//----------------------------------------------------------------------------

CameraSmoothing ReplayCamera::smoothing() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return smoothing_;
}

void ReplayCamera::SetSmoothing(CameraSmoothing s) {
  std::lock_guard<std::mutex> lk(mutex_);
  smoothing_ = s;
}

//----------------------------------------------------------------------------
// Playback
//----------------------------------------------------------------------------

bool ReplayCamera::active() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return active_;
}

void ReplayCamera::SetActive(bool on) {
  std::lock_guard<std::mutex> lk(mutex_);
  active_ = on;
}

void ReplayCamera::Play() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (keys_.empty()) return;
  const f32 first = (f32)keys_.front().tick;
  const f32 last = (f32)keys_.back().tick;
  // A playhead at the very end would stop immediately; restart from the top so
  // Play always plays something.
  if (playhead_ >= last || playhead_ < first) playhead_ = first;
  playing_ = true;
}

void ReplayCamera::Pause() {
  std::lock_guard<std::mutex> lk(mutex_);
  playing_ = false;
}

void ReplayCamera::Stop() {
  std::lock_guard<std::mutex> lk(mutex_);
  playing_ = false;
  playhead_ = keys_.empty() ? 0.0f : (f32)keys_.front().tick;
}

bool ReplayCamera::playing() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return playing_;
}

f32 ReplayCamera::speed() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return speed_;
}

bool ReplayCamera::SetSpeed(f32 speed) {
  if (!std::isfinite(speed) || speed <= 0.0f || speed > kMaxSpeed) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  speed_ = speed;
  return true;
}

f32 ReplayCamera::playhead() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return playhead_;
}

bool ReplayCamera::Seek(f32 tick) {
  if (!std::isfinite(tick)) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  if (keys_.empty()) return false;
  const f32 first = (f32)keys_.front().tick;
  const f32 last = (f32)keys_.back().tick;
  playhead_ = Mathf::Clamp(tick, first, last);
  return true;
}

void ReplayCamera::Advance(f32 dtSeconds) {
  if (!std::isfinite(dtSeconds) || dtSeconds <= 0.0f) return;
  std::lock_guard<std::mutex> lk(mutex_);
  if (!playing_ || keys_.empty()) return;
  const f32 last = (f32)keys_.back().tick;
  playhead_ += dtSeconds * kTicksPerSecond * speed_;
  if (playhead_ >= last) {
    playhead_ = last;
    playing_ = false;   // a replay holds its final view
  }
}

//----------------------------------------------------------------------------
// Sampling
//----------------------------------------------------------------------------

bool ReplayCamera::BracketLocked(f32 tick, size_t* lo, size_t* hi, f32* t) const {
  if (keys_.empty()) return false;
  const size_t n = keys_.size();
  if (n == 1 || tick <= (f32)keys_.front().tick) {
    *lo = *hi = 0;
    *t = 0.0f;
    return true;
  }
  if (tick >= (f32)keys_.back().tick) {
    *lo = *hi = n - 1;
    *t = 0.0f;
    return true;
  }
  // The first key strictly after `tick`.
  const size_t upper = (size_t)(std::upper_bound(
      keys_.begin(), keys_.end(), tick,
      [](f32 v, const CameraKey& a) { return v < (f32)a.tick; }) - keys_.begin());
  *lo = upper - 1;
  *hi = upper;
  const f32 span = (f32)keys_[upper].tick - (f32)keys_[upper - 1].tick;
  *t = span > 0.0f ? (tick - (f32)keys_[upper - 1].tick) / span : 0.0f;
  return true;
}

CameraSample ReplayCamera::SampleLocked(f32 tick) const {
  CameraSample out;
  if (!std::isfinite(tick)) return out;
  size_t lo = 0, hi = 0;
  f32 t = 0.0f;
  if (!BracketLocked(tick, &lo, &hi, &t)) return out;

  const CameraKey& a = keys_[lo];
  const CameraKey& b = keys_[hi];
  if (lo == hi || smoothing_ == CameraSmoothing::Linear) {
    out.position = a.position.Lerp(b.position, t);
  } else {
    // Catmull-Rom needs one neighbour on each side; clamp at the ends so the
    // path still starts and ends exactly on its first and last key.
    const CameraKey& p0 = keys_[lo == 0 ? 0 : lo - 1];
    const CameraKey& p3 = keys_[hi + 1 < keys_.size() ? hi + 1 : hi];
    out.position = CatmullRom(p0.position, a.position, b.position, p3.position, t);
  }
  const f32 et = (smoothing_ == CameraSmoothing::Smooth && lo != hi) ? Ease(t) : t;
  out.pitch = LerpAngle(a.pitch, b.pitch, et);
  out.yaw = LerpAngle(a.yaw, b.yaw, et);
  out.roll = LerpAngle(a.roll, b.roll, et);
  out.fov = LerpF(a.fov, b.fov, et);
  if (fovOverride_ > 0.0f) out.fov = fovOverride_;
  out.valid = true;
  return out;
}

CameraSample ReplayCamera::Sample(f32 tick) const {
  std::lock_guard<std::mutex> lk(mutex_);
  return SampleLocked(tick);
}

CameraSample ReplayCamera::SampleNow() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return SampleLocked(playhead_);
}

//----------------------------------------------------------------------------
// FOV
//----------------------------------------------------------------------------

f32 ReplayCamera::fovOverride() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return fovOverride_;
}

bool ReplayCamera::SetFovOverride(f32 fov) {
  if (fov != 0.0f && (!std::isfinite(fov) || fov < kMinFov || fov > kMaxFov)) {
    return false;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  fovOverride_ = fov;
  return true;
}

//----------------------------------------------------------------------------
// Camera build
//----------------------------------------------------------------------------

Mat4 CameraViewMatrix(const Vec3& eye, f32 pitch, f32 yaw, f32 roll) {
  const Vec3 fwd = DirectionFrom(pitch, yaw);
  // The view's up is world up rolled about the view axis, so roll 0 keeps the
  // horizon level and a non-zero roll banks the camera.
  const Vec3 worldUp = Vec3::Up();
  Vec3 up = worldUp - fwd * worldUp.Dot(fwd);
  if (up.LengthSq() < 1e-8f) up = Vec3(0, 0, 1);   // looking straight up/down
  up = up.Normalized();
  if (roll != 0.0f) {
    const f32 r = roll * Mathf::DEG2RAD;
    const Vec3 right = fwd.Cross(up).Normalized();
    up = (up * std::cos(r) + right * std::sin(r)).Normalized();
  }
  return Mat4::LookAt(eye, eye + fwd, up);
}

bool ReplayCamera::BuildCameraAt(f32 tick, f32 aspect, f32 width, f32 height,
                                 CameraState* out) const {
  if (!out) return false;
  if (!std::isfinite(aspect) || aspect <= 0.0f) return false;
  const CameraSample s = Sample(tick);
  if (!s.valid) return false;
  out->view = CameraViewMatrix(s.position, s.pitch, s.yaw, s.roll);
  out->projection = Mat4::Perspective(s.fov, aspect, 0.05f, 1024.0f);
  out->width = width;
  out->height = height;
  out->valid = true;
  return true;
}

bool ReplayCamera::BuildCamera(f32 aspect, f32 width, f32 height,
                               CameraState* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!out) return false;
  if (!std::isfinite(aspect) || aspect <= 0.0f) return false;
  const CameraSample s = SampleLocked(playhead_);
  if (!s.valid) return false;
  out->view = CameraViewMatrix(s.position, s.pitch, s.yaw, s.roll);
  out->projection = Mat4::Perspective(s.fov, aspect, 0.05f, 1024.0f);
  out->width = width;
  out->height = height;
  out->valid = true;
  return true;
}

//----------------------------------------------------------------------------
// Export
//----------------------------------------------------------------------------

bool ReplayCamera::ExportBegin(f32 fps, f32 aspect, f32 width, f32 height) {
  if (!std::isfinite(fps) || fps <= 0.0f || fps > 240.0f) return false;
  if (!std::isfinite(aspect) || aspect <= 0.0f) return false;
  if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0f ||
      height <= 0.0f) {
    return false;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  if (keys_.empty()) return false;
  exporting_ = true;
  exportFps_ = fps;
  exportTick_ = (f32)keys_.front().tick;
  exportFrame_ = 0;
  exportAspect_ = aspect;
  exportWidth_ = width;
  exportHeight_ = height;
  return true;
}

bool ReplayCamera::ExportStep(CameraState* out) {
  if (!out) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  if (!exporting_ || keys_.empty()) return false;
  const f32 last = (f32)keys_.back().tick;
  if (exportTick_ > last) {
    exporting_ = false;
    return false;
  }
  const CameraSample s = SampleLocked(exportTick_);
  if (!s.valid) {
    exporting_ = false;
    return false;
  }
  out->view = CameraViewMatrix(s.position, s.pitch, s.yaw, s.roll);
  out->projection = Mat4::Perspective(s.fov, exportAspect_, 0.05f, 1024.0f);
  out->width = exportWidth_;
  out->height = exportHeight_;
  out->valid = true;
  // Advance one video frame's worth of ticks, whatever the machine's real
  // frame rate is: an export's length is a property of the timeline, not the
  // machine that rendered it.
  exportTick_ += kTicksPerSecond / exportFps_;
  ++exportFrame_;
  return true;
}

void ReplayCamera::ExportEnd() {
  std::lock_guard<std::mutex> lk(mutex_);
  exporting_ = false;
  exportFps_ = 0.0f;
  exportTick_ = 0.0f;
  exportFrame_ = 0;
}

bool ReplayCamera::exporting() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return exporting_;
}

u64 ReplayCamera::exportFrame() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return exportFrame_;
}

f32 ReplayCamera::exportFps() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return exportFps_;
}

}  // namespace Berry
