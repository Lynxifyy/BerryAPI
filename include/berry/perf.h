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

/// A device class the governor can tune itself for. Deliberately coarse: the
/// point is that a low-end phone and a desktop want different knob ranges and
/// different thermal behaviour, and neither should need hand-configuration.
enum class PlatformClass : u8 {
  Generic = 0,   // unknown: conservative middle-of-the-road defaults
  MobileLow,     // entry phone: narrow ranges, aggressive thermal cap
  MobileHigh,    // flagship phone / tablet
  Desktop,       // desktop or laptop: wide ranges, mild thermal cap
};

const char* PlatformClassName(PlatformClass c);

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
  /// The lowest render distance the governor may ever choose. Exposed as data
  /// because the safe floor differs by platform (a phone can afford less than
  /// a desktop before the world visibly unloads).
  i32 minRenderDistance = 2;

  static QualityProfile Default();
  /// Tuned ranges and a render-distance floor for a device class.
  static QualityProfile ForPlatform(PlatformClass c);
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
  /// 99th-percentile frame time. The governor reacts to this, not the average,
  /// because a smooth average hiding regular long frames still feels bad.
  f64 p99Ms = 0;
  f64 minMs = 0;
  f64 maxMs = 0;
  u64 samples = 0;

  f64 Fps() const { return avgMs > 0 ? 1000.0 / avgMs : 0; }
};

/// A coarse frame-time histogram, so a caller can see the shape of the tail
/// rather than just its p99. Buckets are powers-of-two-ish bands in ms; the
/// last bucket collects everything at or above its lower edge.
struct BERRY_API FrameHistogram {
  /// Lower edge (ms) of each bucket, ascending. The final entry is a catch-all.
  static constexpr size_t kBucketCount = 7;
  static const f64 kEdges[kBucketCount];

  /// Counts per bucket; `counts[i]` covers [kEdges[i], kEdges[i+1]).
  std::array<u64, kBucketCount> counts{};
  u64 total = 0;

  void Add(f64 frameMs);
  void Clear() {
    counts.fill(0);
    total = 0;
  }
  /// Index of the bucket `frameMs` falls into (always in range).
  static size_t BucketFor(f64 frameMs);
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
  /// Frames to wait after a thermal clamp before any restoration is allowed,
  /// even if frames are fast again. A device that just throttled will report
  /// fast frames for a moment (it is doing less work); restoring immediately
  /// would heat it straight back up and oscillate.
  int thermalDwellFrames = 120;
  /// While the platform reports Fair, hold quality at or below this. Fair is
  /// "warm, not throttling": reacting early keeps the device out of the
  /// Serious band instead of waiting for it.
  f32 fairCeiling = 0.7f;
  /// The p99 frame time must also be inside the slow threshold before the
  /// governor reduces, so a smooth average cannot hide a spiky tail.
  bool useTailLatency = true;
  /// Tail-latency reaction only applies once the window holds at least this
  /// many samples. In a short window the p99 IS the maximum, so reacting to it
  /// would mean "reduce on any single spike for a whole window" — the opposite
  /// of the hysteresis this governor exists for.
  size_t tailLatencyMinSamples = 30;
};

/// The adaptive quality governor.
class BERRY_API PerformanceGovernor {
 public:
  PerformanceGovernor() = default;

  void Configure(const GovernorConfig& cfg);
  const GovernorConfig& config() const { return cfg_; }
  void SetProfile(const QualityProfile& p) { profile_ = p; }
  const QualityProfile& profile() const { return profile_; }
  /// Sets both the profile and the thermal ceiling that suit a device class.
  void SetPlatform(PlatformClass c);
  PlatformClass platform() const { return platform_; }

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
  /// Frame-time histogram over the current window.
  FrameHistogram histogram() const;
  const std::deque<f32>& frameHistory() const { return frameMs_; }

  /// Forces a quality level (used by tests and a manual override).
  void SetQuality(const QualityLevel& q);

  void Reset();

 private:
  GovernorAction Step(f32 direction);
  void PushFrame(f32 frameMs);
  /// Lowers every knob to at most `cap` (used by the thermal and Fair holds).
  bool ClampQualityTo(f32 cap);

  GovernorConfig cfg_{};
  QualityProfile profile_ = QualityProfile::Default();
  QualityLevel quality_{};
  std::deque<f32> frameMs_;
  ThermalState thermal_ = ThermalState::Nominal;
  PlatformClass platform_ = PlatformClass::Generic;
  f32 ceiling_ = 1.0f;
  f32 thermalHold_ = 1.0f;
  /// Frames remaining before restoration is allowed again after a clamp.
  int dwellFrames_ = 0;
  int slowStreak_ = 0;
  int fastStreak_ = 0;
  /// Consecutive frames with a bad p99. Tracked separately from the average
  /// streak because a fast frame between two spikes must not erase the signal.
  int tailStreak_ = 0;
  bool dirty_ = false;
};

}  // namespace Berry
