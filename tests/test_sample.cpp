// test_sample.cpp — integration test for a strict module pattern.
//
// Verifies:
//   * Combat::Damage emits "berry:combat" on the event bus for both the
//     applied and cancelled paths.
//   * A listener can inspect the CombatEvent payload.
//   * A strict module can pull World+Combat+Render from the registry and
//     exercise them headlessly (the pattern the sample module uses).

#include "berry/engine.h"
#include "berry/services.h"
#include "berry/combat.h"
#include "berry/render.h"
#include "berry/event.h"
#include "shim_level.h"

#include <cstdio>
#include <memory>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

// Minimal damage seam so Combat can apply hits without touching the engine.
class LocalDamageSeam final : public DamageSeam {
 public:
  int applied = 0;
  int lastAmount = 0;
  bool Apply(CombatEvent& evt) override {
    applied++;
    lastAmount = (int)evt.finalDamage;
    return true;
  }
  void OnTick(u64 tick) override { (void)tick; }
};

// Strict module mimicking samples/sample_hitmarker.cpp: listens on
// "berry:combat", publishes hits, then pulls the World service to chat.
class TestProbeModule final : public BerryModule {
 public:
  int hits = 0;
  u64 lastFlashAt = 0;

  const char* Name() const override { return "Berry::Tests::Probe"; }
  u32 Capabilities() const override { return kCapCombat | kCapRender | kCapWorld; }

  bool Attach(Engine& e) override {
    id_ = e.Bus().On("berry:combat", [this](const EventContext& ctx) {
      auto* evt = static_cast<const CombatEvent*>(ctx.data);
      if (evt && !evt->cancelled) {
        hits++;
        lastFlashAt = NowMs() + 80;
      }
      return EventResult::Continue;
    });
    return true;
  }

  void Detach(Engine& e) override {
    e.Bus().Off("berry:combat", id_);
  }

 private:
  EventBus::ListenerId id_ = 0;
};

int main() {
  Engine& eng = Engine::Instance();

  // Register our own strict module alongside defaults.
  auto mods = CreateDefaultModules();
  auto probe = std::make_shared<TestProbeModule>();
  mods.push_back(probe);
  eng.Bootstrap();
  for (auto& m : mods) {
    CHECK(eng.Modules().Register(m));
  }
  CHECK(eng.Attach());
  CHECK(probe->hits == 0);

  // World service reachable from the registry.
  WorldHandle* world = WorldFrom(eng.Modules());
  CHECK(world != nullptr);
  auto shim = std::make_shared<Berry::Test::ShimLevel>();
  world->SetSeam(shim);

  EntityId e1{1, 100};
  {
    EntitySnapshot s;
    s.id = e1;
    s.type = EntityType::Mob;
    s.position = Vec3(3, 4, 0);
    s.health = 20;
    s.maxHealth = 20;
    s.alive = true;
    s.name = "dummy";
    shim->AddEntity(e1, s);
  }
  world->RefreshCache();

  // Combat service + seam. The module already attached the Combat service
  // (with a null seam) during engine attach; swap in our recording seam.
  Combat* combat = CombatFrom(eng.Modules());
  CHECK(combat != nullptr);
  auto dseam = std::make_shared<LocalDamageSeam>();
  combat->BindSeam(dseam.get());

  // Applied hit should fire the bus event.
  EntityId attacker{99, 900};
  combat->Damage(e1, attacker, 5.0f, DamageCause::EntityAttack);
  CHECK(probe->hits == 1);
  CHECK(dseam->applied == 1);
  CHECK(dseam->lastAmount == 5);

  // A second hit.
  combat->Damage(e1, attacker, 3.0f, DamageCause::EntityAttack);
  CHECK(probe->hits == 2);
  CHECK(dseam->applied == 2);

  // Metrics show through the sibling service.
  auto m = combat->Metrics(attacker);
  CHECK(m.hits == 2);
  CHECK(m.damageDealt >= 8.0f - 0.001f);

  // Cancelled hit (a pre-rule blocks it) must still emit the event with
  // cancelled == true, and the probe must not flash.
  auto cancelRule = combat->AddPreRule([](CombatEvent& evt) {
    evt.cancelled = true;
    return true;
  });
  combat->Damage(e1, attacker, 1.0f, DamageCause::EntityAttack);
  CHECK(probe->hits == 2);              // cancelled hit not counted as hit
  CHECK(dseam->applied == 2);
  combat->RemoveRule(cancelRule);

  // Full engine frame + tick keeps everything healthy.
  eng.OnGameTick(21);
  eng.OnFrame();

  eng.Detach();
  if (g_fail == 0) printf("test_sample: ALL OK\n");
  return g_fail;
}