#include "berry/audio.h"

#include <algorithm>
#include <cmath>

namespace Berry {

bool AudioService::ValidCueName(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  for (unsigned char c : name) {
    if (c < 0x20 || c == 0x7F) return false;
  }
  return true;
}

bool AudioService::Play(const std::string& name, f32 volume, f32 pitch) {
  if (!ValidCueName(name)) return false;
  if (!std::isfinite(volume)) volume = 1.0f;
  if (!std::isfinite(pitch)) pitch = 1.0f;
  volume = std::min(std::max(volume, 0.0f), 1.0f);
  // A pitch outside a sane musical range is clamped, not refused: it is a
  // continuous control like volume.
  pitch = std::min(std::max(pitch, 0.25f), 4.0f);

  AudioSeam* seam = nullptr;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (muted_) return false;
    seam = seam_;
    if (!seam) return false;
    history_.push_back(AudioCue{name, volume, pitch});
    while (history_.size() > kMaxHistory) history_.pop_front();
  }
  // The seam is called outside the lock: a backend may call back in.
  return seam->Play(AudioCue{name, volume, pitch});
}

void AudioService::SetVolume(f32 v) {
  if (!std::isfinite(v)) return;
  std::lock_guard<std::mutex> lk(mutex_);
  volume_ = std::min(std::max(v, 0.0f), 1.0f);
}

f32 AudioService::volume() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return volume_;
}

void AudioService::SetMuted(bool muted) {
  std::lock_guard<std::mutex> lk(mutex_);
  muted_ = muted;
}

bool AudioService::muted() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return muted_;
}

std::vector<AudioCue> AudioService::History() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return std::vector<AudioCue>(history_.begin(), history_.end());
}

size_t AudioService::HistoryCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return history_.size();
}

void AudioService::ClearHistory() {
  std::lock_guard<std::mutex> lk(mutex_);
  history_.clear();
}

}  // namespace Berry
