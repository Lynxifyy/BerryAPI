// Host tests for the client-side crystal-placement advisor (Phase 6e).
//
// These drive the real advisor: the damage model, config validation, a full
// analysis against a stub block world, the self-damage penalty, the candidate
// bound, and the arm/disarm switch the tick hook reads.
#include "berry/crystal.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static bool Near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

namespace {

/// Every cell is air, so every candidate spot is placeable.
struct OpenWorld final : CrystalWorld {
  u32 BlockAt(const Vec3&) const override { return 0; }
};

/// Every cell is solid, so no spot is placeable.
struct SolidWorld final : CrystalWorld {
  u32 BlockAt(const Vec3&) const override { return 1; }
};

EntityId Id(u64 raw) {
  EntityId id;
  id.raw = raw;
  return id;
}

EntitySnapshot PlayerAt(u64 raw, const Vec3& pos) {
  EntitySnapshot s;
  s.id = Id(raw);
  s.type = EntityType::Player;
  s.position = pos;
  s.health = 20;
  s.maxHealth = 20;
  s.alive = true;
  return s;
}

/// A config that ignores self-damage, so a pure enemy-damage ranking is easy to
/// assert.
CrystalConfig OffensiveConfig() {
  CrystalConfig c;
  c.placeRange = 4.5f;
  c.blastRadius = 6.0f;
  c.maxDamage = 12.0f;
  c.minTargets = 1;
  c.selfDamageWeight = 0.0f;
  c.maxSelfDamage = 1000.0f;
  c.levels = 1;
  c.maxResults = 8;
  return c;
}

}  // namespace

//----------------------------------------------------------------------------
// Damage model
//----------------------------------------------------------------------------

static void TestEstimateDamage() {
  const CrystalConfig cfg = OffensiveConfig();

  // Full damage at the epicentre, zero at and beyond the blast radius, half at
  // the midpoint — the linear falloff the docs promise.
  CHECK(Near(CrystalOptimizer::EstimateDamage(Vec3(0, 0, 0), Vec3(0, 0, 0), cfg), 12.0f));
  CHECK(Near(CrystalOptimizer::EstimateDamage(Vec3(0, 0, 0), Vec3(3, 0, 0), cfg), 6.0f));
  CHECK(Near(CrystalOptimizer::EstimateDamage(Vec3(0, 0, 0), Vec3(6, 0, 0), cfg), 0.0f));
  CHECK(Near(CrystalOptimizer::EstimateDamage(Vec3(0, 0, 0), Vec3(9, 0, 0), cfg), 0.0f));

  // A zero-radius blast deals nothing (fail closed, not divide by zero).
  CrystalConfig zero = cfg;
  zero.blastRadius = 0.0f;
  CHECK(Near(CrystalOptimizer::EstimateDamage(Vec3(0, 0, 0), Vec3(0, 0, 0), zero), 0.0f));
}

//----------------------------------------------------------------------------
// Configuration validation
//----------------------------------------------------------------------------

static void TestConfigValidation() {
  CrystalOptimizer o;
  CHECK(o.SetConfig(OffensiveConfig()));
  CHECK(Near(o.config().blastRadius, 6.0f));

  CrystalConfig bad;
  bad.placeRange = 0.0f;
  CHECK(!o.SetConfig(bad));
  bad = OffensiveConfig();
  bad.placeRange = CrystalOptimizer::kMaxRange + 1.0f;
  CHECK(!o.SetConfig(bad));
  bad = OffensiveConfig();
  bad.blastRadius = -1.0f;
  CHECK(!o.SetConfig(bad));
  bad = OffensiveConfig();
  bad.blastRadius = CrystalOptimizer::kMaxBlastRadius + 1.0f;
  CHECK(!o.SetConfig(bad));
  bad = OffensiveConfig();
  bad.maxDamage = -1.0f;
  CHECK(!o.SetConfig(bad));
  bad = OffensiveConfig();
  bad.minTargets = -1;
  CHECK(!o.SetConfig(bad));
  bad = OffensiveConfig();
  bad.selfDamageWeight = -1.0f;
  CHECK(!o.SetConfig(bad));
  bad = OffensiveConfig();
  bad.maxSelfDamage = -1.0f;
  CHECK(!o.SetConfig(bad));
  bad = OffensiveConfig();
  bad.levels = CrystalOptimizer::kMaxLevels + 1;
  CHECK(!o.SetConfig(bad));
  bad = OffensiveConfig();
  bad.maxResults = 0;
  CHECK(!o.SetConfig(bad));

  // A refusal changes nothing.
  CHECK(Near(o.config().blastRadius, 6.0f));
}

//----------------------------------------------------------------------------
// Analysis
//----------------------------------------------------------------------------

static void TestAnalyzeFindsBestSpot() {
  CrystalOptimizer o;
  CHECK(o.SetConfig(OffensiveConfig()));
  OpenWorld world;

  // Placer at (0.5, 0, 0.5); one enemy player at (2.5, 0, 0.5), two blocks east.
  const std::vector<EntitySnapshot> targets = {PlayerAt(7, Vec3(2.5f, 0, 0.5f))};
  const std::vector<CrystalPlacement> recs =
      o.Analyze(Vec3(0.5f, 0, 0.5f), Id(1), targets, world, 100);

  CHECK(!recs.empty());
  const CrystalPlacement best = o.best();
  // The best spot sits on the enemy: the blast centre lands inside the target's
  // hitbox, so it takes full damage.
  CHECK(best.targetsHit == 1);
  CHECK(Near(best.bestDamage, 12.0f));
  CHECK(Near(best.totalDamage, 12.0f));
  CHECK(Near(best.position.x, 2.0f));
  CHECK(Near(best.position.y, 0.0f));
  CHECK(Near(best.position.z, 0.0f));

  // The ranking is sorted by score, descending.
  for (size_t i = 1; i < recs.size(); ++i) {
    CHECK(recs[i - 1].score >= recs[i].score);
  }

  // The statistics account for the run.
  const CrystalStats st = o.stats();
  CHECK(st.analyses == 1);
  CHECK(st.candidates > 0);
  CHECK(st.placements == recs.size());

  // Determinism: the same inputs produce the same ranking, position for
  // position.
  const std::vector<CrystalPlacement> again =
      o.Analyze(Vec3(0.5f, 0, 0.5f), Id(1), targets, world, 101);
  CHECK(again.size() == recs.size());
  for (size_t i = 0; i < again.size(); ++i) {
    CHECK(again[i].position == recs[i].position);
    CHECK(Near(again[i].score, recs[i].score));
  }
}

static void TestSelfDamageIsPenalised() {
  CrystalOptimizer o;
  CrystalConfig cfg = OffensiveConfig();
  cfg.selfDamageWeight = 1.0f;   // a straight subtraction
  cfg.maxSelfDamage = 1000.0f;
  CHECK(o.SetConfig(cfg));
  OpenWorld world;

  // The placer is itself a target, and an enemy sits two blocks away.
  const std::vector<EntitySnapshot> targets = {
      PlayerAt(1, Vec3(0.5f, 0, 0.5f)),   // the placer
      PlayerAt(7, Vec3(2.5f, 0, 0.5f)),   // the enemy
  };
  const std::vector<CrystalPlacement> recs =
      o.Analyze(Vec3(0.5f, 0, 0.5f), Id(1), targets, world, 1);
  CHECK(!recs.empty());

  const CrystalPlacement best = o.best();
  // The best spot hurts the enemy and is scored net of the damage it does to
  // the placer.
  CHECK(best.targetsHit == 1);
  CHECK(best.selfDamage > 0.0f);
  CHECK(Near(best.score, best.totalDamage - best.selfDamage));
  // It must not be the placer's own feet, where the self-damage is maximal.
  CHECK(!(Near(best.position.x, 0.0f) && Near(best.position.z, 0.0f)));
}

static void TestNoPlaceableSpot() {
  CrystalOptimizer o;
  CHECK(o.SetConfig(OffensiveConfig()));
  SolidWorld solid;
  const std::vector<EntitySnapshot> targets = {PlayerAt(7, Vec3(2.5f, 0, 0.5f))};
  CHECK(o.Analyze(Vec3(0.5f, 0, 0.5f), Id(1), targets, solid, 1).empty());
  bool ok = true;
  o.best(&ok);
  CHECK(!ok);
}

static void TestNoTargetsNoRecommendation() {
  CrystalOptimizer o;
  CHECK(o.SetConfig(OffensiveConfig()));
  OpenWorld world;
  // With no targets there is nothing to hit, so nothing is recommended.
  CHECK(o.Analyze(Vec3(0.5f, 0, 0.5f), Id(1), {}, world, 1).empty());

  // A spot that hits a target is still dropped when it is out of the blast
  // radius of every target.
  const std::vector<EntitySnapshot> far = {PlayerAt(7, Vec3(50.0f, 0, 50.0f))};
  CHECK(o.Analyze(Vec3(0.5f, 0, 0.5f), Id(1), far, world, 1).empty());
}

static void TestCandidateBound() {
  CrystalOptimizer o;
  CrystalConfig cfg = OffensiveConfig();
  cfg.placeRange = CrystalOptimizer::kMaxRange;     // the widest grid
  cfg.levels = CrystalOptimizer::kMaxLevels;
  CHECK(o.SetConfig(cfg));
  OpenWorld world;
  const std::vector<EntitySnapshot> targets = {PlayerAt(7, Vec3(0.5f, 0, 0.5f))};
  o.Analyze(Vec3(0.5f, 0, 0.5f), Id(1), targets, world, 1);
  // One analysis never tests more than the cap, however wide the grid.
  CHECK(o.stats().candidates <= CrystalOptimizer::kMaxCandidates);
}

//----------------------------------------------------------------------------
// Arm / disarm (the switch the tick hook reads)
//----------------------------------------------------------------------------

static void TestArmDisarm() {
  CrystalOptimizer o;
  CHECK(!o.armed());
  CHECK(o.Arm());
  CHECK(o.armed());
  CHECK(!o.Arm());        // already armed
  o.Disarm();
  CHECK(!o.armed());

  o.Arm();
  o.Reset();
  CHECK(o.armed());       // Reset clears the ranking, not the arm state
  CHECK(o.stats().analyses == 0);
  CHECK(o.recommendations().empty());
}

int main() {
  TestEstimateDamage();
  TestConfigValidation();
  TestAnalyzeFindsBestSpot();
  TestSelfDamageIsPenalised();
  TestNoPlaceableSpot();
  TestNoTargetsNoRecommendation();
  TestCandidateBound();
  TestArmDisarm();
  if (g_fail == 0) printf("test_crystal: ALL OK\n");
  return g_fail;
}
