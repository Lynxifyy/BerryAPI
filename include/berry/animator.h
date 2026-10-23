#pragma once
// Client-side animator: keyframed body-part animation over the cosmetic
// skeleton.
//
// This is a *client feature*, not a new strict Berry:: module (the strict set
// stays Combat/Render/World/Network). It is the scripting counterpart to the
// cosmetic attach system: a script authors clips of keyframes — one bone's
// offset/rotation/scale at a time — and plays them back, and the resolved pose
// is applied to a cosmetic draw item through the same render seam the wardrobe
// uses. Nothing here is authoritative: it moves the local player's own rendered
// bones, and a replay reproduces it exactly because sampling is pure.
//
// The animator owns no clock. `Advance(dtSeconds)` is the whole input, so the
// host tests drive it deterministically and a paused client does not advance.
// Every feed is bounded (clips, keys per clip, name length), every read fails
// closed (an unknown clip or bone samples to an invalid pose), and a clip is
// replaced atomically: a malformed save/load leaves the previous clips intact.
//
// Interpolation is linear per bone: the pose at time t is the lerp between the
// two keys that bracket t, the first key before t for a time before the first
// key, and the last key after t for a time past the last key. A looping clip
// wraps t into [0, duration); a one-shot clip clamps at the ends.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/cosmetic.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace Berry {

/// One keyframe: a bone's transform at `time` seconds from the clip's start.
/// `position`/`rotation` are an offset from the bone's rest transform, so a clip
/// of all-zero keys is a no-op.
struct BERRY_API AnimKey {
  Bone bone = Bone::None;
  f32 time = 0.0f;        // seconds from clip start, >= 0
  Vec3 position;          // offset from rest, blocks
  Vec3 rotation;          // degrees: pitch, yaw, roll
  f32 scale = 1.0f;
};

/// A named clip: keyframes over a duration, looping or one-shot.
struct BERRY_API AnimClip {
  std::string name;
  f32 duration = 1.0f;    // seconds, > 0
  bool looping = true;
  std::vector<AnimKey> keys;   // kept sorted by (bone, time)
};

/// Owns clips and playback state. Thread-safe.
class BERRY_API Animator {
 public:
  // Bounds: a script cannot grow the animator without limit.
  static constexpr size_t kMaxClips = 32;
  static constexpr size_t kMaxKeysPerClip = 256;
  static constexpr size_t kMaxNameLen = 64;

  Animator() = default;

  //--- authoring -----------------------------------------------------------

  /// Adds an empty clip. False when the name is empty/too long/already used, the
  /// duration is not finite and positive, or the clip cap is reached.
  bool AddClip(const std::string& name, f32 duration, bool looping);

  /// Appends a keyframe to a clip. False when the clip is unknown, the bone is
  /// None/out of range, the time is negative or past the duration, the scale is
  /// not finite and positive, or the per-clip key cap is reached. The key list
  /// stays sorted so sampling is a binary search.
  bool AddKey(const std::string& clip, Bone bone, f32 time, const Vec3& position,
              const Vec3& rotation, f32 scale);

  /// Removes a clip. False when it is unknown. Stops playback if it was current.
  bool RemoveClip(const std::string& name);

  /// Removes every key from a clip, keeping the clip. False when unknown.
  bool ClearClip(const std::string& name);

  size_t clipCount() const;
  bool hasClip(const std::string& name) const;
  /// The number of keys in a clip, or 0 when unknown.
  size_t keyCount(const std::string& name) const;

  //--- playback ------------------------------------------------------------

  /// Starts a clip from time 0. False when the clip is unknown.
  bool Play(const std::string& name);
  /// Stops playback, leaving the current time where it is.
  void Stop();
  /// Stops and rewinds to time 0.
  void Reset();
  bool playing() const;
  /// The current play time in seconds.
  f32 time() const;
  /// The current clip's duration, or 0 when none is selected.
  f32 duration() const;
  /// The current clip's name, or "" when none is selected.
  std::string currentClip() const;

  /// Advances playback by `dtSeconds`. A non-finite or negative delta is
  /// ignored. A looping clip wraps; a one-shot clip stops at the end. A no-op
  /// when not playing.
  void Advance(f32 dtSeconds);

  /// Jumps to an absolute time within the current clip (clamped/wrapped the same
  /// way `Advance` would land there). False when no clip is selected.
  bool Seek(f32 seconds);

  //--- sampling ------------------------------------------------------------

  /// A bone's interpolated pose at an absolute time within the current clip.
  struct BonePose {
    Vec3 position;
    Vec3 rotation;
    f32 scale = 1.0f;
    bool valid = false;   // false when no clip/bone has a key here
  };
  BonePose Sample(Bone bone, f32 time) const;
  /// The bone's pose at the current play time.
  BonePose SampleNow(Bone bone) const;

  /// The whole current pose: one entry per bone that has at least one key in the
  /// current clip, at the current time. Empty when no clip is selected.
  std::vector<std::pair<Bone, BonePose>> SamplePose() const;

  //--- persistence ---------------------------------------------------------

  /// The clips as one JSON document (deterministic: same clips, same bytes).
  std::string Save() const;
  /// Replaces the clips from a JSON document. Rejects the whole document on any
  /// error and leaves the current clips untouched. Playback is stopped.
  bool Load(const std::string& json);

  /// Drops every clip and stops playback.
  void Clear();

 private:
  /// The bracketing keys for a bone at `time`, or false when the bone has none.
  /// Caller holds `mutex_`.
  bool BracketLocked(Bone bone, f32 time, const AnimKey** lo,
                     const AnimKey** hi, f32* t) const;
  /// Samples against the current clip without taking the lock. Caller holds it.
  BonePose SampleLocked(Bone bone, f32 time) const;
  const AnimClip* FindLocked(const std::string& name) const;
  AnimClip* FindLocked(const std::string& name);

  mutable std::mutex mutex_;
  std::vector<AnimClip> clips_;
  std::string current_;   // name of the selected clip, "" when none
  f32 time_ = 0.0f;
  bool playing_ = false;
};

/// A skeleton source that plays an animator's clip on top of another skeleton
/// seam. This is what makes the animator real: the cosmetic renderer resolves a
/// player's bones through this seam, so a clip authored in a script moves the
/// rendered bones instead of sitting in a table. The base seam is untouched — a
/// bone with no key in the clip passes through unchanged.
///
/// It animates one entity (the local player's own rig) by default; a target of 0
/// animates whatever the base seam resolves. While a clip is playing the seam
/// reports no pose revision, which disables the cosmetic resolve cache for that
/// entity — a cached, stationary pose must never be reused for a moving one.
class BERRY_API AnimatedSkeletonSource final : public CosmeticSkeletonSource {
 public:
  AnimatedSkeletonSource(CosmeticSkeletonSource* base, Animator* animator)
      : base_(base), animator_(animator) {}

  void SetBase(CosmeticSkeletonSource* base) { base_ = base; }
  void SetAnimator(Animator* animator) { animator_ = animator; }
  /// The entity this seam animates. 0 (the default) animates every entity the
  /// base seam resolves. Atomic: a script sets it from the worker while the
  /// render thread reads it.
  void SetTarget(EntityId entity) { targetRaw_.store(entity.raw); }
  EntityId target() const {
    EntityId id;
    id.raw = targetRaw_.load();
    return id;
  }

  bool BoneTransform(EntityId entity, Bone bone,
                     CosmeticBoneTransform* out) const override;
  bool PoseRevision(EntityId entity, u64* out) const override;

 private:
  bool Animates(EntityId entity) const {
    const u64 t = targetRaw_.load();
    return animator_ != nullptr && (t == 0 || t == entity.raw);
  }

  CosmeticSkeletonSource* base_ = nullptr;
  Animator* animator_ = nullptr;
  std::atomic<u64> targetRaw_{0};
};

}  // namespace Berry
