#pragma once
// BerryAPI performance governor.
//
// The headline FPS feature: watch real frame time, and when it drifts away
// from a target, spend the quality budget on the settings that hurt least.
// It is a closed feedback loop, not a fixed preset:
//
//   frame time high  -> step quality down (particles, then entity distance,
//                       then render distance), one notch at a time
//   frame time low   -> step quality back up, but slowly, and never past the
//                       ceiling the user or a thermal state set
//
// Two ideas keep it honest:
//   * Hysteresis. A single slow frame is noise (a chunk upload, a GC pause);
//     the governor only moves after the frame time has stayed outside a dead
//     band for several frames. This stops the visible "breathing" that naive
//     governors cause.
//   * Thermal awareness. A device that is already hot throttles itself, and
//     the governor reacting to that throttle would push quality down further
//     for no gain. When the platform reports thermal pressure the governor
//     pins to a low ceiling and stops trying to climb back up until the device
//     cools, which keeps the frame rate steady instead of oscillating.
//
// This is a client-side, local-only component: it changes the local client's
// own render settings and nothing else.

#include "berry/export.h"
#include "berry/types.h"

#include <array>
#include <deque>
#include <string>
#include <vector>

namespace Berry {

/// A single tunable quality knob, from the cheapest to give up (particles) to
/// the most visually important (render distance).
enum class QualityKnob : u8 {
  Particles = 0,        // particle spawn rate / cap
  EntityDistance,       // max distance entities are drawn at
  AnimationDistance,    // max distance mob animations update at
  EntityCulling,        // cull far entities entirely
  LightingUpdates,      // throttle light propagation
  RenderDistance,       // chunk draw distance
  Count,
};

const char* QualityKnobName(QualityKnob k);

/// A per-knob quality value in [0,1], where 1 is full quality. Kept as a
/// fraction so the governor can scale each knob's concrete setting from its
/// own range.
struct BERRY_API QualityLevel {
  std::array<f32, (size_t)QualityKnob::Count> v{};

  QualityLevel() {
    for (f32& x : v) x = 1.0f;
  }

  f32 Get(QualityKnob k) const { return v[(size_t)k]; }
  void Set(QualityKnob k, f32 value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    v[(size_t)k] = value;
  }

  /// True when every knob matches within `eps`.
  bool Equals(const QualityLevel& o, f32 eps = 1e-4f) const {
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i] < o.v[i] - eps || v[i] > o.v[i] + eps) return false;
    }
    return true;
  }

  /// Mean quality across all knobs, 0..1.
  f32 Mean() const {
    f32 sum = 0;
    for (f32 x : v) sum += x;
    return sum / (f32)v.size();
  }
};

/// Concrete engine settings the governor drives, derived from a QualityLevel.
struct BERRY_API RenderSettings {
  i32 renderDistanceChunks = 12;
  i32 maxParticles = 4000;
  f32 entityDistance = 64.0f;
  f32 animationDistance = 32.0f;
  bool cullFarEntities = false;
  i32 lightingUpdatesPerTick = 8;
  i32 targetFps = 60;

  bool operator==(const RenderSettings& o) const {
    return renderDistanceChunks == o.renderDistanceChunks &&
           maxParticles == o.maxParticles &&
           entityDistance == o.entityDistance &&
           animationDistance == o.animationDistance &&
           cullFarEntities == o.cullFarEntities &&
           lightingUpdatesPerTick == o.lightingUpdatesPerTick &&
           targetFps == o.targetFps;
  }
};

/// The full range for one knob: the value at quality 0 and at quality 1.
struct BERRY_API KnobRange {
  f32 minValue = 0;
  f32 maxValue = 1;
};

/// Maps a QualityLevel onto concrete engine settings. Ranges are data so a
/// device profile can retune the governor without touching the loop.
struct BERRY_API QualityProfile {
  KnobRange particles{500.0f, 4000.0f};
  KnobRange entityDistance{24.0f, 64.0f};
  KnobRange animationDistance{16.0f, 32.0f};
  KnobRange renderDistance{4.0f, 12.0f};
  KnobRange lightingUpdates{1.0f, 8.0f};
  /// Entity culling turns on below this quality fraction.
  f32 cullThreshold = 0.5f;

  static QualityProfile Default();
  RenderSettings ToSettings(const QualityLevel& q, i32 targetFps) const;
};

/// Platform thermal pressure, as reported by the OS.
enum class ThermalState : u8 {
  Nominal = 0,
  Fair,
  Serious,
  Critical,
  Shutdown,
};

const char* ThermalStateName(ThermalState s);

/// What the governor did on a given update, for logging and the profiler HUD.
enum class GovernorAction : u8 {
  None = 0,
  Reduced,
  Restored,
  ThermalClamp,
};

const char* GovernorActionName(GovernorAction a);

/// Rolling frame-time statistics.
struct BERRY_API FrameStats {
  f64 avgMs = 0;
  f64 p95Ms = 0;
  f64 minMs = 0;
  f64 maxMs = 0;
  u64 samples = 0;

  f64 Fps() const { return avgMs > 0 ? 1000.0 / avgMs : 0; }
};

struct BERRY_API GovernorConfig {
  i32 targetFps = 60;
  /// Frame time is "too slow" above target * (1 + slowMargin) and "fast
  /// enough to restore" below target * (1 - fastMargin). The gap between them
  /// is the dead band that absorbs jitter.
  f32 slowMargin = 0.10f;
  f32 fastMargin = 0.15f;
  /// Consecutive frames outside the band before the governor acts.
  int framesToReact = 8;
  /// Quality step per action, as a fraction of full quality.
  f32 stepSize = 0.08f;
  /// How many frames of history the rolling stats keep.
  size_t windowFrames = 120;
  /// Clamp applied when the platform reports thermal pressure.
  f32 thermalCeiling = 0.4f;
  /// Never reduce below this: some visual quality is always kept.
  f32 floorQuality = 0.25f;
};

/// Answers the render hooks' "cull or keep" question from the current
/// settings. Pure functions of the settings plus the draw's own coordinates,
/// so the decision is deterministic and testable without a GPU.
struct BERRY_API CullPolicy {
  RenderSettings settings;

  /// Should an entity at `distance` from the camera be drawn? Entities are
  /// culled past `entityDistance` only when culling is enabled; otherwise they
  /// are always kept (the distance still feeds the animation decision).
  bool KeepEntity(f32 distance) const {
    if (!settings.cullFarEntities) return true;
    return distance <= settings.entityDistance;
  }

  /// Should an entity's skeleton animate? Cheaper than culling: a distant mob
  /// still draws, it just stops updating its animation.
  bool AnimateEntity(f32 distance) const {
    return distance <= settings.animationDistance;
  }

  /// Given how many particles are already alive, should one more spawn?
  /// This is the count-cap half of the particle limiter; distance culling is
  /// the caller's job (it knows the spawn position).
  bool AllowParticle(i32 aliveCount) const {
    if (settings.maxParticles <= 0) return false;
    return aliveCount < settings.maxParticles;
  }

  /// Combined particle decision: a spawn is allowed when it is inside the
  /// entity distance and the live count is under the cap.
  bool AllowParticleAt(f32 distance, i32 aliveCount) const {
    return distance <= settings.entityDistance && AllowParticle(aliveCount);
  }
};

/// The adaptive quality governor.
class BERRY_API PerformanceGovernor {
 public:
  PerformanceGovernor() = default;

  void Configure(const GovernorConfig& cfg);
  const GovernorConfig& config() const { return cfg_; }
  void SetProfile(const QualityProfile& p) { profile_ = p; }
  const QualityProfile& profile() const { return profile_; }

  /// Records one frame's duration and re-evaluates the quality level.
  /// Returns the action taken this frame (None when inside the dead band).
  GovernorAction OnFrame(f32 frameMs);

  /// Reports platform thermal pressure. While a device is thermally limited
  /// the governor holds a low ceiling and stops restoring quality.
  GovernorAction OnThermal(ThermalState state);
  ThermalState thermalState() const { return thermal_; }
  bool thermallyLimited() const {
    return thermal_ == ThermalState::Serious ||
           thermal_ == ThermalState::Critical ||
           thermal_ == ThermalState::Shutdown;
  }

  /// A hard ceiling the user (or a policy) set. Quality never exceeds it.
  void SetCeiling(f32 ceiling);
  f32 ceiling() const { return ceiling_; }

  /// The current quality level and the concrete settings it implies.
  const QualityLevel& quality() const { return quality_; }
  RenderSettings settings() const { return profile_.ToSettings(quality_, cfg_.targetFps); }

  /// True when the settings changed on the most recent update.
  bool settingsDirty() const { return dirty_; }
  void ClearDirty() { dirty_ = false; }

  FrameStats stats() const;
  const std::deque<f32>& frameHistory() const { return frameMs_; }

  /// Forces a quality level (used by tests and a manual override).
  void SetQuality(const QualityLevel& q);

  void Reset();

 private:
  GovernorAction Step(f32 direction);
  void PushFrame(f32 frameMs);

  GovernorConfig cfg_{};
  QualityProfile profile_ = QualityProfile::Default();
  QualityLevel quality_{};
  std::deque<f32> frameMs_;
  ThermalState thermal_ = ThermalState::Nominal;
  f32 ceiling_ = 1.0f;
  f32 thermalHold_ = 1.0f;
  int slowStreak_ = 0;
  int fastStreak_ = 0;
  bool dirty_ = false;
};

}  // namespace Berry
