// Host tests for the local combat-state telemetry (CSHR).
//
// These pin the properties the combat tooling relies on, with no device and no
// clocks:
//   * hitbox history is bounded, exact-tick lookups fail closed, and a repeated
//     feed for one tick does not accumulate duplicates,
//   * reach validation measures the real gap and reports valid=false rather than
//     a guessed distance when a position is unknown,
//   * swing timing derives CPS from the tick delta and never invents an interval
//     for the first swing,
//   * projectile extrapolation follows the 20 tps semi-implicit step,
//   * hit-prediction accuracy confirms, misses and never double-counts,
//   * the click budget reports over-budget clicks without dropping them.
#include "berry/combat_state.h"
#include "berry/event.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

EntitySnapshot Snap(u64 raw, EntityType type, f32 x, f32 y, f32 z) {
  EntitySnapshot s;
  s.id.raw = raw;
  s.type = type;
  s.position = Vec3(x, y, z);
  s.alive = true;
  s.health = 20;
  s.maxHealth = 20;
  return s;
}

/// Captures one event channel's payloads so a test can assert the tracker
/// published what it claims.
struct ChannelProbe {
  std::string channel;
  std::vector<std::string> payloads;
  EventBus::ListenerId id = 0;

  explicit ChannelProbe(const char* ch) : channel(ch) {
    id = EventBus::Instance().On(channel.c_str(), [this](const EventContext& e) {
      if (e.data) payloads.push_back(*(const char**)e.data);
      return EventResult::Continue;
    });
  }
  ~ChannelProbe() { EventBus::Instance().Off(channel.c_str(), id); }
};

}  // namespace

//----------------------------------------------------------------------------
// Hitbox history
//----------------------------------------------------------------------------

static void TestHitboxHistory() {
  CombatStateTracker cs;
  const EntityId id{7, 0};

  CHECK(cs.HitboxCount(id) == 0);
  CHECK(cs.TrackedEntities() == 0);

  cs.ObserveEntity(Snap(7, EntityType::Player, 0, 64, 0), 100);
  CHECK(cs.HitboxCount(id) == 1);
  CHECK(cs.TrackedEntities() == 1);

  // Player hitbox is 0.6 x 1.8 x 0.6, sitting on the feet.
  Aabb box;
  CHECK(cs.HitboxAt(id, 100, &box));
  CHECK(std::fabs(box.min.x - (-0.3f)) < 1e-4f);
  CHECK(std::fabs(box.max.x - 0.3f) < 1e-4f);
  CHECK(std::fabs(box.min.y - 64.0f) < 1e-4f);
  CHECK(std::fabs(box.max.y - 65.8f) < 1e-4f);

  // A tick with no sample fails closed rather than returning a stale box.
  CHECK(!cs.HitboxAt(id, 101, &box));

  // Re-feeding the same tick replaces rather than accumulates.
  cs.ObserveEntity(Snap(7, EntityType::Player, 5, 64, 0), 100);
  CHECK(cs.HitboxCount(id) == 1);
  CHECK(cs.HitboxAt(id, 100, &box));
  CHECK(std::fabs(box.min.x - 4.7f) < 1e-4f);

  // The history is bounded per entity.
  for (u64 t = 101; t < 101 + CombatStateTracker::kHistoryPerEntity + 10; ++t) {
    cs.ObserveEntity(Snap(7, EntityType::Player, (f32)t, 64, 0), t);
  }
  CHECK(cs.HitboxCount(id) == CombatStateTracker::kHistoryPerEntity);

  // at_or_before returns the newest sample not past the tick.
  Aabb b2;
  u64 gotTick = 0;
  CHECK(cs.HitboxAtOrBefore(id, 200, &b2, &gotTick));
  CHECK(gotTick <= 200);
  CHECK(!cs.HitboxAtOrBefore(id, 0, &b2, &gotTick));   // nothing that old
}

static void TestHitboxEntityCapEvictsOldest() {
  CombatStateTracker cs;
  for (size_t i = 0; i < CombatStateTracker::kMaxEntities + 32; ++i) {
    cs.ObserveEntity(Snap(i + 1, EntityType::Mob, (f32)i, 64, 0), 10 + i);
  }
  CHECK(cs.TrackedEntities() == CombatStateTracker::kMaxEntities);
  // The first-fed entity was evicted; the newest is present.
  CHECK(cs.HitboxCount(EntityId{1, 0}) == 0);
  CHECK(cs.HitboxCount(EntityId{(u64)(CombatStateTracker::kMaxEntities + 32), 0}) == 1);
}

//----------------------------------------------------------------------------
// Reach validation
//----------------------------------------------------------------------------

static void TestReachValidation() {
  CombatStateTracker cs;
  CHECK(cs.SetMaxReach(3.0f));
  CHECK(!cs.SetMaxReach(0.0f));       // refused
  CHECK(!cs.SetMaxReach(-1.0f));
  CHECK(std::fabs(cs.maxReach() - 3.0f) < 1e-4f);

  // Attacker at origin, victim 2 blocks away: within reach.
  cs.ObserveEntity(Snap(1, EntityType::Player, 0, 64, 0), 50);
  cs.ObserveEntity(Snap(2, EntityType::Player, 2, 64, 0), 50);

  CombatEvent hit;
  hit.attacker = EntityId{1, 0};
  hit.victim = EntityId{2, 0};
  hit.finalDamage = 4.0f;
  hit.tick = 50;

  ReachCheck rc = cs.ObserveHit(hit, 50);
  CHECK(rc.valid);
  CHECK(rc.withinReach);
  CHECK(rc.distance > 1.0f && rc.distance < 2.0f);
  CHECK(cs.reachViolations() == 0);

  // A victim 5 blocks away: out of reach, counted.
  cs.ObserveEntity(Snap(3, EntityType::Player, 5, 64, 0), 51);
  CombatEvent far;
  far.attacker = EntityId{1, 0};
  far.victim = EntityId{3, 0};
  far.finalDamage = 4.0f;
  far.tick = 51;
  rc = cs.ObserveHit(far, 51);
  CHECK(rc.valid);
  CHECK(!rc.withinReach);
  CHECK(rc.distance > 3.0f);
  CHECK(cs.reachViolations() == 1);
  CHECK(cs.lastReach().victim == far.victim);

  // An unknown victim position: invalid, never "within reach".
  CombatEvent unknown;
  unknown.attacker = EntityId{1, 0};
  unknown.victim = EntityId{999, 0};
  unknown.finalDamage = 1.0f;
  unknown.tick = 52;
  rc = cs.ObserveHit(unknown, 52);
  CHECK(!rc.valid);
  CHECK(!rc.withinReach);
  CHECK(cs.reachViolations() == 1);   // an unknown position is not a violation
}

//----------------------------------------------------------------------------
// Swing timing
//----------------------------------------------------------------------------

static void TestSwingTiming() {
  CombatStateTracker cs;
  const EntityId a{1, 0};

  // The first swing has no interval.
  SwingSample s = cs.ObserveSwing(a, 100);
  CHECK(!s.valid);
  CHECK(s.deltaTicks == 0);
  CHECK(s.cps == 0);

  // Four ticks later: 20 tps / 4 = 5 swings per second.
  s = cs.ObserveSwing(a, 104);
  CHECK(s.valid);
  CHECK(s.deltaTicks == 4);
  CHECK(std::fabs(s.cps - 5.0) < 1e-6);

  // Ten ticks later: 2 swings per second.
  s = cs.ObserveSwing(a, 114);
  CHECK(s.valid);
  CHECK(s.deltaTicks == 10);
  CHECK(std::fabs(s.cps - 2.0) < 1e-6);
  CHECK(cs.lastSwing().tick == 114);

  // Average across the window: 2 intervals over a 14-tick span.
  const f64 avg = cs.AverageSwingCps(a);
  CHECK(std::fabs(avg - (2.0 * 20.0 / 14.0)) < 1e-6);

  // An entity that never swung has no average.
  CHECK(cs.AverageSwingCps(EntityId{42, 0}) == 0);
}

//----------------------------------------------------------------------------
// Projectile prediction
//----------------------------------------------------------------------------

static void TestProjectileExtrapolation() {
  // A projectile with no gravity advances by velocity*dt each tick.
  const Vec3 p = CombatStateTracker::Extrapolate(Vec3(0, 0, 0), Vec3(20, 0, 0), 0.0f, 1);
  CHECK(std::fabs(p.x - 1.0f) < 1e-5f);   // 20 blocks/s * 0.05 s
  CHECK(std::fabs(p.y) < 1e-5f);

  // Ten ticks: 20 * 0.05 * 10 = 10 blocks.
  const Vec3 p10 = CombatStateTracker::Extrapolate(Vec3(0, 0, 0), Vec3(20, 0, 0), 0.0f, 10);
  CHECK(std::fabs(p10.x - 10.0f) < 1e-4f);

  // Gravity pulls the vertical velocity down. After one tick the y velocity is
  // -1 (=-20*0.05) but the first step used the un-decayed velocity, so y stays 0.
  const Vec3 g = CombatStateTracker::Extrapolate(Vec3(0, 0, 0), Vec3(0, 0, 0), -20.0f, 1);
  CHECK(std::fabs(g.y) < 1e-5f);
  // After two ticks the accumulated pull shows: step 2 uses v=-1 -> y=-0.05.
  const Vec3 g2 = CombatStateTracker::Extrapolate(Vec3(0, 0, 0), Vec3(0, 0, 0), -20.0f, 2);
  CHECK(std::fabs(g2.y - (-0.05f)) < 1e-5f);

  // Zero ticks returns the input unchanged.
  const Vec3 same = CombatStateTracker::Extrapolate(Vec3(1, 2, 3), Vec3(9, 9, 9), -20.0f, 0);
  CHECK(same == Vec3(1, 2, 3));
}

static void TestProjectileResolve() {
  CombatStateTracker cs;
  const EntityId id{5, 0};

  // Resolving with no pending prediction reports -1.
  CHECK(cs.ResolveProjectile(id, Vec3(0, 0, 0), 10) < 0);
  CHECK(cs.MeanPredictionError() < 0);
  CHECK(cs.PredictionCount() == 0);

  cs.NoteProjectile(id, Vec3(10, 0, 0), 10);
  const f32 err = cs.ResolveProjectile(id, Vec3(11, 0, 0), 15);
  CHECK(std::fabs(err - 1.0f) < 1e-5f);
  CHECK(cs.PredictionCount() == 1);
  CHECK(std::fabs(cs.MeanPredictionError() - 1.0f) < 1e-5f);

  // A second, worse prediction raises the mean.
  cs.NoteProjectile(id, Vec3(0, 0, 0), 20);
  cs.ResolveProjectile(id, Vec3(3, 0, 0), 25);
  CHECK(std::fabs(cs.MeanPredictionError() - 2.0f) < 1e-5f);

  // A resolved prediction is consumed; a second resolve reports -1.
  CHECK(cs.ResolveProjectile(id, Vec3(0, 0, 0), 26) < 0);
}

//----------------------------------------------------------------------------
// Hit prediction accuracy
//----------------------------------------------------------------------------

static void TestHitPredictionAccuracy() {
  CombatStateTracker cs;
  const EntityId attacker{1, 0};
  const EntityId victim{2, 0};

  ChannelProbe predicted(combat_event::Predicted());
  ChannelProbe confirmed(combat_event::Confirmed());
  ChannelProbe missed(combat_event::Missed());

  CHECK(cs.PredictedHits() == 0);
  CHECK(cs.HitAccuracy() == 0);

  // Predict a hit and confirm it with the applied event.
  cs.PredictHit(attacker, victim, 100);
  CHECK(cs.PredictedHits() == 1);

  CombatEvent evt;
  evt.attacker = attacker;
  evt.victim = victim;
  evt.finalDamage = 3.0f;
  evt.tick = 101;
  CHECK(cs.ConfirmHit(evt, 101));
  CHECK(cs.ConfirmedHits() == 1);
  CHECK(cs.MissedHits() == 0);
  CHECK(std::fabs(cs.HitAccuracy() - 1.0) < 1e-9);

  // A confirm with no matching prediction is refused.
  CHECK(!cs.ConfirmHit(evt, 102));

  // A prediction that ages out past the window becomes a miss.
  cs.PredictHit(attacker, victim, 200);
  // The window is inclusive: a prediction is still live at exactly
  // tick+window, so it takes one more tick to expire.
  cs.Tick(200 + CombatStateTracker::kPredictionWindowTicks + 1);
  CHECK(cs.PredictedHits() == 2);
  CHECK(cs.ConfirmedHits() == 1);
  CHECK(cs.MissedHits() == 1);
  CHECK(std::fabs(cs.HitAccuracy() - 0.5) < 1e-9);

  // The event channels carried the right payloads.
  CHECK(predicted.payloads.size() == 2);
  if (predicted.payloads.size() == 2) CHECK(predicted.payloads[1] == "1:2");
  CHECK(confirmed.payloads.size() == 1);
  if (confirmed.payloads.size() == 1) CHECK(confirmed.payloads[0] == "1:2");
  CHECK(missed.payloads.size() == 1);
  if (missed.payloads.size() == 1) CHECK(missed.payloads[0] == "1:2");
}

//----------------------------------------------------------------------------
// Click budget
//----------------------------------------------------------------------------

static void TestClickBudget() {
  CombatStateTracker cs;
  CHECK(cs.SetClickBudget(20.0f, 20.0f));
  CHECK(!cs.SetClickBudget(0.0f, 5.0f));      // refused
  CHECK(!cs.SetClickBudget(20.0f, 0.0f));     // burst < 1 refused

  // The first click fills the bucket; a burst within the allowance is allowed.
  CHECK(cs.NoteClick(MouseButton::Left, 1000));
  CHECK(cs.ClickRate(MouseButton::Left) >= 1.0);

  // Drain the bucket with a burst of clicks in the same millisecond.
  for (int i = 0; i < 19; ++i) {
    cs.NoteClick(MouseButton::Left, 1000);
  }
  CHECK(!cs.ClickLimited());
  // The 21st click in the same ms is over budget but is NOT dropped: it reports
  // the limit and the rate still counts it.
  const bool ok = cs.NoteClick(MouseButton::Left, 1000);
  CHECK(!ok);
  CHECK(cs.ClickLimited());
  CHECK(cs.ClickRate(MouseButton::Left) == 21.0);

  // A second later the bucket has refilled to the burst cap.
  cs.NoteClick(MouseButton::Left, 2000);
  CHECK(cs.ClickTokens(MouseButton::Left) >= 19.0f);

  // The right button has its own bucket.
  CHECK(cs.NoteClick(MouseButton::Right, 2000));
  CHECK(cs.ClickRate(MouseButton::Right) == 1.0);
}

//----------------------------------------------------------------------------
// Reset
//----------------------------------------------------------------------------

static void TestClear() {
  CombatStateTracker cs;
  cs.ObserveEntity(Snap(1, EntityType::Player, 0, 64, 0), 1);
  cs.ObserveSwing(EntityId{1, 0}, 1);
  cs.PredictHit(EntityId{1, 0}, EntityId{2, 0}, 1);
  cs.NoteClick(MouseButton::Left, 0);
  CHECK(cs.TrackedEntities() == 1);

  cs.Clear();
  CHECK(cs.TrackedEntities() == 0);
  CHECK(cs.HitboxSamples() == 0);
  CHECK(cs.PredictedHits() == 0);
  CHECK(cs.lastSwing().tick == 0);
  CHECK(cs.PredictionCount() == 0);
  // Clear() does not reset configuration.
  CHECK(std::fabs(cs.maxReach() - 3.0f) < 1e-4f);
}

// The combat channels are script-facing: they must be exactly the names the IDL
// declares (api/berry.idl `event combatPredicted(...)` etc.), because a script
// awaits the literal string and the async layer does no name translation. A
// probe on the literal only fires if the tracker publishes on it, so this pins
// the emitted name to the documented one.
static void TestEventChannelNames() {
  CHECK(std::string(combat_event::Predicted()) == "combatPredicted");
  CHECK(std::string(combat_event::Confirmed()) == "combatConfirmed");
  CHECK(std::string(combat_event::Missed()) == "combatMissed");

  CombatStateTracker cs;
  const EntityId attacker{1, 0};
  const EntityId victim{2, 0};

  // Listeners registered on the literal IDL names, not on the accessors.
  ChannelProbe predicted("combatPredicted");
  ChannelProbe confirmed("combatConfirmed");
  ChannelProbe missed("combatMissed");

  cs.PredictHit(attacker, victim, 10);
  CHECK(predicted.payloads.size() == 1);
  CHECK(predicted.payloads[0] == "1:2");

  CombatEvent evt;
  evt.attacker = attacker;
  evt.victim = victim;
  evt.finalDamage = 4.0f;
  evt.tick = 11;
  CHECK(cs.ConfirmHit(evt, 11));
  CHECK(confirmed.payloads.size() == 1);
  CHECK(confirmed.payloads[0] == "1:2");

  cs.PredictHit(attacker, victim, 20);
  cs.Tick(20 + CombatStateTracker::kPredictionWindowTicks + 1);
  CHECK(missed.payloads.size() == 1);
  CHECK(missed.payloads[0] == "1:2");
}

int main() {
TestHitboxHistory();
TestHitboxEntityCapEvictsOldest();
TestReachValidation();
TestSwingTiming();
TestProjectileExtrapolation();
TestProjectileResolve();
TestHitPredictionAccuracy();
TestClickBudget();
TestClear();
TestEventChannelNames();

  if (g_fail) {
    printf("test_combat_state: %d failure(s)\n", g_fail);
    return 1;
  }
  printf("test_combat_state: all OK\n");
  return 0;
}
