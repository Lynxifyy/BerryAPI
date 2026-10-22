#include "berry/engine.h"
#include "berry/services.h"
#include "berry/module.h"
#include "berry/game.h"
#include "berry/combat.h"
#include "berry/render.h"
#include "berry/event.h"
#include "shim_level.h"

#include <cstdio>
#include <cmath>
#include <memory>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

// Damage seam that records observed (engine-applied) damage events.
class CaptureDamageSeam final : public DamageSeam {
 public:
  int called = 0;
  f32 lastAmount = 0;
  EntityId lastVictim;

  bool OnDamage(const CombatEvent& evt) override {
    called++;
    lastAmount = evt.finalDamage;
    lastVictim = evt.victim;
    return true;   // accept the observation
  }
  void OnTick(u64 tick) override { (void)tick; }
};

int main() {
  // --- Engine + default modules bootstrap ---
  Engine& eng = Engine::Instance();
  eng.Bootstrap();
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

  // Time: only readable through the public seam — mutate via the shim's
  // test-only helper, read through WorldHandle (read-only API).
  LevelTime t;
  t.day = false;
  t.absolute = 13000;
  shim->SetTime(t);          // shim state only; not part of LevelSeam
  CHECK(!world->GetTime().day);

  // Block get (read-only path); mutating is a shim-only test op.
  shim->SetBlock(Vec3(5, 40, -8), 1);
  CHECK(world->GetBlock(Vec3(5, 40, -8)) == 1);
  CHECK(world->GetBlock(Vec3(99, 99, 99)) == 0);

  // RecordMessage is a shim-only test hook; there is no game-facing
  // SendMessage in the read-only world API.
  shim->RecordMessage("hello from BerryAPI");
  CHECK(shim->Messages().size() == 1);
  CHECK(shim->Messages()[0] == "hello from BerryAPI");

  // KV
  world->SetValue("mode", "survival");
  CHECK(world->HasValue("mode"));
  CHECK(std::string(world->GetValue("mode", "?")) == "survival");
  CHECK(std::string(world->GetValue("missing", "fb")) == "fb");

  // --- Combat module (read-only observer) ---
  Combat* combat = CombatFrom(eng.Modules());
  CHECK(combat != nullptr);
  CaptureDamageSeam dseam;
  combat->BindSeam(&dseam);   // module already attached via engine; swap seam

  EntityId attacker{99, 900};

  // Observe an engine-applied hit.
  CombatEvent evt;
  evt.victim = e1;
  evt.attacker = attacker;
  evt.baseDamage = 5.0f;
  evt.finalDamage = 5.0f;
  evt.cause = DamageCause::EntityAttack;
  auto outcome = combat->Observe(evt);
  CHECK(outcome == CombatOutcome::Observed);
  CHECK(dseam.called == 1);
  CHECK(dseam.lastAmount == 5.0f);

  // Invalid events are ignored (fail closed): zero victim, NaN damage.
  dseam.called = 0;
  CombatEvent bad;
  bad.victim = EntityId{};              // no victim
  bad.attacker = attacker;
  bad.finalDamage = 5.0f;
  CHECK(combat->Observe(bad) == CombatOutcome::Ignored);
  CHECK(dseam.called == 0);

  CombatEvent nan;
  nan.victim = e1;
  nan.attacker = attacker;
  nan.finalDamage = std::nanf("");
  CHECK(combat->Observe(nan) == CombatOutcome::Ignored);
  CHECK(dseam.called == 0);

  // Metrics
  auto metrics = combat->Metrics(attacker);
  CHECK(metrics.hits == 1);
  CHECK(metrics.damageDealt == 5.0);

  // Kill attribution is recorded from engine-observed events only.
  combat->RecordKill(attacker, e1, DamageCause::EntityAttack);
  auto kills = combat->RecentKills();
  CHECK(kills.size() == 1);
  CHECK(kills[0].killer.raw == attacker.raw);
  CHECK(kills[0].victim.raw == e1.raw);
  CHECK(combat->Metrics(attacker).kills == 1);

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

  // --- Reactive world grid is driven from the engine tick ---
  // The World module diffs each tick against the previous one and publishes
  // the transitions, so a script's `Events.once("entityEntered")` fires
  // without polling. Assert the whole chain: engine tick -> World::OnTick ->
  // grid diff -> EventBus.
  {
    std::string entered, left;
    EventBus::ListenerId entId = EventBus::Instance().On(
        "entityEntered", [&](const EventContext& e) {
          if (e.data) entered = *(const char**)e.data;
          return EventResult::Continue;
        });
    EventBus::ListenerId leftId = EventBus::Instance().On(
        "entityLeft", [&](const EventContext& e) {
          if (e.data) left = *(const char**)e.data;
          return EventResult::Continue;
        });

    // First tick establishes the baseline: e1 is Entered.
    eng.OnGameTick(100);
    CHECK(entered == "1:mob");
    entered.clear();

    // A new entity appears; the next tick reports it Entered.
    EntityId e2{2, 200};
    EntitySnapshot s2;
    s2.id = e2;
    s2.type = EntityType::Player;
    s2.position = Vec3(0, 0, 0);
    s2.health = 20;
    s2.maxHealth = 20;
    s2.alive = true;
    s2.name = "steve";
    shim->AddEntity(e2, s2);
    eng.OnGameTick(101);
    CHECK(entered == "2:player");
    entered.clear();

    // Removing e1 makes the next tick report it Left.
    shim->RemoveEntity(e1);
    eng.OnGameTick(102);
    CHECK(left == "1:mob");

    EventBus::Instance().Off("entityEntered", entId);
    EventBus::Instance().Off("entityLeft", leftId);
    // Restore e1 so the later frame/tick section sees the original world.
    shim->AddEntity(e1, snap);
  }

  // --- Engine frame + tick ---
  eng.OnGameTick(21);
  eng.OnFrame();

  eng.Detach();

  if (g_fail == 0) printf("test_modules: ALL OK\n");
  return g_fail;
}