// sample_hitmarker.cpp — a strict BerryAPI module.
//
// Demonstrates:
//   1. A custom Berry:: module with a capability mask.
//   2. Pulling sibling services from the registry (Combat, Render, World)
//      without reaching into their internals.
//   3. Registering with the engine through CreateDefaultModules() + module
//      registration, plus a static constructor for when this TU is compiled
//      into the shared lib.
//
// Behavior: when a combat hit is applied, draw a brief red diagonal tick near
// the middle of the screen (a "hitmarker") and print the attacker's DPS in
// chat once per ~40 ticks.
//
// Compile (host, demonstrates it links against the library):
//   g++ -std=c++17 -Iinclude -c samples/sample_hitmarker.cpp -o /tmp/hm.o
//   (link with build/libBerryAPI.so when deployed on-device)

#include "berry/module.h"
#include "berry/services.h"
#include "berry/combat.h"
#include "berry/render.h"
#include "berry/engine.h"
#include "berry/event.h"
#include "berry/logger.h"

#include <cstdio>

namespace Berry::Samples {

// Strict sample module.
class HitmarkerModule : public BerryModule {
 public:
  HitmarkerModule() {
    SetServicePtr(nullptr);         // no primary service; purely uses siblings
  }

  const char* Name() const override { return "Berry::Samples::Hitmarker"; }
  u32 Capabilities() const override { return kCapCombat | kCapRender | kCapWorld; }

  bool Attach(Engine& e) override {
    // Subscribe to the combat pipeline. Combat::Observe emits "berry:combat"
    // with a CombatEvent* payload on every observed (engine-applied) hit.
    applied_ = e.Bus().On("berry:combat", [this](const EventContext& ctx) {
      auto* evt = static_cast<const CombatEvent*>(ctx.data);
      if (evt && evt->finalDamage > 0) flashUntil_ = Berry::NowMs() + 80;
      return EventResult::Continue;
    });
    Log::Info("Hitmarker attached (cap %08x)", Capabilities());
    return true;
  }

  void Detach(Engine& e) override {
    e.Bus().Off("berry:combat", applied_);
    Log::Info("Hitmarker detached");
  }

  void OnTick(GameTick tick) override {
    // Once per ~2s, report the local player's DPS (if any combat data).
    if (tick % 40 != 0) return;
    ModuleRegistry& reg = Engine::Instance().Modules();
    Combat* combat = CombatFrom(reg);
    if (!combat) return;
    WorldHandle* world = WorldFrom(reg);
    if (!world) return;

    EntitySnapshot self;
    // The host shim doesn't track a local player; fall back to first entity.
    auto ents = world->CachedEntities();
    if (ents.empty()) return;
    self = world->GetEntity(ents[0]);
    auto m = combat->Metrics(self.id);
    char msg[128];
    std::snprintf(msg, sizeof(msg), "[Berry] DPS %.1f (hits %llu)",
                  m.Dps(), (unsigned long long)m.hits);
    // Read-only world API: report metrics into the module-local KV store
    // instead of mutating the game via chat.
    world->SetValue("hitmarker.dps", msg);
    Log::Info("%s", msg);
  }

  // Optional per-frame draw: modules can call Renderer from their own frame
  // hook. This example draws the hitmarker via the Renderer if present.
  void Emit(Engine& e) {
    Renderer* render = RendererFrom(e.Modules());
    if (!render) return;
    if (Berry::NowMs() > flashUntil_) return;
    render->BeginFrame(Vec2(1280, 720));
    // Two short diagonal lines crossing at screen center.
    Rgba c(255, 60, 60);
    render->DrawLine(Vec2(628, 348), Vec2(642, 362), c, 3);
    render->DrawLine(Vec2(652, 348), Vec2(638, 362), c, 3);
    render->EndFrame();
  }

 private:
  EventBus::ListenerId applied_ = 0;
  u64 flashUntil_ = 0;
};

}  // namespace Berry::Samples

// ---------------------------------------------------------------------------
// Wire-in: register the sample alongside the built-in modules. When this TU
// is part of libBerryAPI.so (built with BERRY_BUILD_SHARED), a constructor
// registers it after the engine bootstrap.
// ---------------------------------------------------------------------------
#if defined(BERRY_BUILD_SHARED) && (defined(__clang__) || defined(__GNUC__))
__attribute__((constructor))
static void RegisterSampleHitmarker() {
  using namespace Berry;
  Engine& e = Engine::Instance();
  // Re-register with the sample appended. If the engine already registered
  // defaults, this just adds one more module.
  auto& reg = e.Modules();
  if (!e.attached()) {
    auto defaults = CreateDefaultModules();
    defaults.push_back(std::make_shared<Berry::Samples::HitmarkerModule>());
    for (auto& m : defaults) reg.Register(m);
  }
}
#endif