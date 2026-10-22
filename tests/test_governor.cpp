// Host tests for the adaptive FPS governor and its cull policy.
//
// Covers:
//   * QualityLevel clamping, equality and mean.
//   * QualityProfile -> RenderSettings mapping, including the cull threshold.
//   * FrameStats rolling average and p95.
//   * The feedback loop: reduction on sustained slow frames, restoration on
//     sustained fast frames, and no action inside the dead band.
//   * Hysteresis: a single slow frame is ignored; restore is slower than
//     reduce.
//   * Floors, user ceilings and thermal clamping, including the "a hot device
//     must not climb back up" rule.
//   * CullPolicy decisions.
#include "berry/perf.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static bool Near(f32 a, f32 b, f32 eps = 1e-3f) {
  return std::fabs(a - b) <= eps;
}

static GovernorConfig TestConfig() {
  GovernorConfig cfg;
  cfg.targetFps = 60;         // 16.67 ms target
  cfg.slowMargin = 0.10f;     // slow above ~18.3 ms
  cfg.fastMargin = 0.15f;     // fast below ~14.2 ms
  cfg.framesToReact = 4;
  cfg.stepSize = 0.1f;
  cfg.windowFrames = 16;
  cfg.thermalCeiling = 0.4f;
  cfg.floorQuality = 0.2f;
  return cfg;
}

static void TestQualityLevel() {
  QualityLevel q;
  for (size_t i = 0; i < (size_t)QualityKnob::Count; ++i) {
    CHECK(Near(q.Get((QualityKnob)i), 1.0f));
  }
  CHECK(Near(q.Mean(), 1.0f));

  q.Set(QualityKnob::Particles, 2.0f);   // clamps high
  CHECK(Near(q.Get(QualityKnob::Particles), 1.0f));
  q.Set(QualityKnob::Particles, -1.0f);  // clamps low
  CHECK(Near(q.Get(QualityKnob::Particles), 0.0f));

  QualityLevel other = q;
  CHECK(q.Equals(other));
  other.Set(QualityKnob::RenderDistance, 0.5f);
  CHECK(!q.Equals(other));
  CHECK(q.Mean() < 1.0f);
}

static void TestKnobNames() {
  CHECK(std::string(QualityKnobName(QualityKnob::Particles)) == "particles");
  CHECK(std::string(QualityKnobName(QualityKnob::RenderDistance)) ==
        "render-distance");
  CHECK(std::string(ThermalStateName(ThermalState::Critical)) == "critical");
  CHECK(std::string(GovernorActionName(GovernorAction::ThermalClamp)) ==
        "thermal-clamp");
}

static void TestProfileMapping() {
  QualityProfile p = QualityProfile::Default();
  QualityLevel full;   // all 1.0
  RenderSettings hi = p.ToSettings(full, 60);
  CHECK(hi.renderDistanceChunks == 12);
  CHECK(hi.maxParticles == 4000);
  CHECK(Near(hi.entityDistance, 64.0f));
  CHECK(Near(hi.animationDistance, 32.0f));
  CHECK(hi.lightingUpdatesPerTick == 8);
  CHECK(!hi.cullFarEntities);           // quality 1.0 is above the threshold
  CHECK(hi.targetFps == 60);

  QualityLevel none;
  for (size_t i = 0; i < (size_t)QualityKnob::Count; ++i) {
    none.Set((QualityKnob)i, 0.0f);
  }
  RenderSettings lo = p.ToSettings(none, 30);
  CHECK(lo.renderDistanceChunks == 4);
  CHECK(lo.maxParticles == 500);
  CHECK(Near(lo.entityDistance, 24.0f));
  CHECK(Near(lo.animationDistance, 16.0f));
  CHECK(lo.lightingUpdatesPerTick == 1);
  CHECK(lo.cullFarEntities);            // quality 0.0 is below the threshold
  CHECK(lo.targetFps == 30);

  // Culling flips exactly at the threshold.
  QualityLevel at;
  for (size_t i = 0; i < (size_t)QualityKnob::Count; ++i) {
    at.Set((QualityKnob)i, p.cullThreshold);
  }
  CHECK(!p.ToSettings(at, 60).cullFarEntities);   // 0.5 is not < 0.5
  at.Set(QualityKnob::EntityCulling, p.cullThreshold - 0.01f);
  CHECK(p.ToSettings(at, 60).cullFarEntities);

  // Settings round-trip for equality.
  CHECK(hi == p.ToSettings(full, 60));
  CHECK(!(hi == lo));
}

static void TestFrameStats() {
  PerformanceGovernor g;
  GovernorConfig cfg = TestConfig();
  cfg.windowFrames = 100;
  g.Configure(cfg);

  CHECK(g.stats().samples == 0);
  CHECK(Near((f32)g.stats().Fps(), 0.0f));

  // Ten frames from 10 to 19 ms: average 14.5, min 10, max 19.
  for (int i = 0; i < 10; ++i) g.OnFrame(10.0f + (f32)i);
  FrameStats st = g.stats();
  CHECK(st.samples == 10);
  CHECK(Near((f32)st.avgMs, 14.5f, 0.01f));
  CHECK(Near((f32)st.minMs, 10.0f));
  CHECK(Near((f32)st.maxMs, 19.0f));
  CHECK(st.p95Ms >= st.avgMs);
  CHECK(st.p95Ms <= st.maxMs);
  CHECK(st.Fps() > 0);

  // Bad samples are rejected: they must not enter the window.
  const size_t before = g.frameHistory().size();
  g.OnFrame(0.0f);
  g.OnFrame(-5.0f);
  g.OnFrame(std::nanf(""));
  CHECK(g.frameHistory().size() == before);

  // A huge spike is clamped rather than poisoning the average.
  g.OnFrame(1e9f);
  CHECK(g.frameHistory().back() <= 10000.0f);
}

static void TestReduceOnSlowFrames() {
  PerformanceGovernor g;
  g.Configure(TestConfig());
  CHECK(Near(g.quality().Mean(), 1.0f));

  // One slow frame is noise: no action.
  CHECK(g.OnFrame(40.0f) == GovernorAction::None);

  // Sustained slow frames drive quality down one knob per action.
  GovernorAction last = GovernorAction::None;
  for (int i = 0; i < 40; ++i) {
    GovernorAction a = g.OnFrame(40.0f);
    if (a != GovernorAction::None) last = a;
  }
  CHECK(last == GovernorAction::Reduced);
  CHECK(g.quality().Mean() < 1.0f);
  CHECK(g.settingsDirty());
  g.ClearDirty();
  CHECK(!g.settingsDirty());

  // The render settings followed quality down — but with the priority order
  // the cheap knobs absorb the cut first, so render distance is still full.
  CHECK(g.quality().Get(QualityKnob::Particles) <= 0.2f + 1e-4f);
  CHECK(Near(g.quality().Get(QualityKnob::RenderDistance), 1.0f));
  CHECK(g.settings().renderDistanceChunks == 12);
}

static void TestRestoreOnFastFrames() {
  PerformanceGovernor g;
  g.Configure(TestConfig());

  // Drive quality down first.
  for (int i = 0; i < 80; ++i) g.OnFrame(40.0f);
  const f32 lowered = g.quality().Mean();
  CHECK(lowered < 1.0f);

  // Now run fast frames. Restore needs twice the streak, so it is slower.
  bool restored = false;
  for (int i = 0; i < 400; ++i) {
    if (g.OnFrame(8.0f) == GovernorAction::Restored) restored = true;
  }
  CHECK(restored);
  CHECK(g.quality().Mean() > lowered);
  CHECK(Near(g.quality().Mean(), 1.0f));   // fully recovered when it can
}

static void TestDeadBandHolds() {
  PerformanceGovernor g;
  g.Configure(TestConfig());
  // 16.67 ms is exactly the target: inside the dead band forever.
  for (int i = 0; i < 200; ++i) {
    CHECK(g.OnFrame(16.67f) == GovernorAction::None);
  }
  CHECK(Near(g.quality().Mean(), 1.0f));
}

static void TestFloorQuality() {
  PerformanceGovernor g;
  GovernorConfig cfg = TestConfig();
  cfg.floorQuality = 0.5f;
  g.Configure(cfg);

  for (int i = 0; i < 500; ++i) g.OnFrame(60.0f);
  // Never drops below the floor.
  for (size_t i = 0; i < (size_t)QualityKnob::Count; ++i) {
    CHECK(g.quality().Get((QualityKnob)i) >= 0.5f - 1e-4f);
  }
  // Once every knob is at the floor there is nothing left to give.
  CHECK(g.OnFrame(60.0f) == GovernorAction::None);
}

static void TestUserCeiling() {
  PerformanceGovernor g;
  g.Configure(TestConfig());
  g.SetCeiling(0.6f);
  // The ceiling immediately pulls quality down.
  for (size_t i = 0; i < (size_t)QualityKnob::Count; ++i) {
    CHECK(g.quality().Get((QualityKnob)i) <= 0.6f + 1e-4f);
  }
  CHECK(Near(g.ceiling(), 0.6f));

  // Fast frames cannot restore past the ceiling.
  for (int i = 0; i < 400; ++i) g.OnFrame(5.0f);
  for (size_t i = 0; i < (size_t)QualityKnob::Count; ++i) {
    CHECK(g.quality().Get((QualityKnob)i) <= 0.6f + 1e-4f);
  }
}

static void TestThermalClamp() {
  PerformanceGovernor g;
  g.Configure(TestConfig());
  CHECK(!g.thermallyLimited());

  // Fair is not "limited", but it IS warm: the governor clamps to the Fair
  // ceiling now rather than waiting for the device to reach Serious, and arms
  // the dwell so quality cannot spring straight back up.
  CHECK(g.OnThermal(ThermalState::Fair) == GovernorAction::ThermalClamp);
  CHECK(!g.thermallyLimited());
  for (size_t i = 0; i < (size_t)QualityKnob::Count; ++i) {
    CHECK(g.quality().Get((QualityKnob)i) <= g.config().fairCeiling + 1e-4f);
  }

  // Back to nominal: the hold is released, but the dwell still blocks an
  // immediate restore.
  g.OnThermal(ThermalState::Nominal);
  CHECK(g.OnFrame(5.0f) != GovernorAction::Restored);

  // Serious clamps quality down to the thermal ceiling immediately.
  CHECK(g.OnThermal(ThermalState::Serious) == GovernorAction::ThermalClamp);
  CHECK(g.thermallyLimited());
  for (size_t i = 0; i < (size_t)QualityKnob::Count; ++i) {
    CHECK(g.quality().Get((QualityKnob)i) <= 0.4f + 1e-4f);
  }

  // While hot, fast frames must NOT restore quality: a device that is already
  // throttling would only get hotter for no frame-rate gain.
  const f32 clamped = g.quality().Mean();
  for (int i = 0; i < 400; ++i) {
    const GovernorAction a = g.OnFrame(5.0f);
    CHECK(a != GovernorAction::Restored);
  }
  CHECK(Near(g.quality().Mean(), clamped));

  // Critical keeps the clamp.
  CHECK(g.OnThermal(ThermalState::Critical) == GovernorAction::None);
  CHECK(g.thermallyLimited());

  // Cooling to nominal releases the hold and allows restoration again.
  g.OnThermal(ThermalState::Nominal);
  CHECK(!g.thermallyLimited());
  bool restored = false;
  for (int i = 0; i < 400; ++i) {
    if (g.OnFrame(5.0f) == GovernorAction::Restored) restored = true;
  }
  CHECK(restored);
  CHECK(g.quality().Mean() > clamped);
}

static void TestThermalDwellBlocksEarlyRestore() {
  PerformanceGovernor g;
  GovernorConfig cfg = TestConfig();
  cfg.thermalDwellFrames = 200;
  g.Configure(cfg);

  g.OnThermal(ThermalState::Serious);
  g.OnThermal(ThermalState::Nominal);
  // The clamp armed the dwell, so even a long run of fast frames cannot
  // restore until it expires. This is the anti-oscillation rule.
  for (int i = 0; i < 150; ++i) {
    CHECK(g.OnFrame(5.0f) != GovernorAction::Restored);
  }
  // Past the dwell, restoration resumes.
  bool restored = false;
  for (int i = 0; i < 400; ++i) {
    if (g.OnFrame(5.0f) == GovernorAction::Restored) restored = true;
  }
  CHECK(restored);
}

static void TestSpendOrderIsPriorityOrdered() {
  PerformanceGovernor g;
  GovernorConfig cfg = TestConfig();
  cfg.floorQuality = 0.0f;
  cfg.stepSize = 0.25f;
  cfg.framesToReact = 1;
  g.Configure(cfg);

  // Each reduction must take from particles before anything else, so after
  // four steps particles has hit the floor and no other knob has moved yet.
  for (int i = 0; i < 4; ++i) {
    CHECK(g.OnFrame(60.0f) == GovernorAction::Reduced);
  }
  CHECK(Near(g.quality().Get(QualityKnob::Particles), 0.0f));
  // Render distance and entity distance are untouched: the cheap knob absorbed
  // the whole cut, which is the point of the priority order.
  CHECK(Near(g.quality().Get(QualityKnob::RenderDistance), 1.0f));
  CHECK(Near(g.quality().Get(QualityKnob::EntityDistance), 1.0f));

  // The next reduction finally reaches entity distance.
  CHECK(g.OnFrame(60.0f) == GovernorAction::Reduced);
  CHECK(g.quality().Get(QualityKnob::EntityDistance) < 1.0f);
  CHECK(Near(g.quality().Get(QualityKnob::RenderDistance), 1.0f));
}

static void TestRestoreReversesSpendOrder() {
  PerformanceGovernor g;
  GovernorConfig cfg = TestConfig();
  cfg.floorQuality = 0.0f;
  cfg.stepSize = 0.25f;
  cfg.framesToReact = 1;
  g.Configure(cfg);

  // Reduce once: particles drops, render distance stays full.
  CHECK(g.OnFrame(60.0f) == GovernorAction::Reduced);
  CHECK(g.quality().Get(QualityKnob::Particles) < 1.0f);
  CHECK(Near(g.quality().Get(QualityKnob::RenderDistance), 1.0f));

  // Restore must give back the most visually important knob first, i.e. the
  // last one in spend order that is below the ceiling. Since only particles
  // moved, particles is restored.
  bool restored = false;
  for (int i = 0; i < 20; ++i) {
    if (g.OnFrame(5.0f) == GovernorAction::Restored) restored = true;
  }
  CHECK(restored);
  CHECK(Near(g.quality().Get(QualityKnob::Particles), 1.0f));
}

static void TestTailLatencyReacts() {
  PerformanceGovernor g;
  GovernorConfig cfg = TestConfig();
  cfg.windowFrames = 100;
  cfg.framesToReact = 2;
  cfg.useTailLatency = true;
  cfg.tailLatencyMinSamples = 30;
  g.Configure(cfg);

  // Nine in-band frames then one 40 ms stutter, repeated. The spike is at the
  // END of each period, so the window average never leaves the dead band
  // (15.0 -> 17.5 ms); only the p99 tail can move the governor.
  auto stream = [](int i) { return (i % 10 == 9) ? 40.0f : 15.0f; };
  bool reduced = false;
  for (int i = 0; i < 300; ++i) {
    if (g.OnFrame(stream(i)) == GovernorAction::Reduced) reduced = true;
  }
  CHECK(reduced);

  // With tail-latency reaction off, the identical stream stays in the band.
  PerformanceGovernor g2;
  cfg.useTailLatency = false;
  g2.Configure(cfg);
  bool reduced2 = false;
  for (int i = 0; i < 300; ++i) {
    if (g2.OnFrame(stream(i)) == GovernorAction::Reduced) reduced2 = true;
  }
  CHECK(!reduced2);
}

static void TestTailLatencyNeedsEnoughSamples() {
  // In a short window p99 IS the maximum, so a single spike must not trigger a
  // reduction: that would defeat the hysteresis entirely. Same stream as the
  // test above, but the window is too short for the p99 signal to be trusted.
  auto stream = [](int i) { return (i % 10 == 9) ? 40.0f : 15.0f; };

  PerformanceGovernor g;
  GovernorConfig cfg = TestConfig();
  cfg.windowFrames = 10;              // smaller than the min-sample gate
  cfg.framesToReact = 2;
  cfg.useTailLatency = true;
  cfg.tailLatencyMinSamples = 30;
  g.Configure(cfg);
  bool reduced = false;
  for (int i = 0; i < 300; ++i) {
    if (g.OnFrame(stream(i)) == GovernorAction::Reduced) reduced = true;
  }
  CHECK(!reduced);

  // With the gate lowered, the same short-window stream does react — which
  // proves the difference is the gate, not the stream.
  PerformanceGovernor g2;
  cfg.tailLatencyMinSamples = 2;
  g2.Configure(cfg);
  bool reduced2 = false;
  for (int i = 0; i < 300; ++i) {
    if (g2.OnFrame(stream(i)) == GovernorAction::Reduced) reduced2 = true;
  }
  CHECK(reduced2);
}

static void TestFrameHistogram() {
  FrameHistogram h;
  CHECK(h.total == 0);
  h.Add(4.0);
  h.Add(10.0);
  h.Add(20.0);
  h.Add(1000.0);          // catch-all tail bucket
  CHECK(h.total == 4);
  CHECK(h.counts[FrameHistogram::BucketFor(4.0)] == 1);
  CHECK(h.counts[FrameHistogram::BucketFor(10.0)] == 1);
  CHECK(h.counts[FrameHistogram::BucketFor(20.0)] == 1);
  CHECK(h.counts[FrameHistogram::kBucketCount - 1] == 1);
  // Sum of buckets equals the total.
  u64 sum = 0;
  for (u64 c : h.counts) sum += c;
  CHECK(sum == h.total);
  // Bad samples are dropped, not bucketed.
  h.Add(-1.0);
  h.Add(std::nan(""));
  CHECK(h.total == 4);
  h.Clear();
  CHECK(h.total == 0);

  // The governor's histogram reflects its window.
  PerformanceGovernor g;
  g.Configure(TestConfig());
  for (int i = 0; i < 10; ++i) g.OnFrame(16.0f);
  CHECK(g.histogram().total == 10);
}

static void TestPlatformProfiles() {
  CHECK(std::string(PlatformClassName(PlatformClass::MobileLow)) == "mobile-low");
  CHECK(std::string(PlatformClassName(PlatformClass::Desktop)) == "desktop");

  QualityProfile low = QualityProfile::ForPlatform(PlatformClass::MobileLow);
  QualityProfile desk = QualityProfile::ForPlatform(PlatformClass::Desktop);
  // A desktop can afford more chunks at full quality and has a higher floor.
  CHECK(desk.renderDistance.maxValue > low.renderDistance.maxValue);
  CHECK(desk.minRenderDistance >= low.minRenderDistance);

  // The low-end profile never picks a render distance below its floor.
  QualityLevel none;
  for (size_t i = 0; i < (size_t)QualityKnob::Count; ++i) {
    none.Set((QualityKnob)i, 0.0f);
  }
  RenderSettings ls = low.ToSettings(none, 60);
  CHECK(ls.renderDistanceChunks == low.minRenderDistance);

  // SetPlatform swaps in the matching profile and a tighter thermal ceiling.
  PerformanceGovernor g;
  g.Configure(TestConfig());
  g.SetPlatform(PlatformClass::MobileLow);
  CHECK(g.platform() == PlatformClass::MobileLow);
  CHECK(Near(g.profile().renderDistance.maxValue, 8.0f));
  CHECK(g.config().thermalCeiling <= 0.35f + 1e-4f);
  g.SetPlatform(PlatformClass::Desktop);
  CHECK(g.platform() == PlatformClass::Desktop);
  CHECK(Near(g.profile().renderDistance.maxValue, 16.0f));
}

static void TestParticleClassCaps() {
  CullPolicy cp;
  cp.settings.entityDistance = 32.0f;
  cp.settings.maxParticles = 1000;

  // A zero class cap disables that class even when the global cap allows it.
  cp.classCaps[(size_t)CullPolicy::ParticleClass::Ambient] = 0;
  CHECK(!cp.AllowParticleClass(CullPolicy::ParticleClass::Ambient, 0));
  CHECK(cp.AllowParticleClass(CullPolicy::ParticleClass::Effect, 0));

  // A class at its own cap is refused while another class is not.
  cp.classCaps[(size_t)CullPolicy::ParticleClass::Ambient] = 10;
  CHECK(cp.AllowParticleClass(CullPolicy::ParticleClass::Ambient, 9));
  CHECK(!cp.AllowParticleClass(CullPolicy::ParticleClass::Ambient, 10));
  CHECK(cp.AllowParticleClass(CullPolicy::ParticleClass::Combat, 10));

  // The global cap still applies to every class.
  cp.settings.maxParticles = 0;
  CHECK(!cp.AllowParticleClass(CullPolicy::ParticleClass::Effect, 0));

  // Detailed decision: distance, class budget and the ambient screen-size test.
  cp.settings.maxParticles = 1000;
  cp.classCaps[(size_t)CullPolicy::ParticleClass::Ambient] = 10;
  CHECK(!cp.AllowParticleDetailed(CullPolicy::ParticleClass::Ambient, 100.0f, 0));
  CHECK(!cp.AllowParticleDetailed(CullPolicy::ParticleClass::Ambient, 10.0f, 10));
  // A tiny ambient particle is culled; a tiny effect particle is not.
  CHECK(!cp.AllowParticleDetailed(CullPolicy::ParticleClass::Ambient, 10.0f, 0,
                                  0.0001f));
  CHECK(cp.AllowParticleDetailed(CullPolicy::ParticleClass::Effect, 10.0f, 0,
                                 0.0001f));
  CHECK(cp.AllowParticleDetailed(CullPolicy::ParticleClass::Ambient, 10.0f, 0,
                                 0.5f));
}

static void TestThermalCeilingRespectsUserCeiling() {
  PerformanceGovernor g;
  GovernorConfig cfg = TestConfig();
  cfg.thermalCeiling = 0.4f;
  g.Configure(cfg);
  g.SetCeiling(0.3f);   // user wants even less than thermal would allow
  g.OnThermal(ThermalState::Serious);
  for (size_t i = 0; i < (size_t)QualityKnob::Count; ++i) {
    CHECK(g.quality().Get((QualityKnob)i) <= 0.3f + 1e-4f);
  }
}

static void TestSetQualityAndReset() {
  PerformanceGovernor g;
  g.Configure(TestConfig());
  QualityLevel q;
  for (size_t i = 0; i < (size_t)QualityKnob::Count; ++i) {
    q.Set((QualityKnob)i, 0.5f);
  }
  g.SetQuality(q);
  CHECK(g.settingsDirty());
  CHECK(Near(g.quality().Mean(), 0.5f));

  // Setting the same level again is not a change.
  g.ClearDirty();
  g.SetQuality(q);
  CHECK(!g.settingsDirty());

  g.Reset();
  CHECK(Near(g.quality().Mean(), 1.0f));
  CHECK(g.stats().samples == 0);
  CHECK(!g.thermallyLimited());
  CHECK(Near(g.ceiling(), 1.0f));
}

static void TestCullPolicy() {
  CullPolicy cp;
  cp.settings.entityDistance = 32.0f;
  cp.settings.animationDistance = 16.0f;
  cp.settings.maxParticles = 100;
  cp.settings.cullFarEntities = false;

  // Culling off: every entity is kept regardless of distance.
  CHECK(cp.KeepEntity(1000.0f));
  CHECK(cp.KeepEntity(0.0f));
  // Animation still respects its own distance.
  CHECK(cp.AnimateEntity(10.0f));
  CHECK(!cp.AnimateEntity(20.0f));

  // Culling on: the boundary is inclusive.
  cp.settings.cullFarEntities = true;
  CHECK(cp.KeepEntity(32.0f));
  CHECK(!cp.KeepEntity(32.1f));

  // Particles: cap plus distance.
  CHECK(cp.AllowParticle(99));
  CHECK(!cp.AllowParticle(100));
  CHECK(cp.AllowParticleAt(10.0f, 0));
  CHECK(!cp.AllowParticleAt(100.0f, 0));
  cp.settings.maxParticles = 0;
  CHECK(!cp.AllowParticle(0));       // a zero cap disables particles entirely
}

static void TestCullPolicyFromGovernor() {
  PerformanceGovernor g;
  GovernorConfig cfg = TestConfig();
  cfg.floorQuality = 0.0f;
  g.Configure(cfg);

  // Drive quality to the bottom so culling engages.
  for (int i = 0; i < 500; ++i) g.OnFrame(80.0f);
  CullPolicy cp;
  cp.settings = g.settings();
  CHECK(cp.settings.cullFarEntities);
  CHECK(cp.settings.renderDistanceChunks == 4);
  CHECK(!cp.KeepEntity(1000.0f));
}

int main() {
  TestQualityLevel();
  TestKnobNames();
  TestProfileMapping();
  TestFrameStats();
  TestReduceOnSlowFrames();
  TestRestoreOnFastFrames();
  TestDeadBandHolds();
  TestFloorQuality();
  TestUserCeiling();
  TestThermalClamp();
  TestThermalDwellBlocksEarlyRestore();
  TestSpendOrderIsPriorityOrdered();
  TestRestoreReversesSpendOrder();
  TestTailLatencyReacts();
  TestTailLatencyNeedsEnoughSamples();
  TestFrameHistogram();
  TestPlatformProfiles();
  TestParticleClassCaps();
  TestThermalCeilingRespectsUserCeiling();
  TestSetQualityAndReset();
  TestCullPolicy();
  TestCullPolicyFromGovernor();
  if (g_fail == 0) printf("test_governor: ALL OK\n");
  return g_fail;
}
