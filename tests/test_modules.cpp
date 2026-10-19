#include "berry/engine.h"
#include "berry/services.h"
#include "berry/module.h"
#include "berry/game.h"
#include "berry/combat.h"
#include "berry/render.h"
#include "shim_level.h"

#include <cstdio>
#include <memory>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

// Damage seam that records what the Combat module asked the engine to apply.
class CaptureDamageSeam final : public DamageSeam {
 public:
  int called = 0;
  f32 lastAmount = 0;
  EntityId lastVictim;

  bool Apply(CombatEvent& evt) override {
    called++;
    lastAmount = evt.finalDamage;
    lastVictim = evt.victim;
    return true;
  }
  void OnTick(u64 tick) override { (void)tick; }
};

int main() {
  // --- Engine + default modules bootstrap ---
  Engine& eng = Engine::Instance();
  eng.Bootstrap({});
  CHECK(eng.Attach());

  // World module: inject a shim level.
  WorldHandle* world = WorldFrom(eng.Modules());
  CHECK(world != nullptr);
  auto shim = std::make_shared<Berry::Test::ShimLevel>();

  EntityId e1{1, 100};
  {
    EntitySnapshot s;
    s.id = e1;
    s.type = EntityType::Mob;
    s.position = Vec3(10, 4, 2);
    s.health = 20;
    s.maxHealth = 20;
    s.alive = true;
    s.name = "zombie";
    shim->AddEntity(e1, s);
  }

  world->SetSeam(shim);
  world->RefreshCache();
  CHECK(world->CachedEntities().size() == 1);
  auto snap = world->GetEntity(e1);
  CHECK(snap.health == 20);
  CHECK(snap.name == "zombie");

  // Time control
  LevelTime t;
  t.day = false;
  t.absolute = 13000;
  world->SetTime(t);
  CHECK(!world->GetTime().day);

  // Block set/get
  CHECK(world->SetBlock(Vec3(5, 40, -8), 1));
  CHECK(world->GetBlock(Vec3(5, 40, -8)) == 1);
  CHECK(world->GetBlock(Vec3(99, 99, 99)) == 0);

  // Messages
  CHECK(world->SendMessage("hello from BerryAPI"));
  CHECK(shim->Messages().size() == 1);
  CHECK(shim->Messages()[0] == "hello from BerryAPI");

  // KV
  world->SetValue("mode", "survival");
  CHECK(world->HasValue("mode"));
  CHECK(std::string(world->GetValue("mode", "?")) == "survival");
  CHECK(std::string(world->GetValue("missing", "fb")) == "fb");

  // --- Combat module ---
  Combat* combat = CombatFrom(eng.Modules());
  CHECK(combat != nullptr);
  CaptureDamageSeam dseam;
  combat->BindSeam(&dseam);   // module already attached via engine; swap seam

  // Pre-rule that doubles damage.
  auto preId = combat->AddPreRule([](CombatEvent& evt) {
    evt.finalDamage = evt.baseDamage * 2.0f;
    return false;
  });
  // Pre-rule that cancels a specific victim.
  auto cancelId = combat->AddPreRule([](CombatEvent& evt) {
    if (evt.victim.raw == 2) evt.cancelled = true;
    return evt.cancelled;
  });

  EntityId attacker{99, 900};
  auto outcome = combat->Damage(e1, attacker, 5.0f, DamageCause::EntityAttack);
  CHECK(outcome == CombatOutcome::Applied);
  CHECK(dseam.called == 1);
  CHECK(dseam.lastAmount == 10.0f);   // doubled

  // Cancel rule path
  EntityId victim2{2, 200};
  dseam.called = 0;
  auto outcome2 = combat->Damage(victim2, attacker, 5.0f, DamageCause::EntityAttack);
  CHECK(outcome2 == CombatOutcome::Cancelled);
  CHECK(dseam.called == 0);

  // Metrics
  auto metrics = combat->Metrics(attacker);
  CHECK(metrics.hits == 1);
  CHECK(metrics.damageDealt == 10.0);

  // Kill attribution (e1 survived; cancel rule still only affects victim2)
  bool killed = combat->Kill(e1, attacker, DamageCause::EntityAttack);
  CHECK(killed);
  auto kills = combat->RecentKills();
  CHECK(kills.size() == 1);
  CHECK(kills[0].killer.raw == attacker.raw);
  CHECK(kills[0].victim.raw == e1.raw);
  CHECK(combat->Metrics(attacker).kills == 1);

  // Remove rules
  combat->RemoveRule(preId);
  combat->RemoveRule(cancelId);

  // --- Render module ---
  Renderer* r = RendererFrom(eng.Modules());
  CHECK(r != nullptr);
  auto capture = std::make_shared<Berry::Test::CaptureRenderSeam>();
  r->BindSeam(capture.get());   // module already attached; swap seam

  r->BeginFrame(Vec2(1920, 1080));
  r->DrawLine(Vec2(0, 0), Vec2(10, 10), Rgba::Red());
  r->DrawText(Vec2(100, 50), "BerryAPI", Rgba::Green());
  r->DrawRect(Rect2D{Vec2(1, 1), Vec2(100, 100), Rgba::White(), Rgba::Blue(), true});
  r->DrawCircle(Vec2(200, 200), 30, Rgba::Yellow());
  r->EndFrame();

  CHECK(capture->lines == 1);
  CHECK(capture->texts == 1);
  CHECK(capture->rects == 1);
  CHECK(capture->circles == 1);
  CHECK(capture->lastScreen.x == 1920 && capture->lastScreen.y == 1080);
  CHECK(!capture->textContents.empty() && capture->textContents[0] == "BerryAPI");

  // --- ESP projection ---
  CameraState cam;
  cam.width = 1280;
  cam.height = 720;
  cam.view = Mat4::LookAt(Vec3(0, 0, -10), Vec3(0, 0, 0), Vec3(0, 1, 0));
  cam.projection = Mat4::Perspective(70, 1280.0f / 720.0f, 0.1f, 200.0f);
  cam.valid = true;
  r->SetCamera(cam);

  EspBox box;
  bool projected = r->EspBoxForEntity(snap, cam, &box);
  CHECK(projected);
  CHECK(box.label == "zombie");

  // --- Engine frame + tick ---
  eng.OnGameTick(21);
  eng.OnFrame();

  eng.Detach();

  if (g_fail == 0) printf("test_modules: ALL OK\n");
  return g_fail;
}