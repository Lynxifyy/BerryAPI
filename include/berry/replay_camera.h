#pragma once
// Replay camera: a keyframed camera timeline over recorded replay ticks.
//
// This is the *client feature* half of the replay mod (the tape itself lives in
// `record.h`/`session_recorder.h`). A replay is only worth watching if the
// viewpoint is scripted: the camera glides along a path while the tape plays
// back, so a viewer sees the fight from an angle the player never had. This
// class owns that path — a list of camera keys (position, pitch/yaw/roll, FOV)
// at ticks on the tape — and samples it into a `CameraState` the render path
// can adopt.
//
// Like the animator, it owns no clock: `Advance(dtSeconds)` is the whole input,
// so the host tests drive it deterministically and a paused client does not
// advance. Every feed is bounded (keys, FOV range, speed) and every read fails
// closed (no keys samples to an invalid pose and builds no camera), so a script
// cannot leave the renderer with a camera it cannot trust.
//
// It is client-side and read-only with respect to the world: it changes where
// this client looks while replaying what it already recorded. It reveals
// nothing the client was not already sent, because a tape is only ever what
// this client saw.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/draw.h"

#include <mutex>
#include <string>
#include <vector>

namespace Berry {

/// One camera key: where the camera is and how it is aimed at `tick`.
struct BERRY_API CameraKey {
  u64 tick = 0;          // position on the timeline, in replay ticks
  Vec3 position;         // eye position, world blocks
  f32 pitch = 0.0f;      // degrees, positive looks up (MCPE convention)
  f32 yaw = 0.0f;        // degrees, 0 looks toward +Z
  f32 roll = 0.0f;       // degrees, about the view axis
  f32 fov = 70.0f;       // vertical field of view, degrees
};

/// How the sample between two keys is formed.
enum class CameraSmoothing : u32 {
  /// Straight lerp between the bracketing keys.
  Linear = 0,
  /// Catmull-Rom through the neighbouring keys, so a path curves instead of
  /// turning at each key. Angles and FOV use an eased lerp instead, which
  /// cannot overshoot past a key the way a spline can.
  Smooth,
};

const char* CameraSmoothingName(CameraSmoothing s);
/// Parses a smoothing name ("linear"/"smooth"), or false when it names neither.
bool CameraSmoothingFromName(const std::string& name, CameraSmoothing* out);

/// A sampled camera pose. `valid` is false when there are no keys to sample.
struct BERRY_API CameraSample {
  Vec3 position;
  f32 pitch = 0.0f;
  f32 yaw = 0.0f;
  f32 roll = 0.0f;
  f32 fov = 70.0f;
  bool valid = false;
};

/// Owns a camera timeline and its playhead. Thread-safe.
class BERRY_API ReplayCamera {
 public:
  // Bounds: a script cannot grow the timeline without limit, and a speed or FOV
  // outside a sane range is refused rather than clamped silently.
  static constexpr size_t kMaxKeys = 4096;
  static constexpr f32 kTicksPerSecond = 20.0f;   // the sim tick rate
  static constexpr f32 kMinFov = 1.0f;
  static constexpr f32 kMaxFov = 179.0f;
  static constexpr f32 kMaxSpeed = 64.0f;

  ReplayCamera() = default;

  //--- authoring -----------------------------------------------------------

  /// Inserts a key. False when the position or angles are not finite, the FOV
  /// is outside `[kMinFov, kMaxFov]`, the key cap is reached, or a key already
  /// exists at `tick` (replace it with `RemoveKey` first, so authoring is never
  /// ambiguous about which of two keys at one tick wins).
  bool AddKey(const CameraKey& key);
  /// Removes the key at `tick`. False when there is none.
  bool RemoveKey(u64 tick);
  /// Drops every key and stops playback.
  void Clear();

  size_t keyCount() const;
  bool empty() const;
  bool hasKeyAt(u64 tick) const;
  /// The first and last key ticks, or 0 when there are no keys.
  u64 firstTick() const;
  u64 lastTick() const;
  /// `lastTick - firstTick`, or 0 when there are fewer than two keys.
  u64 durationTicks() const;

  //--- smoothing -----------------------------------------------------------

  CameraSmoothing smoothing() const;
  void SetSmoothing(CameraSmoothing s);

  //--- playback ------------------------------------------------------------

  /// True when the frame hook should drive the renderer's camera from this
  /// timeline. Off by default, so binding a replay camera never hijacks the
  /// player's view until a script asks it to.
  bool active() const;
  void SetActive(bool on);

  /// Starts playback from the current playhead (or the first key when the
  /// playhead is outside the range). A no-op with no keys.
  void Play();
  /// Stops advancing but keeps the playhead, so the view holds its frame.
  void Pause();
  /// Pauses and rewinds the playhead to the first key.
  void Stop();
  bool playing() const;

  f32 speed() const;
  /// Sets the playback speed multiplier. Must be finite and in
  /// `(0, kMaxSpeed]`; a bad value is refused and leaves the speed unchanged.
  bool SetSpeed(f32 speed);

  /// The playhead in ticks (fractional, so a sample can fall between keys).
  f32 playhead() const;
  /// Moves the playhead, clamped to `[firstTick, lastTick]`. False when there
  /// are no keys (nothing to seek within).
  bool Seek(f32 tick);

  /// Advances the playhead by `dtSeconds * kTicksPerSecond * speed`. A
  /// non-finite or negative delta is ignored. A playhead that reaches the last
  /// key stops playback there, so a replay holds its final view.
  void Advance(f32 dtSeconds);

  //--- sampling ------------------------------------------------------------

  /// The camera pose at an absolute tick. Before the first key it holds the
  /// first; past the last it holds the last. Invalid with no keys.
  CameraSample Sample(f32 tick) const;
  /// The pose at the current playhead.
  CameraSample SampleNow() const;

  //--- FOV -----------------------------------------------------------------

  /// The FOV override in degrees, or 0 when the keyframe FOV is used.
  f32 fovOverride() const;
  /// Pins the FOV, or clears the pin with 0. A non-zero value must be finite
  /// and in `[kMinFov, kMaxFov]`; a bad value is refused.
  bool SetFovOverride(f32 fov);

  //--- camera build --------------------------------------------------------

  /// Builds a `CameraState` for `aspect` (width / height) from the current
  /// sample. False (and `out` untouched) when there are no keys or the aspect
  /// is not finite and positive. `width`/`height` are recorded on the camera
  /// for a caller that projects to pixels.
  bool BuildCamera(f32 aspect, f32 width, f32 height, CameraState* out) const;
  bool BuildCameraAt(f32 tick, f32 aspect, f32 width, f32 height,
                     CameraState* out) const;

  //--- direct (offline) rendering ------------------------------------------
  //
  // An export walks the timeline at a fixed frames-per-second rate instead of
  // real time, so a replay renders to a video of a known length whatever the
  // machine's frame rate is: each `ExportStep` yields the camera for the next
  // video frame and advances one video-frame's worth of ticks, independent of
  // how long the frame actually took. The frame hook drives this (it holds the
  // renderer's camera), so an export is just the normal path with a synthetic
  // clock.

  /// Begins an export at `fps`, starting from the first key. False when `fps`
  /// is not finite and in `(0, 240]`, there are no keys, or the viewport is not
  /// finite and positive. An export already in progress is replaced.
  bool ExportBegin(f32 fps, f32 aspect, f32 width, f32 height);
  /// Yields the next video frame's camera into `out` and advances the export
  /// clock one frame. False (and `out` untouched) at the end of the timeline,
  /// which also ends the export.
  bool ExportStep(CameraState* out);
  /// Ends an export. A no-op when none is running.
  void ExportEnd();
  bool exporting() const;
  /// The number of frames yielded so far by the current export.
  u64 exportFrame() const;
  /// The export frame rate, or 0 when not exporting.
  f32 exportFps() const;

 private:
  /// The keys bracketing `tick`. `lo`/`hi` are indices into `keys_`; `t` is the
  /// 0..1 fraction between them. Caller holds `mutex_`. False with no keys.
  bool BracketLocked(f32 tick, size_t* lo, size_t* hi, f32* t) const;
  CameraSample SampleLocked(f32 tick) const;

  mutable std::mutex mutex_;
  std::vector<CameraKey> keys_;   // kept sorted by tick
  CameraSmoothing smoothing_ = CameraSmoothing::Linear;
  bool active_ = false;
  bool playing_ = false;
  f32 speed_ = 1.0f;
  f32 playhead_ = 0.0f;
  f32 fovOverride_ = 0.0f;

  // Export state (guarded by mutex_).
  bool exporting_ = false;
  f32 exportFps_ = 0.0f;
  f32 exportTick_ = 0.0f;
  u64 exportFrame_ = 0;
  f32 exportAspect_ = 1.0f;
  f32 exportWidth_ = 0.0f;
  f32 exportHeight_ = 0.0f;
};

/// Builds a view matrix for a sampled pose. Shared by `ReplayCamera` and any
/// caller that already has a pose, so the up-vector convention (world up, then
/// rolled about the view axis) is defined in exactly one place.
BERRY_API Mat4 CameraViewMatrix(const Vec3& eye, f32 pitch, f32 yaw, f32 roll);

}  // namespace Berry
