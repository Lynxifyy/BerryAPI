// Adaptive FPS governor. See include/berry/perf.h.
#include "berry/perf.h"

#include <algorithm>
#include <cmath>

namespace Berry {

const char* QualityKnobName(QualityKnob k) {
  switch (k) {
    case QualityKnob::Particles: return "particles";
    case QualityKnob::EntityDistance: return "entity-distance";
    case QualityKnob::AnimationDistance: return "animation-distance";
    case QualityKnob::EntityCulling: return "entity-culling";
    case QualityKnob::LightingUpdates: return "lighting-updates";
    case QualityKnob::RenderDistance: return "render-distance";
    case QualityKnob::Count: break;
  }
  return "unknown";
}

const char* ThermalStateName(ThermalState s) {
  switch (s) {
    case ThermalState::Nominal: return "nominal";
    case ThermalState::Fair: return "fair";
    case ThermalState::Serious: return "serious";
    case ThermalState::Critical: return "critical";
    case ThermalState::Shutdown: return "shutdown";
  }
  return "unknown";
}

const char* GovernorActionName(GovernorAction a) {
  switch (a) {
    case GovernorAction::None: return "none";
    case GovernorAction::Reduced: return "reduced";
    case GovernorAction::Restored: return "restored";
    case GovernorAction::ThermalClamp: return "thermal-clamp";
  }
  return "unknown";
}

const char* PlatformClassName(PlatformClass c) {
  switch (c) {
    case PlatformClass::Generic: return "generic";
    case PlatformClass::MobileLow: return "mobile-low";
    case PlatformClass::MobileHigh: return "mobile-high";
    case PlatformClass::Desktop: return "desktop";
  }
  return "unknown";
}

const f64 FrameHistogram::kEdges[FrameHistogram::kBucketCount] = {
    0.0, 8.0, 16.0, 24.0, 33.0, 50.0, 100.0};

void FrameHistogram::Add(f64 frameMs) {
  // A non-finite sample would corrupt every later comparison; drop it rather
  // than bucket it (the governor's PushFrame already rejects these, but the
  // histogram is a public type a caller may drive directly).
  if (!std::isfinite(frameMs) || frameMs < 0.0) return;
  counts[BucketFor(frameMs)] += 1;
  ++total;
}

size_t FrameHistogram::BucketFor(f64 frameMs) {
  // Walk the edges backwards so a value lands in the highest bucket whose
  // lower edge it meets. The final bucket is the catch-all for the tail.
  for (size_t i = kBucketCount; i-- > 0;) {
    if (frameMs >= kEdges[i]) return i;
  }
  return 0;
}

QualityProfile QualityProfile::Default() { return QualityProfile{}; }

QualityProfile QualityProfile::ForPlatform(PlatformClass c) {
  QualityProfile p;
  switch (c) {
    case PlatformClass::MobileLow:
      // A small phone cannot afford 12 chunks or 4000 particles, and pulling
      // render distance to 2 is a real win there.
      p.particles = KnobRange{200.0f, 1500.0f};
      p.entityDistance = KnobRange{16.0f, 40.0f};
      p.animationDistance = KnobRange{8.0f, 20.0f};
      p.renderDistance = KnobRange{2.0f, 8.0f};
      p.lightingUpdates = KnobRange{1.0f, 4.0f};
      p.cullThreshold = 0.6f;
      p.minRenderDistance = 2;
      break;
    case PlatformClass::MobileHigh:
      p.particles = KnobRange{400.0f, 3000.0f};
      p.entityDistance = KnobRange{20.0f, 56.0f};
      p.animationDistance = KnobRange{12.0f, 28.0f};
      p.renderDistance = KnobRange{3.0f, 10.0f};
      p.lightingUpdates = KnobRange{1.0f, 6.0f};
      p.cullThreshold = 0.55f;
      p.minRenderDistance = 3;
      break;
    case PlatformClass::Desktop:
      // Headroom to spare: keep quality high and cull late.
      p.particles = KnobRange{1000.0f, 8000.0f};
      p.entityDistance = KnobRange{32.0f, 96.0f};
      p.animationDistance = KnobRange{24.0f, 48.0f};
      p.renderDistance = KnobRange{6.0f, 16.0f};
      p.lightingUpdates = KnobRange{2.0f, 12.0f};
      p.cullThreshold = 0.35f;
      p.minRenderDistance = 4;
      break;
    case PlatformClass::Generic:
      break;   // the member defaults
  }
  return p;
}

namespace {

f32 Lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

i32 LerpI(f32 a, f32 b, f32 t) {
  const f32 v = Lerp(a, b, t);
  return (i32)std::lround(v);
}

/// The order the governor spends quality in, cheapest-to-lose first. Reduce
/// walks it forwards; restore walks it backwards, so the last thing dropped is
/// the first thing restored. The previous "pick the highest knob" rule could
/// ping-pong between two knobs that happened to sit at similar levels.
constexpr QualityKnob kSpendOrder[] = {
    QualityKnob::Particles,        QualityKnob::EntityDistance,
    QualityKnob::AnimationDistance, QualityKnob::EntityCulling,
    QualityKnob::LightingUpdates,  QualityKnob::RenderDistance,
};

}  // namespace

RenderSettings QualityProfile::ToSettings(const QualityLevel& q,
                                          i32 targetFps) const {
  RenderSettings s;
  s.targetFps = targetFps;
  const i32 floorDist = minRenderDistance > 0 ? minRenderDistance : 1;
  s.renderDistanceChunks =
      std::max(floorDist, LerpI(renderDistance.minValue, renderDistance.maxValue,
                                q.Get(QualityKnob::RenderDistance)));
  s.maxParticles =
      std::max(0, LerpI(particles.minValue, particles.maxValue,
                        q.Get(QualityKnob::Particles)));
  s.entityDistance = std::max(4.0f, Lerp(entityDistance.minValue,
                                         entityDistance.maxValue,
                                         q.Get(QualityKnob::EntityDistance)));
  s.animationDistance =
      std::max(4.0f, Lerp(animationDistance.minValue, animationDistance.maxValue,
                          q.Get(QualityKnob::AnimationDistance)));
  s.cullFarEntities = q.Get(QualityKnob::EntityCulling) < cullThreshold;
  s.lightingUpdatesPerTick =
      std::max(1, LerpI(lightingUpdates.minValue, lightingUpdates.maxValue,
                        q.Get(QualityKnob::LightingUpdates)));
  return s;
}

void PerformanceGovernor::Configure(const GovernorConfig& cfg) {
  cfg_ = cfg;
  if (cfg_.windowFrames < 2) cfg_.windowFrames = 2;
  if (cfg_.stepSize <= 0.0f) cfg_.stepSize = 0.01f;
  if (cfg_.stepSize > 1.0f) cfg_.stepSize = 1.0f;
  if (cfg_.framesToReact < 1) cfg_.framesToReact = 1;
  if (cfg_.floorQuality < 0.0f) cfg_.floorQuality = 0.0f;
  if (cfg_.floorQuality > 1.0f) cfg_.floorQuality = 1.0f;
  if (cfg_.thermalCeiling < cfg_.floorQuality) {
    cfg_.thermalCeiling = cfg_.floorQuality;
  }
  if (cfg_.fairCeiling < cfg_.thermalCeiling) {
    cfg_.fairCeiling = cfg_.thermalCeiling;
  }
  if (cfg_.fairCeiling > 1.0f) cfg_.fairCeiling = 1.0f;
  if (cfg_.thermalDwellFrames < 0) cfg_.thermalDwellFrames = 0;
  while (frameMs_.size() > cfg_.windowFrames) frameMs_.pop_front();
}

void PerformanceGovernor::PushFrame(f32 frameMs) {
  // Reject nonsense so one bad sample cannot move the governor.
  if (!(frameMs > 0.0f) || !std::isfinite(frameMs)) return;
  if (frameMs > 10000.0f) frameMs = 10000.0f;
  frameMs_.push_back(frameMs);
  while (frameMs_.size() > cfg_.windowFrames) frameMs_.pop_front();
}

FrameStats PerformanceGovernor::stats() const {
  FrameStats st;
  if (frameMs_.empty()) return st;
  std::vector<f32> sorted(frameMs_.begin(), frameMs_.end());
  std::sort(sorted.begin(), sorted.end());
  f64 sum = 0;
  for (f32 v : sorted) sum += v;
  st.samples = sorted.size();
  st.avgMs = sum / (f64)sorted.size();
  st.minMs = sorted.front();
  st.maxMs = sorted.back();
  // Nearest-rank percentiles: index ceil(p * n) - 1, clamped into range.
  auto percentile = [&sorted](f64 p) -> f64 {
    size_t idx = (size_t)std::ceil(p * (f64)sorted.size());
    if (idx > 0) --idx;
    if (idx >= sorted.size()) idx = sorted.size() - 1;
    return sorted[idx];
  };
  st.p95Ms = percentile(0.95);
  st.p99Ms = percentile(0.99);
  return st;
}

FrameHistogram PerformanceGovernor::histogram() const {
  FrameHistogram h;
  for (f32 v : frameMs_) h.Add((f64)v);
  return h;
}

void PerformanceGovernor::SetCeiling(f32 ceiling) {
  ceiling_ = std::clamp(ceiling, 0.0f, 1.0f);
  // A lower ceiling immediately pulls quality down to it.
  if (ClampQualityTo(ceiling_)) dirty_ = true;
}

void PerformanceGovernor::SetPlatform(PlatformClass c) {
  platform_ = c;
  profile_ = QualityProfile::ForPlatform(c);
  // A tighter device gets a tighter thermal ceiling: a phone that throttles
  // hard must be clamped sooner than a desktop that barely warms up.
  switch (c) {
    case PlatformClass::MobileLow: cfg_.thermalCeiling = 0.35f; break;
    case PlatformClass::MobileHigh: cfg_.thermalCeiling = 0.45f; break;
    case PlatformClass::Desktop: cfg_.thermalCeiling = 0.6f; break;
    case PlatformClass::Generic: break;
  }
  if (cfg_.thermalCeiling < cfg_.floorQuality) {
    cfg_.thermalCeiling = cfg_.floorQuality;
  }
  if (cfg_.fairCeiling < cfg_.thermalCeiling) {
    cfg_.fairCeiling = cfg_.thermalCeiling;
  }
}

bool PerformanceGovernor::ClampQualityTo(f32 cap) {
  bool changed = false;
  for (size_t i = 0; i < quality_.v.size(); ++i) {
    if (quality_.v[i] > cap) {
      quality_.v[i] = cap;
      changed = true;
    }
  }
  return changed;
}

void PerformanceGovernor::SetQuality(const QualityLevel& q) {
  if (quality_.Equals(q)) return;
  quality_ = q;
  dirty_ = true;
}

void PerformanceGovernor::Reset() {
  frameMs_.clear();
  quality_ = QualityLevel{};
  ceiling_ = 1.0f;
  thermal_ = ThermalState::Nominal;
  thermalHold_ = 1.0f;
  dwellFrames_ = 0;
  slowStreak_ = 0;
  fastStreak_ = 0;
  tailStreak_ = 0;
  dirty_ = true;
}

GovernorAction PerformanceGovernor::Step(f32 direction) {
  // The effective ceiling is the tighter of the user ceiling and any thermal
  // hold, so a hot device is never allowed to climb back to full quality.
  const f32 ceiling = std::min(ceiling_, thermalHold_);
  bool changed = false;

  // Spend the quality budget in a fixed order (particles first, render distance
  // last) rather than always taking from whichever knob is highest. A fixed
  // order means the cheap knobs are exhausted before an expensive one is
  // touched, and — because restore walks the order backwards — the last thing
  // dropped is the first thing restored, so the two cannot ping-pong.
  if (direction < 0.0f) {
    // Reduce: take the step from the first knob in spend order that is still
    // above the floor.
    for (QualityKnob k : kSpendOrder) {
      const f32 cur = quality_.Get(k);
      if (cur > cfg_.floorQuality + 1e-6f) {
        const f32 next = std::max(cfg_.floorQuality, cur - cfg_.stepSize);
        if (next < cur) {
          quality_.Set(k, next);
          changed = true;
          break;
        }
      }
    }
    if (!changed) {
      // Every knob is at the floor: nothing left to give.
      return GovernorAction::None;
    }
    dirty_ = true;
    return GovernorAction::Reduced;
  }

  // Restore: raise the last knob in spend order that is still below the
  // ceiling, so restoration retraces the reduction path in reverse.
  for (size_t i = sizeof(kSpendOrder) / sizeof(kSpendOrder[0]); i-- > 0;) {
    const QualityKnob k = kSpendOrder[i];
    const f32 cur = quality_.Get(k);
    if (cur < ceiling - 1e-6f) {
      const f32 next = std::min(ceiling, cur + cfg_.stepSize);
      if (next > cur) {
        quality_.Set(k, next);
        changed = true;
        break;
      }
    }
  }
  if (!changed) return GovernorAction::None;
  dirty_ = true;
  return GovernorAction::Restored;
}

GovernorAction PerformanceGovernor::OnFrame(f32 frameMs) {
  PushFrame(frameMs);

  // A pending thermal dwell blocks restoration but not reduction: cooling
  // down is always allowed to reduce further, never to climb back early.
  if (dwellFrames_ > 0) --dwellFrames_;

  // Not enough history to judge: never react on the first few frames.
  if (frameMs_.size() < (size_t)cfg_.framesToReact) return GovernorAction::None;

  const f64 targetMs = 1000.0 / (f64)cfg_.targetFps;
  const f64 slowThreshold = targetMs * (1.0 + cfg_.slowMargin);
  const f64 fastThreshold = targetMs * (1.0 - cfg_.fastMargin);

  // Use the window average, not the instantaneous frame, so a single spike
  // (a chunk upload, a GC pause) does not move the governor.
  const FrameStats st = stats();
  const f64 avg = st.avgMs;

  // A smooth average can still hide a spiky tail. When tail-latency reaction
  // is on, a bad p99 counts as "slow" even if the average is inside the band,
  // so the governor reacts to the stutter the player actually feels. The tail
  // streak is separate from the average streak: the fast frame that follows a
  // spike must not reset the very signal the spike created.
  if (cfg_.useTailLatency &&
      frameMs_.size() >= cfg_.tailLatencyMinSamples) {
    if (st.p99Ms > slowThreshold) {
      ++tailStreak_;
    } else {
      tailStreak_ = 0;
    }
  } else {
    tailStreak_ = 0;
  }

  if (avg > slowThreshold || tailStreak_ >= cfg_.framesToReact) {
    ++slowStreak_;
    fastStreak_ = 0;
  } else if (avg < fastThreshold) {
    ++fastStreak_;
    slowStreak_ = 0;
  } else {
    // Inside the dead band: hold. Reset the average streaks so jitter never
    // accumulates into an action. The tail streak is left alone on purpose: it
    // is the memory that a spike happened.
    slowStreak_ = 0;
    fastStreak_ = 0;
    return GovernorAction::None;
  }

  if (slowStreak_ >= cfg_.framesToReact || tailStreak_ >= cfg_.framesToReact) {
    slowStreak_ = 0;
    tailStreak_ = 0;
    return Step(-1.0f);
  }
  if (fastStreak_ >= cfg_.framesToReact * 2) {
    // Restore more slowly than we reduce: quality regressions are more
    // noticeable than quality improvements.
    fastStreak_ = 0;
    // Never restore while thermally limited, and never during the dwell that
    // follows a clamp: a device that just throttled reports fast frames for a
    // moment, and restoring then would heat it straight back up.
    if (thermallyLimited() || dwellFrames_ > 0) return GovernorAction::None;
    return Step(+1.0f);
  }
  return GovernorAction::None;
}

GovernorAction PerformanceGovernor::OnThermal(ThermalState state) {
  thermal_ = state;

  if (thermallyLimited()) {
    thermalHold_ = cfg_.thermalCeiling;
    // Immediately drop to the thermal ceiling rather than waiting for frames.
    const bool changed = ClampQualityTo(std::min(ceiling_, thermalHold_));
    dirty_ = dirty_ || changed;
    // Arm the dwell on every clamp: restoration is blocked for a while even
    // if the device reports fast frames right after.
    dwellFrames_ = cfg_.thermalDwellFrames;
    return changed ? GovernorAction::ThermalClamp : GovernorAction::None;
  }

  // Cooling down: release the hold. Nominal lifts it entirely; Fair keeps a
  // mild cap so a warm device does not immediately bounce back to full load.
  if (state == ThermalState::Fair) {
    thermalHold_ = std::min(1.0f, cfg_.fairCeiling);
    // Fair is "warm, not throttling": clamp to the Fair ceiling now so the
    // device is kept out of the Serious band instead of waiting for it. This
    // also arms the dwell, so quality does not spring straight back up.
    const bool changed = ClampQualityTo(std::min(ceiling_, thermalHold_));
    dirty_ = dirty_ || changed;
    if (changed) dwellFrames_ = cfg_.thermalDwellFrames;
    return changed ? GovernorAction::ThermalClamp : GovernorAction::None;
  }
  thermalHold_ = 1.0f;
  // The ceiling change alone does not restore quality; the normal OnFrame
  // feedback does that gradually.
  return GovernorAction::None;
}

}  // namespace Berry
