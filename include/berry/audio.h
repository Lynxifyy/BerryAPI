#pragma once
// Client-side audio cues (`audio.*`).
//
// A script plays a named cue; the device backend decides what that name means.
// This is a client-side presentation concern (a sound the local player hears),
// so nothing here is authoritative and nothing is sent anywhere.
//
// The rules are deliberately small and fail closed:
//
//   * A cue name is bounded and printable; an unknown or malformed name is
//     refused rather than passed to the backend.
//   * The master volume is clamped to [0, 1]; a per-play volume is clamped the
//     same way. Muting is a hard stop: a muted play is dropped, not queued.
//   * The backend is a seam. The host build binds a recording shim (so tests can
//     assert on what would play); on device the adapter forwards to the engine.
//     With no backend bound, a play is dropped and reported false.
//
// A ring of recent plays is kept for the HUD and the tests, bounded so a script
// that plays in a loop cannot grow it without limit.

#include "berry/export.h"
#include "berry/types.h"

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace Berry {

/// One cue that was (or would be) played.
struct BERRY_API AudioCue {
  std::string name;
  f32 volume = 1.0f;
  f32 pitch = 1.0f;
};

/// The device backend. The host build binds a recording implementation; the
/// on-device adapter forwards to the engine's sound system.
class BERRY_API AudioSeam {
 public:
  virtual ~AudioSeam() = default;
  /// Plays one cue. Returns false when the backend could not play it.
  virtual bool Play(const AudioCue& cue) = 0;
};

/// Owns the master volume, the mute switch and the play path. Thread-safe.
class BERRY_API AudioService {
 public:
  /// Bounds a cue name: printable, non-empty, within a sane length.
  static bool ValidCueName(const std::string& name);

  void SetSeam(AudioSeam* seam) { seam_ = seam; }
  AudioSeam* Seam() const { return seam_; }

  /// Plays a cue. Fails closed (returns false) on an invalid name, a missing
  /// backend, or when muted. Volume/pitch are clamped into range.
  bool Play(const std::string& name, f32 volume = 1.0f, f32 pitch = 1.0f);

  /// Master volume in [0, 1]. Out-of-range input is clamped, not refused: a
  /// volume is a continuous control and clamping is the useful reading.
  void SetVolume(f32 v);
  f32 volume() const;
  void SetMuted(bool muted);
  bool muted() const;

  /// The last `n` cues played, oldest first, for a HUD or a test. Bounded by
  /// kMaxHistory regardless of how many were played.
  static constexpr size_t kMaxHistory = 32;
  std::vector<AudioCue> History() const;
  size_t HistoryCount() const;
  void ClearHistory();

 private:
  mutable std::mutex mutex_;
  AudioSeam* seam_ = nullptr;
  f32 volume_ = 1.0f;
  bool muted_ = false;
  std::deque<AudioCue> history_;
};

}  // namespace Berry
