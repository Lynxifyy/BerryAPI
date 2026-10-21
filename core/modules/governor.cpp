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

QualityProfile QualityProfile::Default() { return QualityProfile{}; }

namespace {

f32 Lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

i32 LerpI(f32 a, f32 b, f32 t) {
  const f32 v = Lerp(a, b, t);
  return (i32)std::lround(v);
}

}  // namespace

RenderSettings QualityProfile::ToSettings(const QualityLevel& q,
                                          i32 targetFps) const {
  RenderSettings s;
  s.targetFps = targetFps;
  s.renderDistanceChunks =
      std::max(2, LerpI(renderDistance.minValue, renderDistance.maxValue,
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
  // Nearest-rank p95: index ceil(0.95 * n) - 1, clamped into range.
  size_t idx = (size_t)std::ceil(0.95 * (f64)sorted.size());
  if (idx > 0) --idx;
  if (idx >= sorted.size()) idx = sorted.size() - 1;
  st.p95Ms = sorted[idx];
  return st;
}

void PerformanceGovernor::SetCeiling(f32 ceiling) {
  ceiling_ = std::clamp(ceiling, 0.0f, 1.0f);
  // A lower ceiling immediately pulls quality down to it.
  for (size_t i = 0; i < quality_.v.size(); ++i) {
    if (quality_.v[i] > ceiling_) {
      quality_.v[i] = ceiling_;
      dirty_ = true;
    }
  }
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
  slowStreak_ = 0;
  fastStreak_ = 0;
  dirty_ = true;
}

GovernorAction PerformanceGovernor::Step(f32 direction) {
  // The effective ceiling is the tighter of the user ceiling and any thermal
  // hold, so a hot device is never allowed to climb back to full quality.
  const f32 ceiling = std::min(ceiling_, thermalHold_);
  bool changed = false;

  // Spend the quality budget in the order that costs the least visually:
  // particles first, render distance last. `direction` is -1 to reduce and +1
  // to restore.
  if (direction < 0.0f) {
    // Reduce: take the step from the single highest knob that is still above
    // its own floor. One knob per action keeps the visual change small.
    f32 best = -1.0f;
    size_t bestIdx = 0;
    for (size_t i = 0; i < quality_.v.size(); ++i) {
      if (quality_.v[i] > best) {
        best = quality_.v[i];
        bestIdx = i;
      }
    }
    if (best > cfg_.floorQuality + 1e-6f) {
      const f32 next = std::max(cfg_.floorQuality, best - cfg_.stepSize);
      if (next < best) {
        quality_.v[bestIdx] = next;
        changed = true;
      }
    }
    if (!changed) {
      // Every knob is at the floor: nothing left to give.
      return GovernorAction::None;
    }
    dirty_ = true;
    return GovernorAction::Reduced;
  }

  // Restore: raise the lowest knob, but never above the ceiling.
  f32 worst = 2.0f;
  size_t worstIdx = 0;
  for (size_t i = 0; i < quality_.v.size(); ++i) {
    if (quality_.v[i] < worst) {
      worst = quality_.v[i];
      worstIdx = i;
    }
  }
  if (worst >= ceiling - 1e-6f) return GovernorAction::None;
  const f32 next = std::min(ceiling, worst + cfg_.stepSize);
  if (next > worst) {
    quality_.v[worstIdx] = next;
    changed = true;
  }
  if (!changed) return GovernorAction::None;
  dirty_ = true;
  return GovernorAction::Restored;
}

GovernorAction PerformanceGovernor::OnFrame(f32 frameMs) {
  PushFrame(frameMs);

  // Not enough history to judge: never react on the first few frames.
  if (frameMs_.size() < (size_t)cfg_.framesToReact) return GovernorAction::None;

  const f64 targetMs = 1000.0 / (f64)cfg_.targetFps;
  const f64 slowThreshold = targetMs * (1.0 + cfg_.slowMargin);
  const f64 fastThreshold = targetMs * (1.0 - cfg_.fastMargin);

  // Use the window average, not the instantaneous frame, so a single spike
  // (a chunk upload, a GC pause) does not move the governor.
  const FrameStats st = stats();
  const f64 avg = st.avgMs;

  if (avg > slowThreshold) {
    ++slowStreak_;
    fastStreak_ = 0;
  } else if (avg < fastThreshold) {
    ++fastStreak_;
    slowStreak_ = 0;
  } else {
    // Inside the dead band: hold. Reset both streaks so jitter never
    // accumulates into an action.
    slowStreak_ = 0;
    fastStreak_ = 0;
    return GovernorAction::None;
  }

  if (slowStreak_ >= cfg_.framesToReact) {
    slowStreak_ = 0;
    return Step(-1.0f);
  }
  if (fastStreak_ >= cfg_.framesToReact * 2) {
    // Restore more slowly than we reduce: quality regressions are more
    // noticeable than quality improvements.
    fastStreak_ = 0;
    if (thermallyLimited()) return GovernorAction::None;
    return Step(+1.0f);
  }
  return GovernorAction::None;
}

GovernorAction PerformanceGovernor::OnThermal(ThermalState state) {
  const ThermalState prev = thermal_;
  thermal_ = state;

  if (thermallyLimited()) {
    thermalHold_ = cfg_.thermalCeiling;
    // Immediately drop to the thermal ceiling rather than waiting for frames.
    bool changed = false;
    for (size_t i = 0; i < quality_.v.size(); ++i) {
      const f32 cap = std::min(ceiling_, thermalHold_);
      if (quality_.v[i] > cap) {
        quality_.v[i] = cap;
        changed = true;
      }
    }
    dirty_ = dirty_ || changed;
    return changed ? GovernorAction::ThermalClamp : GovernorAction::None;
  }

  // Cooling down: release the hold. Nominal lifts it entirely; Fair keeps a
  // mild cap so a warm device does not immediately bounce back to full load.
  if (state == ThermalState::Fair) {
    thermalHold_ = std::min(1.0f, cfg_.thermalCeiling + 0.3f);
  } else {
    thermalHold_ = 1.0f;
  }
  // The ceiling change alone does not restore quality; the normal OnFrame
  // feedback does that gradually.
  (void)prev;
  return GovernorAction::None;
}

}  // namespace Berry
