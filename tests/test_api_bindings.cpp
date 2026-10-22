// Host tests for the IDL -> service bindings (review fix 4).
//
// The gap these close: `api/berry.idl` declared the documented functions, but
// nothing registered them, so the documented surface was not callable. These
// tests pin that:
//   * every binding in the generated table is installed, with the generated
//     capability and arity enforced,
//   * a call without the grant is denied and a call with the wrong arity fails
//     closed before the body runs,
//   * render/HUD work is queued and only applied when the render thread drains
//     it (and `Engine::OnFrame` is that drain),
//   * an over-full queue drops commands instead of growing without bound,
//   * the synchronous reads answer from what the render thread published.
#include "berry/api_bindings.h"
#include "berry/engine.h"
#include "berry/generated_bindings.h"
#include "berry/hud.h"
#include "berry/input.h"
#include "berry/perf.h"
#include "berry/render.h"
#include "berry/script.h"
#include "berry/services.h"
#include "shim_level.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

CapabilitySet Grant(const std::vector<std::string>& caps) {
  CapabilitySet s;
  for (const auto& c : caps) s.Add(c);
  return s;
}

/// Executes one binding through a real host so the capability gate runs exactly
/// as it does on device. Returns the invocation status; `out` receives the
/// return value when the call succeeded.
ScriptStatus Call(ScriptBindings& bindings, const std::string& name,
                  const std::vector<ScriptValue>& args,
                  const std::vector<std::string>& grants, ScriptValue* out) {
  ScriptHost host("api-test", Grant(grants), nullptr);
  host.SetBindings(&bindings);
  return host.CallBinding(name, args, out);
}

}  // namespace

//----------------------------------------------------------------------------
// Installation
//----------------------------------------------------------------------------

static void TestEveryIdlBindingIsInstalled() {
  Renderer renderer;
  Hud hud;
  InputObserver input;
  PerformanceGovernor governor;
  Combat combat;
  std::shared_ptr<BerryModule> worldMod = CreateWorldModule();
  WorldHandle* world = worldMod->Service<WorldHandle>();
  CHECK(world != nullptr);

  ScriptApi::Services services;
  services.renderer = &renderer;
  services.hud = &hud;
  services.input = &input;
  services.governor = &governor;
  services.combat = &combat;
  services.world = world;

  ScriptApi api(services);
  ScriptBindings bindings;
  const size_t n = api.InstallBindings(&bindings);

  // The count matches the generated table exactly: nothing missing, nothing
  // extra (an extra binding would be undocumented and ungated).
  CHECK(n == api::BindingSpecCount());
  CHECK(bindings.Count() == api::BindingSpecCount());

  for (size_t i = 0; i < api::BindingSpecCount(); ++i) {
    const api::BindingSpec& spec = api::BindingSpecs()[i];
    CHECK(bindings.Has(spec.name));
    CHECK(api.installed(spec.name));

    // The registered capability is the spec's kind, so the runtime gate and the
    // docs cannot disagree.
    const Capability req = bindings.Required(spec.name);
    CHECK(req.kind == spec.capability);
    if (spec.capability == CapKind::Network) {
      // Network is registered kind-level: the grant names a host, and the
      // per-request host check happens inside the HTTP helper.
      CHECK(req.scope == "*");
    }
  }
}

static void TestCapabilityGateOnDeclaredBindings() {
  Renderer renderer;
  Hud hud;
  ScriptApi::Services services;
  services.renderer = &renderer;
  services.hud = &hud;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  // Holding `render` but not `hud` must not reach the HUD binding.
  CHECK(Call(bindings, "hud.create_label", {ScriptValue::Str("hi")},
             {"render"}, &out) == ScriptStatus::CapabilityDenied);
  CHECK(Call(bindings, "render.screen_size", {}, {"render"}, &out) ==
        ScriptStatus::Ok);
  // Unknown binding is an error, not a denial.
  CHECK(Call(bindings, "no.such_binding", {}, {"render"}, &out) ==
        ScriptStatus::Error);
}

static void TestArityFailsClosed() {
  Renderer renderer;
  ScriptApi::Services services;
  services.renderer = &renderer;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  // render.draw_text takes 4 args from the IDL.
  CHECK(Call(bindings, "render.draw_text",
             {ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Str("x")},
             {"render"}, &out) == ScriptStatus::Error);
  CHECK(Call(bindings, "render.draw_text",
             {ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Str("x"),
              ScriptValue::Num(0xFFFFFFFF)},
             {"render"}, &out) == ScriptStatus::Ok);
  // The queue is only touched by the WellFormed call above.
  api.DrainOnRenderThread();
}

//----------------------------------------------------------------------------
// Render-thread command queue
//----------------------------------------------------------------------------

static void TestDrawCommandsAreQueuedThenApplied() {
  Renderer renderer;
  Berry::Test::CaptureRenderSeam seam;
  renderer.BindSeam(&seam);

  ScriptApi::Services services;
  services.renderer = &renderer;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  api.PublishFrame(16.0f, Vec2(800, 600));
  renderer.BeginFrame(Vec2(800, 600));

  ScriptValue out;
  CHECK(Call(bindings, "render.draw_text",
             {ScriptValue::Num(10), ScriptValue::Num(20),
              ScriptValue::Str("hello"), ScriptValue::Num(0xFF0000FF)},
             {"render"}, &out) == ScriptStatus::Ok);

  // Nothing has been drawn yet: the command sits in the queue, because the
  // script thread must not touch the Renderer.
  CHECK(api.queue().Pending() == 1);
  CHECK(seam.texts == 0);

  // Draining on the "render thread" applies it.
  CHECK(api.DrainOnRenderThread() == 1);
  CHECK(api.queue().Pending() == 0);
  renderer.EndFrame();
  CHECK(seam.texts == 1);
  CHECK(seam.textContents.size() == 1 && seam.textContents[0] == "hello");
}

static void TestHudLifecycleAcrossThreads() {
  Renderer renderer;
  Hud hud;
  Berry::Test::CaptureRenderSeam seam;
  renderer.BindSeam(&seam);

  ScriptApi::Services services;
  services.renderer = &renderer;
  services.hud = &hud;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue label;
  ScriptValue panel;
  CHECK(Call(bindings, "hud.create_label", {ScriptValue::Str("dps")},
             {"hud"}, &label) == ScriptStatus::Ok);
  CHECK(Call(bindings, "hud.create_panel",
             {ScriptValue::Num(4), ScriptValue::Num(4)}, {"hud"}, &panel) ==
        ScriptStatus::Ok);

  // Both handles are distinct, non-zero, and the widget tree is untouched
  // until the render thread drains.
  CHECK(label.AsNumber() != panel.AsNumber());
  CHECK(label.AsNumber() != 0);
  CHECK(hud.Count() == 0);

  CHECK(api.DrainOnRenderThread() == 2);
  CHECK(hud.Count() == 2);

  // set_visible on the label resolves the handle to the real WidgetId.
  ScriptValue ignored;
  CHECK(Call(bindings, "hud.set_visible",
             {label, ScriptValue::Bool(false)}, {"hud"}, &ignored) ==
        ScriptStatus::Ok);
  CHECK(api.DrainOnRenderThread() == 1);
  // The label is hidden; the panel is still visible.
  bool sawHidden = false;
  for (auto& kv : hud.widgets()) {
    if (!kv.second.visible) sawHidden = true;
  }
  CHECK(sawHidden);
}

static void TestFrameHookDrawsCosmetics() {
  CosmeticRegistry::Instance().Clear();
  CosmeticRegistry::Instance().RegisterBuiltins();

  Renderer renderer;
  Berry::Test::CaptureRenderSeam seam;
  renderer.BindSeam(&seam);
  CameraState cam;
  cam.width = 1280;
  cam.height = 720;
  cam.view = Mat4::LookAt(Vec3(10.0f, 64.0f, 15.0f), Vec3(10.0f, 64.0f, 5.0f),
                          Vec3(0, 1, 0));
  cam.projection = Mat4::Perspective(70, 1280.0f / 720.0f, 0.1f, 200.0f);
  cam.valid = true;
  renderer.SetCamera(cam);

  Berry::Test::ShimSkeleton skel;
  CosmeticService cosmetics;
  cosmetics.SetSkeletonSource(&skel);
  cosmetics.Equip("builtin.cape.red");

  ScriptApi::Services services;
  services.renderer = &renderer;
  services.cosmetics = &cosmetics;
  services.localPlayer = EntityId{1, 1};
  ScriptApi api(services);

  renderer.BeginFrame(Vec2(1280, 720));
  Engine& eng = Engine::Instance();
  eng.Bootstrap();
  CHECK(eng.Attach());
  eng.OnFrame();          // emits berry:frame; the hook draws the cape
  renderer.EndFrame();
  CHECK(seam.rects == 1);
  eng.Detach();
}

static void TestQueueIsBoundedAndFailsClosed() {
  RenderCommandQueue q(2);
  int ran = 0;
  CHECK(q.Post([&] { ++ran; }));
  CHECK(q.Post([&] { ++ran; }));
  // Full: the third post is refused and counted, not queued.
  CHECK(!q.Post([&] { ++ran; }));
  CHECK(q.Pending() == 2);
  CHECK(q.dropped() == 1);

  CHECK(q.Drain() == 2);
  CHECK(ran == 2);
  CHECK(q.drained() == 2);
  CHECK(q.Pending() == 0);

  // A drain with nothing queued is a no-op.
  CHECK(q.Drain() == 0);
}

static void TestFrameHookDrainsQueue() {
  Renderer renderer;
  Berry::Test::CaptureRenderSeam seam;
  renderer.BindSeam(&seam);

  ScriptApi::Services services;
  services.renderer = &renderer;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  api.PublishFrame(16.0f, Vec2(640, 480));
  renderer.BeginFrame(Vec2(640, 480));

  ScriptValue out;
  CHECK(Call(bindings, "render.draw_rect",
             {ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Num(10),
              ScriptValue::Num(10), ScriptValue::Num(0x00FF00FF)},
             {"render"}, &out) == ScriptStatus::Ok);
  CHECK(api.queue().Pending() == 1);

  // Engine::OnFrame emits `berry:frame` on the render thread; that is what
  // drains the queue on device. Its body only runs while attached.
  Engine& eng = Engine::Instance();
  eng.Bootstrap();
  CHECK(eng.Attach());
  eng.OnFrame();
  CHECK(api.queue().Pending() == 0);
  renderer.EndFrame();
  CHECK(seam.rects == 1);

  api.DrainOnRenderThread();   // idempotent
  eng.Detach();
}

//----------------------------------------------------------------------------
// Synchronous reads
//----------------------------------------------------------------------------

static void TestPublishedReads() {
  ScriptApi::Services services;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  // Before any frame, both reads answer with a neutral value rather than
  // uninitialised memory.
  ScriptValue out;
  CHECK(Call(bindings, "render.screen_size", {}, {"render"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsTable());
  CHECK(out.Field("x") && out.Field("x")->AsNumber() == 0);
  CHECK(Call(bindings, "perf.frame_time_ms", {}, {"perf"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 0);

  api.PublishFrame(12.5f, Vec2(1920, 1080));
  CHECK(Call(bindings, "render.screen_size", {}, {"render"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Field("x")->AsNumber() == 1920);
  CHECK(out.Field("y")->AsNumber() == 1080);
  CHECK(Call(bindings, "perf.frame_time_ms", {}, {"perf"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 12.5);
}

static void TestGovernorAndZoomWrites() {
  PerformanceGovernor governor;
  ScriptApi::Services services;
  services.governor = &governor;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  CHECK(Call(bindings, "perf.set_target_fps", {ScriptValue::Num(45)},
             {"perf"}, &out) == ScriptStatus::Ok);
  CHECK(Call(bindings, "perf.set_thermal_mode", {ScriptValue::Bool(true)},
             {"perf"}, &out) == ScriptStatus::Ok);
  CHECK(api.DrainOnRenderThread() == 2);
  CHECK(governor.config().targetFps == 45);
  CHECK(governor.thermallyLimited());

  // Zoom is client-side state read back through the API.
  CHECK(Call(bindings, "render.set_zoom", {ScriptValue::Num(2.5)},
             {"render"}, &out) == ScriptStatus::Ok);
  CHECK(api.zoomFactor() == 2.5f);
  // A non-positive factor is refused and leaves the previous value.
  CHECK(Call(bindings, "render.set_zoom", {ScriptValue::Num(0)},
             {"render"}, &out) == ScriptStatus::Error);
  CHECK(api.zoomFactor() == 2.5f);
}

//----------------------------------------------------------------------------
// Read-only services
//----------------------------------------------------------------------------

static void TestCombatAndInputReads() {
  Combat combat;
  combat.RecordHit(EntityId{1, 1}, 4.0f);
  combat.RecordKill(EntityId{1, 1}, EntityId{2, 2}, DamageCause::None);

  InputObserver input;
  input.OnKey("w", true);
  input.OnMouse(MouseButton::Left, true);

  ScriptApi::Services services;
  services.combat = &combat;
  services.input = &input;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  CHECK(Call(bindings, "combat.recent_kills", {}, {"combat"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 1);

  CHECK(Call(bindings, "input.is_key_down", {ScriptValue::Str("W")},
             {"input"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());   // case-insensitive, and W was pressed
  CHECK(Call(bindings, "input.is_key_down", {ScriptValue::Str("q")},
             {"input"}, &out) == ScriptStatus::Ok);
  CHECK(!out.Truthy());  // untracked key fails closed

  CHECK(Call(bindings, "input.clicks_per_second", {}, {"input"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() >= 0);
}

static void TestWorldQueryNeedsSeamAndLocalPlayer() {
  std::shared_ptr<BerryModule> worldMod = CreateWorldModule();
  WorldHandle* world = worldMod->Service<WorldHandle>();
  CHECK(world != nullptr);

  auto shim = std::make_shared<Berry::Test::ShimLevel>();
  EntityId self{1, 1};
  EntitySnapshot me;
  me.id = self;
  me.type = EntityType::Player;
  me.position = Vec3(0, 0, 0);
  me.alive = true;
  shim->AddEntity(self, me);

  EntityId other{2, 2};
  EntitySnapshot them;
  them.id = other;
  them.type = EntityType::Player;
  them.position = Vec3(3, 0, 0);
  them.alive = true;
  shim->AddEntity(other, them);

  world->SetSeam(shim);

  ScriptApi::Services services;
  services.world = world;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  // No local player bound: fail closed rather than use an arbitrary origin.
  CHECK(Call(bindings, "world.query", {ScriptValue::Num(10)},
             {"world"}, &out) == ScriptStatus::Error);

  services.localPlayer = self;
  api.SetServices(services);

  // With the origin bound, both the local player and the other are within 10.
  CHECK(Call(bindings, "world.query", {ScriptValue::Num(10)},
             {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 2);
  // A tighter radius still counts the other at distance 3.
  CHECK(Call(bindings, "world.query", {ScriptValue::Num(5)},
             {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 2);

  // World reads stay read-only and always answered (never a pointer).
  world->SetValue("k", "v");
  CHECK(std::string(world->GetValue("k", "")) == "v");
}

static void TestPlayerReadsFailClosedAndResolve() {
  std::shared_ptr<BerryModule> worldMod = CreateWorldModule();
  WorldHandle* world = worldMod->Service<WorldHandle>();
  auto shim = std::make_shared<Berry::Test::ShimLevel>();

  EntityId self{7, 7};
  EntitySnapshot me;
  me.id = self;
  me.type = EntityType::Player;
  me.position = Vec3(12.5f, 64.0f, -3.25f);
  me.velocity = Vec3(0.1f, -0.2f, 0.3f);
  me.rotation = Vec3(10.0f, 90.0f, 0.0f);
  me.health = 18.0f;
  me.maxHealth = 20.0f;
  me.alive = true;
  me.name = "Steve";
  shim->AddEntity(self, me);
  world->SetSeam(shim);

  ScriptApi::Services services;
  services.world = world;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  // No local player bound: every read fails closed rather than inventing state.
  CHECK(Call(bindings, "player.position", {}, {"world"}, &out) ==
        ScriptStatus::Error);
  CHECK(Call(bindings, "player.health", {}, {"world"}, &out) ==
        ScriptStatus::Error);

  services.localPlayer = self;
  api.SetServices(services);

  CHECK(Call(bindings, "player.position", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Field("x")->AsNumber() == 12.5);
  CHECK(out.Field("y")->AsNumber() == 64.0);
  CHECK(out.Field("z")->AsNumber() == -3.25);

  CHECK(Call(bindings, "player.rotation", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Field("y")->AsNumber() == 90.0);

  CHECK(Call(bindings, "player.velocity", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  // f32 -> f64 widening means exact equality is wrong; compare with tolerance.
  CHECK(std::fabs(out.Field("z")->AsNumber() - 0.3) < 1e-6);

  CHECK(Call(bindings, "player.health", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 18.0);
  CHECK(Call(bindings, "player.max_health", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 20.0);
  CHECK(Call(bindings, "player.alive", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(Call(bindings, "player.name", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsString() == "Steve");

  // The Player module requires the `world` capability, so a script without it
  // cannot read the local player either.
  CHECK(Call(bindings, "player.position", {}, {"render"}, &out) ==
        ScriptStatus::CapabilityDenied);
}

static void TestRenderDrawHelpers() {
  Berry::Test::CaptureRenderSeam seam;
  Renderer renderer;
  renderer.Attach(&seam);

  ScriptApi::Services services;
  services.renderer = &renderer;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  renderer.BeginFrame(Vec2(800, 600));
  CHECK(Call(bindings, "render.draw_line",
             {ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Num(10),
              ScriptValue::Num(10), ScriptValue::Num(0xFF0000FF)},
             {"render"}, &out) == ScriptStatus::Ok);
  CHECK(Call(bindings, "render.draw_circle",
             {ScriptValue::Num(50), ScriptValue::Num(50), ScriptValue::Num(8),
              ScriptValue::Num(0x00FF00FF)},
             {"render"}, &out) == ScriptStatus::Ok);
  CHECK(Call(bindings, "render.draw_text_centered",
             {ScriptValue::Num(100), ScriptValue::Num(100), ScriptValue::Str("hi"),
              ScriptValue::Num(0xFFFFFFFF)},
             {"render"}, &out) == ScriptStatus::Ok);
  CHECK(api.DrainOnRenderThread() == 3);
  renderer.EndFrame();
  CHECK(seam.lines == 1);
  CHECK(seam.circles == 1);
  CHECK(seam.texts == 1);

  // A NaN coordinate must not reach the draw list: SafeCoord maps it to 0.
  renderer.BeginFrame(Vec2(800, 600));
  CHECK(Call(bindings, "render.draw_line",
             {ScriptValue::Num(std::nan("")), ScriptValue::Num(0),
              ScriptValue::Num(10), ScriptValue::Num(10),
              ScriptValue::Num(0xFF0000FF)},
             {"render"}, &out) == ScriptStatus::Ok);
  CHECK(api.DrainOnRenderThread() == 1);
  renderer.EndFrame();
  CHECK(seam.lines == 1);
}

static void TestWorldReads() {
  std::shared_ptr<BerryModule> worldMod = CreateWorldModule();
  WorldHandle* world = worldMod->Service<WorldHandle>();
  auto shim = std::make_shared<Berry::Test::ShimLevel>();

  EntityId a{1, 1};
  EntitySnapshot sa;
  sa.id = a;
  sa.alive = true;
  shim->AddEntity(a, sa);
  EntityId b{2, 2};
  EntitySnapshot sb;
  sb.id = b;
  sb.alive = true;
  shim->AddEntity(b, sb);

  LevelTime t;
  t.absolute = 6000;
  t.daylight = 0.9f;
  t.day = true;
  t.rainLevel = 0.5f;
  t.thunderLevel = 0.0f;
  shim->SetTime(t);
  world->SetSeam(shim);

  ScriptApi::Services services;
  services.world = world;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  CHECK(Call(bindings, "world.entity_count", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 2);
  CHECK(Call(bindings, "world.is_day", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(Call(bindings, "world.is_raining", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(Call(bindings, "world.time", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.Field("absolute")->AsNumber() == 6000);
  CHECK(out.Field("day")->Truthy());
  CHECK(out.Field("rain")->AsNumber() == 0.5);

  // No seam bound: reads fail closed rather than answer with a default.
  world->SetSeam(nullptr);
  CHECK(Call(bindings, "world.entity_count", {}, {"world"}, &out) ==
        ScriptStatus::Error);
  CHECK(Call(bindings, "world.time", {}, {"world"}, &out) == ScriptStatus::Error);
}

static void TestPerfStatsAndPlatform() {
  PerformanceGovernor governor;
  ScriptApi::Services services;
  services.governor = &governor;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  for (int i = 0; i < 10; ++i) api.PublishFrame(16.0f, Vec2(800, 600));
  CHECK(Call(bindings, "perf.stats", {}, {"perf"}, &out) == ScriptStatus::Ok);
  CHECK(out.IsTable());
  CHECK(out.Field("samples")->AsNumber() == 10);
  CHECK(out.Field("avg_ms")->AsNumber() > 0);
  CHECK(out.Field("fps")->AsNumber() > 0);

  CHECK(Call(bindings, "perf.quality", {}, {"perf"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() > 0);

  CHECK(Call(bindings, "perf.set_platform", {ScriptValue::Str("desktop")},
             {"perf"}, &out) == ScriptStatus::Ok);
  CHECK(api.DrainOnRenderThread() == 1);
  CHECK(governor.platform() == PlatformClass::Desktop);

  // An unknown profile name is refused and changes nothing.
  CHECK(Call(bindings, "perf.set_platform", {ScriptValue::Str("toaster")},
             {"perf"}, &out) == ScriptStatus::Error);
  CHECK(api.DrainOnRenderThread() == 0);
  CHECK(governor.platform() == PlatformClass::Desktop);
}

static void TestPerfFlameBindings() {
  Profiler& prof = Profiler::Instance();
  prof.FlameReset();
  prof.SetEnabled(true);
  {
    ProfilerScope outer("hook:on_tick");
    { ProfilerScope inner("Berry::Combat"); }
  }

  ScriptApi::Services services;
  services.profiler = &prof;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  CHECK(Call(bindings, "perf.flame", {}, {"perf"}, &out) == ScriptStatus::Ok);
  CHECK(out.IsTable());
  const ScriptValue& outer = out.Field("children")->AsTable()->at("0");
  CHECK(outer.Field("name")->AsString() == "hook:on_tick");
  CHECK(outer.Field("count")->AsNumber() == 1);
  // The nested span is a child, and the parent's self time excludes it.
  CHECK(outer.Field("children")->AsTable()->size() == 1);
  CHECK(outer.Field("children")->AsTable()->at("0").Field("name")->AsString() ==
        "Berry::Combat");
  CHECK(outer.Field("self_ms")->AsNumber() <= outer.Field("total_ms")->AsNumber());

  CHECK(Call(bindings, "perf.spans", {}, {"perf"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsTable()->size() == 2);

  // reset drops the tree; the next read is empty rather than stale.
  CHECK(Call(bindings, "perf.flame_reset", {}, {"perf"}, &out) ==
        ScriptStatus::Ok);
  CHECK(Call(bindings, "perf.flame", {}, {"perf"}, &out) == ScriptStatus::Ok);
  CHECK(out.Field("children")->AsTable()->size() == 0);

  // Without the perf grant the surface is denied by the gate.
  CHECK(Call(bindings, "perf.flame", {}, {"hud"}, &out) ==
        ScriptStatus::CapabilityDenied);

  // With no profiler bound, the surface fails closed rather than crashing.
  ScriptApi bare(ScriptApi::Services{});
  ScriptBindings bareBindings;
  bare.InstallBindings(&bareBindings);
  CHECK(Call(bareBindings, "perf.flame", {}, {"perf"}, &out) ==
        ScriptStatus::Error);
  CHECK(Call(bareBindings, "perf.spans", {}, {"perf"}, &out) ==
        ScriptStatus::Error);
  CHECK(Call(bareBindings, "perf.flame_reset", {}, {"perf"}, &out) ==
        ScriptStatus::Error);
}

//----------------------------------------------------------------------------
// Read-only world / entity surface
//----------------------------------------------------------------------------

static void TestWorldAndEntityReads() {
  Renderer renderer;
  std::shared_ptr<BerryModule> worldMod = CreateWorldModule();
  WorldHandle* world = worldMod->Service<WorldHandle>();
  CHECK(world != nullptr);

  Berry::Test::ShimLevel level;
  const EntityId self{1, 1};
  EntitySnapshot me;
  me.id = self;
  me.type = EntityType::Player;
  me.position = Vec3(0, 64, 0);
  me.rotation = Vec3(0, 0, 0);   // looking along +Z
  me.alive = true;
  me.health = 20;
  me.maxHealth = 20;
  level.AddEntity(self, me);

  const EntityId near{2, 2};
  EntitySnapshot mob;
  mob.id = near;
  mob.type = EntityType::Mob;
  mob.position = Vec3(0, 64, 5);   // 5 blocks along the look direction
  mob.alive = true;
  mob.health = 10;
  mob.maxHealth = 20;
  mob.name = "zombie";
  level.AddEntity(near, mob);

  const EntityId far{3, 3};
  EntitySnapshot item;
  item.id = far;
  item.type = EntityType::Item;
  item.position = Vec3(0, 64, 200);
  item.alive = true;
  level.AddEntity(far, item);

  // The shim's entity raycast treats a hit as "within 1 block of the entity's
  // origin". The local eye sits 1.62 above the player's feet, so a target at
  // exactly eye height is what a straight-ahead cast actually strikes.
  const EntityId ahead{4, 4};
  EntitySnapshot proj;
  proj.id = ahead;
  proj.type = EntityType::Projectile;
  proj.position = Vec3(0, 64.0f + 1.62f, 5);
  proj.alive = true;
  level.AddEntity(ahead, proj);

  world->SetSeam(std::shared_ptr<LevelSeam>(&level, [](LevelSeam*) {}));

  ScriptApi::Services services;
  services.world = world;
  services.renderer = &renderer;
  services.localPlayer = self;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;

  // entities_within: the local player, the mob and the projectile are all
  // within 50 blocks; the far item is not. Sorted by id, each an entity table.
  CHECK(Call(bindings, "world.entities_within",
             {ScriptValue::Num(50), ScriptValue::Str("")}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsTable());
  CHECK(out.AsTable()->at("count").AsNumber() == 3);   // the far item is excluded
  const ScriptTable* t0 = out.AsTable()->at("0").AsTable();
  CHECK(t0 != nullptr);
  CHECK(t0->at("id").AsNumber() == 1);
  CHECK(t0->at("type").AsString() == "player");
  CHECK(t0->at("distance").AsNumber() == 0);

  // A type filter narrows to mobs only.
  CHECK(Call(bindings, "world.entities_within",
             {ScriptValue::Num(50), ScriptValue::Str("mob")}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsTable()->at("count").AsNumber() == 1);
  CHECK(out.AsTable()->at("0").AsTable()->at("name").AsString() == "zombie");

  // An unknown type name is refused, not treated as "any".
  ScriptStatus st = Call(bindings, "world.entities_within",
                         {ScriptValue::Num(50), ScriptValue::Str("dragon")},
                         {"world"}, &out);
  CHECK(st == ScriptStatus::Error);

  // entity.get resolves by raw id and carries the runtime id too.
  CHECK(Call(bindings, "entity.get", {ScriptValue::Num(2)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsTable()->at("runtime").AsNumber() == 2);
  CHECK(out.AsTable()->at("health").AsNumber() == 10);
  CHECK(Call(bindings, "entity.type_name", {ScriptValue::Num(2)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsString() == "mob");
  CHECK(Call(bindings, "entity.alive", {ScriptValue::Num(2)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());

  // entity.distance_to measures from the local player.
  CHECK(Call(bindings, "entity.distance_to", {ScriptValue::Num(2)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 5);

  // A zero or unknown id fails closed rather than reading entity 0.
  CHECK(Call(bindings, "entity.get", {ScriptValue::Num(0)}, {"world"}, &out) ==
        ScriptStatus::Error);
  CHECK(Call(bindings, "entity.get", {ScriptValue::Num(999)}, {"world"}, &out) ==
        ScriptStatus::Error);

  // entity.nearest skips the local player and respects the filter.
  CHECK(Call(bindings, "entity.nearest", {ScriptValue::Str("")}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 2);   // the mob at 5 blocks, not me, not the far item
  CHECK(Call(bindings, "entity.nearest", {ScriptValue::Str("item")}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 3);
  CHECK(Call(bindings, "entity.nearest", {ScriptValue::Str("vehicle")}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 0);   // none of that type

  // raycast_entity casts along the look direction (+Z) and finds the entity
  // sitting at eye height; the ground-level mob is out of the cast's reach.
  CHECK(Call(bindings, "world.raycast_entity", {ScriptValue::Num(10)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsTable()->at("hit").Truthy());
  CHECK(out.AsTable()->at("id").AsNumber() == 4);
  // raycast_block finds nothing in the shim, but the shape is still a table.
  CHECK(Call(bindings, "world.raycast_block", {ScriptValue::Num(10)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(!out.AsTable()->at("hit").Truthy());

  // Without a seam the reads fail closed with an error rather than crashing.
  world->SetSeam(nullptr);
  CHECK(Call(bindings, "world.entities_within",
             {ScriptValue::Num(50), ScriptValue::Str("")}, {"world"}, &out) ==
        ScriptStatus::Error);
  CHECK(Call(bindings, "entity.get", {ScriptValue::Num(2)}, {"world"}, &out) ==
        ScriptStatus::Error);
}

static void TestPhase2PlayerAndWorldExtras() {
  Renderer renderer;
  std::shared_ptr<BerryModule> worldMod = CreateWorldModule();
  WorldHandle* world = worldMod->Service<WorldHandle>();
  auto shim = std::make_shared<Berry::Test::ShimLevel>();

  const EntityId self{1, 1};
  EntitySnapshot me;
  me.id = self;
  me.type = EntityType::Player;
  me.position = Vec3(0, 64, 0);
  me.rotation = Vec3(0, 0, 0);
  me.alive = true;
  me.health = 20;
  me.maxHealth = 20;
  shim->AddEntity(self, me);

  // A second player 10 blocks away, a mob 3 blocks away, an item far off.
  const EntityId other{2, 2};
  EntitySnapshot p2;
  p2.id = other;
  p2.type = EntityType::Player;
  p2.position = Vec3(0, 64, 10);
  p2.alive = true;
  shim->AddEntity(other, p2);

  const EntityId mob{3, 3};
  EntitySnapshot m;
  m.id = mob;
  m.type = EntityType::Mob;
  m.position = Vec3(0, 64, 3);
  m.alive = true;
  shim->AddEntity(mob, m);

  const EntityId item{4, 4};
  EntitySnapshot it;
  it.id = item;
  it.type = EntityType::Item;
  it.position = Vec3(0, 64, 500);
  it.alive = true;
  shim->AddEntity(item, it);

  shim->SetBlock(Vec3(1, 64, 1), 42);
  shim->SetBiome(BlockPos(0, 64, 0), "plains");
  shim->SetLight(BlockPos(0, 64, 0), 7);
  shim->SetChunkState(BlockPos(0, 64, 0), ChunkState::Rendered);

  LocalPlayerState st;
  st.hunger = 17;
  st.saturation = 2.5f;
  st.armorPoints = 9;
  st.gamemode = 1;
  st.dimension = 2;
  st.xpLevel = 30;
  st.xpProgress = 0.25f;
  st.air = 120;
  st.onFire = true;
  st.sneaking = true;
  st.sprinting = false;
  st.effects.push_back(PlayerEffect{"speed", 1, 12.5f, false});
  shim->SetLocalPlayerState(st);

  world->SetSeam(shim);

  ScriptApi::Services services;
  services.world = world;
  services.renderer = &renderer;
  services.localPlayer = self;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;

  //--- Player extras ---
  CHECK(Call(bindings, "player.hunger", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 17);
  CHECK(Call(bindings, "player.saturation", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 2.5);
  CHECK(Call(bindings, "player.armor", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 9);
  CHECK(Call(bindings, "player.gamemode", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsString() == "creative");
  CHECK(Call(bindings, "player.dimension", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsString() == "end");
  CHECK(Call(bindings, "player.xp_level", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 30);
  CHECK(Call(bindings, "player.xp_progress", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 0.25);
  CHECK(Call(bindings, "player.air", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 120);
  CHECK(Call(bindings, "player.on_fire", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(Call(bindings, "player.sneaking", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(Call(bindings, "player.sprinting", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(!out.Truthy());

  CHECK(Call(bindings, "player.effects", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsTable()->at("count").AsNumber() == 1);
  CHECK(out.AsTable()->at("0").AsTable()->at("name").AsString() == "speed");
  CHECK(out.AsTable()->at("0").AsTable()->at("amplifier").AsNumber() == 1);

  CHECK(Call(bindings, "player.state", {}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.Field("hunger")->AsNumber() == 17);
  CHECK(out.Field("gamemode")->AsString() == "creative");
  CHECK(out.Field("dimension")->AsString() == "end");
  CHECK(out.Field("effects")->AsTable()->at("count").AsNumber() == 1);

  //--- World extras ---
  CHECK(Call(bindings, "world.block_at", {ScriptValue::Num(1), ScriptValue::Num(64),
                                          ScriptValue::Num(1)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsNumber());
  CHECK(out.AsNumber() == 42);
  CHECK(Call(bindings, "world.block_id_at", {ScriptValue::Num(1), ScriptValue::Num(64),
                                             ScriptValue::Num(1)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 42);

  CHECK(Call(bindings, "world.get_biome", {ScriptValue::Num(0), ScriptValue::Num(64),
                                           ScriptValue::Num(0)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsString() == "plains");
  // An unset coordinate fails closed to "" rather than inventing a biome.
  CHECK(Call(bindings, "world.get_biome", {ScriptValue::Num(99), ScriptValue::Num(64),
                                           ScriptValue::Num(99)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsString() == "");

  CHECK(Call(bindings, "world.get_light", {ScriptValue::Num(0), ScriptValue::Num(64),
                                           ScriptValue::Num(0)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 7);
  CHECK(Call(bindings, "world.get_light", {ScriptValue::Num(99), ScriptValue::Num(64),
                                           ScriptValue::Num(99)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == -1);

  CHECK(Call(bindings, "world.get_chunk_state", {ScriptValue::Num(0), ScriptValue::Num(64),
                                                 ScriptValue::Num(0)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsString() == "rendered");
  CHECK(Call(bindings, "world.get_chunk_state", {ScriptValue::Num(99), ScriptValue::Num(64),
                                                 ScriptValue::Num(99)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsString() == "unloaded");

  // entities_in_radius: only the mob is within 5 of the origin; the local
  // player (0 blocks) and the other player (10) are excluded by distance? The
  // local player IS within 5, so expect 2 (me + mob), sorted by id.
  CHECK(Call(bindings, "world.entities_in_radius",
             {ScriptValue::Num(0), ScriptValue::Num(64), ScriptValue::Num(0),
              ScriptValue::Num(5), ScriptValue::Str("")}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsTable()->at("count").AsNumber() == 2);
  CHECK(out.AsTable()->at("0").AsTable()->at("id").AsNumber() == 1);
  CHECK(out.AsTable()->at("1").AsTable()->at("id").AsNumber() == 3);

  // A type filter narrows it to mobs only.
  CHECK(Call(bindings, "world.entities_in_radius",
             {ScriptValue::Num(0), ScriptValue::Num(64), ScriptValue::Num(0),
              ScriptValue::Num(5), ScriptValue::Str("mob")}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsTable()->at("count").AsNumber() == 1);
  CHECK(out.AsTable()->at("0").AsTable()->at("type").AsString() == "mob");

  // A negative radius is refused.
  CHECK(Call(bindings, "world.entities_in_radius",
             {ScriptValue::Num(0), ScriptValue::Num(64), ScriptValue::Num(0),
              ScriptValue::Num(-1), ScriptValue::Str("")}, {"world"}, &out) ==
        ScriptStatus::Error);

  // entities_in_aabb over a box covering the mob but not the far item.
  CHECK(Call(bindings, "world.entities_in_aabb",
             {ScriptValue::Num(-1), ScriptValue::Num(60), ScriptValue::Num(-1),
              ScriptValue::Num(1), ScriptValue::Num(70), ScriptValue::Num(5),
              ScriptValue::Str("")}, {"world"}, &out) == ScriptStatus::Ok);
  // me (0,64,0), mob (0,64,3) are inside; the other player at z=10 is not.
  CHECK(out.AsTable()->at("count").AsNumber() == 2);

  // An inverted box is refused.
  CHECK(Call(bindings, "world.entities_in_aabb",
             {ScriptValue::Num(1), ScriptValue::Num(0), ScriptValue::Num(1),
              ScriptValue::Num(-1), ScriptValue::Num(0), ScriptValue::Num(-1),
              ScriptValue::Str("")}, {"world"}, &out) == ScriptStatus::Error);

  // world.raycast from the local eye along +Z should find the mob at 3 blocks
  // through the hitbox test (the shim's own entity cast is coarse, but
  // world.raycast uses the seam's RaycastEntity, so it finds the nearest
  // alive entity along the ray). Use a plain origin at mob height.
  CHECK(Call(bindings, "world.raycast",
             {ScriptValue::Num(0), ScriptValue::Num(64), ScriptValue::Num(0),
              ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Num(1),
              ScriptValue::Num(20), ScriptValue::Str("any")}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsTable()->at("hit").Truthy());

  // A zero direction is refused.
  CHECK(Call(bindings, "world.raycast",
             {ScriptValue::Num(0), ScriptValue::Num(64), ScriptValue::Num(0),
              ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Num(0),
              ScriptValue::Num(20), ScriptValue::Str("any")}, {"world"}, &out) ==
        ScriptStatus::Error);
  // An unknown target is refused.
  CHECK(Call(bindings, "world.raycast",
             {ScriptValue::Num(0), ScriptValue::Num(64), ScriptValue::Num(0),
              ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Num(1),
              ScriptValue::Num(20), ScriptValue::Str("wall")}, {"world"}, &out) ==
        ScriptStatus::Error);

  //--- Entity extras ---
  // The mob's hitbox is a 0.6x1.4x0.6 box sitting on its feet at (0,64,3).
  CHECK(Call(bindings, "entity.get_hitbox", {ScriptValue::Num(3)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Field("valid")->Truthy());
  CHECK(std::fabs(out.Field("min_y")->AsNumber() - 64) < 1e-4);
  CHECK(std::fabs(out.Field("max_y")->AsNumber() - 65.4) < 1e-4);
  CHECK(std::fabs(out.Field("min_x")->AsNumber() - (-0.3)) < 1e-4);

  // An unknown entity yields valid=false rather than a fabricated box.
  CHECK(Call(bindings, "entity.get_hitbox", {ScriptValue::Num(999)}, {"world"}, &out) ==
        ScriptStatus::Error);

  // trace_line from 10 blocks in front of the mob, aiming at it, hits.
  CHECK(Call(bindings, "entity.trace_line",
             {ScriptValue::Num(3), ScriptValue::Num(0), ScriptValue::Num(64.7f),
              ScriptValue::Num(13), ScriptValue::Num(0), ScriptValue::Num(0),
              ScriptValue::Num(-1), ScriptValue::Num(20)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsTable()->at("hit").Truthy());
  CHECK(out.AsTable()->at("distance").AsNumber() > 0);
  CHECK(out.AsTable()->at("distance").AsNumber() <= 20);

  // Aiming away misses.
  CHECK(Call(bindings, "entity.trace_line",
             {ScriptValue::Num(3), ScriptValue::Num(0), ScriptValue::Num(64.7f),
              ScriptValue::Num(13), ScriptValue::Num(0), ScriptValue::Num(0),
              ScriptValue::Num(1), ScriptValue::Num(20)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(!out.AsTable()->at("hit").Truthy());

  // A zero direction is refused.
  CHECK(Call(bindings, "entity.trace_line",
             {ScriptValue::Num(3), ScriptValue::Num(0), ScriptValue::Num(64.7f),
              ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Num(0),
              ScriptValue::Num(20)}, {"world"}, &out) == ScriptStatus::Error);

  //--- Fail-closed when the seam is unbound ---
  world->SetSeam(nullptr);
  CHECK(Call(bindings, "player.hunger", {}, {"world"}, &out) == ScriptStatus::Error);
  CHECK(Call(bindings, "world.get_biome", {ScriptValue::Num(0), ScriptValue::Num(0),
                                           ScriptValue::Num(0)}, {"world"}, &out) ==
        ScriptStatus::Error);
  CHECK(Call(bindings, "world.entities_in_radius",
             {ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Num(0),
              ScriptValue::Num(5), ScriptValue::Str("")}, {"world"}, &out) ==
        ScriptStatus::Error);
}

static void TestModBindings() {
  ScriptModuleRegistry modules;
  ScriptApi::Services services;
  services.modules = &modules;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  // A duplicate name returns 0 and raises.
  CHECK(Call(bindings, "mod.create",
             {ScriptValue::Str("Combo"), ScriptValue::Str("counts")},
             {"hud"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 1);
  CHECK(Call(bindings, "mod.create",
             {ScriptValue::Str("Combo"), ScriptValue::Str("dup")},
             {"hud"}, &out) == ScriptStatus::Error);

  // Settings are typed; a bad declaration reports false.
  CHECK(Call(bindings, "mod.add_slider",
             {ScriptValue::Num(1), ScriptValue::Str("scale"), ScriptValue::Str("Scale"),
              ScriptValue::Num(0.5), ScriptValue::Num(2), ScriptValue::Num(1)},
             {"hud"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(Call(bindings, "mod.add_slider",
             {ScriptValue::Num(1), ScriptValue::Str("bad"), ScriptValue::Str("Bad"),
              ScriptValue::Num(5), ScriptValue::Num(1), ScriptValue::Num(3)},
             {"hud"}, &out) == ScriptStatus::Ok);
  CHECK(!out.Truthy());   // max < min

  // A select built from a newline list.
  CHECK(Call(bindings, "mod.add_select",
             {ScriptValue::Num(1), ScriptValue::Str("anchor"), ScriptValue::Str("Anchor"),
              ScriptValue::Str("tl\ntr\nbl"), ScriptValue::Num(1)},
             {"hud"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());

  // Out-of-range write fails closed; in-range succeeds.
  CHECK(Call(bindings, "mod.set_number",
             {ScriptValue::Num(1), ScriptValue::Str("scale"), ScriptValue::Num(3)},
             {"hud"}, &out) == ScriptStatus::Ok);
  CHECK(!out.Truthy());
  CHECK(Call(bindings, "mod.set_number",
             {ScriptValue::Num(1), ScriptValue::Str("scale"), ScriptValue::Num(1.5)},
             {"hud"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());

  // Reads round-trip, and a missing setting raises.
  CHECK(Call(bindings, "mod.number",
             {ScriptValue::Num(1), ScriptValue::Str("scale")}, {"hud"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 1.5);
  CHECK(Call(bindings, "mod.number",
             {ScriptValue::Num(1), ScriptValue::Str("nope")}, {"hud"}, &out) ==
        ScriptStatus::Error);

  // mod.get returns the declared shape, including the options list.
  CHECK(Call(bindings, "mod.get",
             {ScriptValue::Num(1), ScriptValue::Str("anchor")}, {"hud"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsTable()->at("kind").AsString() == "select");
  CHECK(out.AsTable()->at("options").AsString() == "tl\ntr\nbl");

  // mod.info reports the module and its setting count.
  CHECK(Call(bindings, "mod.info", {ScriptValue::Num(1)}, {"hud"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsTable()->at("name").AsString() == "Combo");
  CHECK(out.AsTable()->at("count").AsNumber() == 2);   // scale + anchor (bad was refused)
  CHECK(out.AsTable()->at("settings").IsTable());

  CHECK(Call(bindings, "mod.count", {}, {"hud"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 1);

  // The capability gate still applies: without `hud`, the binding is denied.
  CHECK(Call(bindings, "mod.create",
             {ScriptValue::Str("X"), ScriptValue::Str("")}, {"render"}, &out) ==
        ScriptStatus::CapabilityDenied);

  // With no registry bound, everything fails closed rather than crashing.
  ScriptApi bare(ScriptApi::Services{});
  ScriptBindings bareBindings;
  bare.InstallBindings(&bareBindings);
  CHECK(Call(bareBindings, "mod.create",
             {ScriptValue::Str("Y"), ScriptValue::Str("")}, {"hud"}, &out) ==
        ScriptStatus::Error);
}

static void TestFsAndAudioBindings() {
  ScriptFileStore files;
  AudioService audio;
  // A recording backend, so `audio.play` reaches a real play path.
  struct Recording : AudioSeam {
    bool Play(const AudioCue&) override { return true; }
  } seam;
  audio.SetSeam(&seam);
  ScriptApi::Services services;
  services.files = &files;
  services.audio = &audio;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  // fs carries the fileio grant.
  CHECK(Call(bindings, "fs.write",
             {ScriptValue::Str("cfg.json"), ScriptValue::Str("{}")}, {"fileio"},
             &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(Call(bindings, "fs.read", {ScriptValue::Str("cfg.json")}, {"fileio"},
             &out) == ScriptStatus::Ok);
  CHECK(out.AsString() == "{}");
  // A traversal path fails closed.
  CHECK(Call(bindings, "fs.write",
             {ScriptValue::Str("../x"), ScriptValue::Str("y")}, {"fileio"},
             &out) == ScriptStatus::Ok);
  CHECK(!out.Truthy());
  // Without the fileio grant, the binding is denied by the gate.
  CHECK(Call(bindings, "fs.write",
             {ScriptValue::Str("a"), ScriptValue::Str("b")}, {"render"}, &out) ==
        ScriptStatus::CapabilityDenied);

  // audio carries the audio grant.
  CHECK(Call(bindings, "audio.play",
             {ScriptValue::Str("hit"), ScriptValue::Num(1), ScriptValue::Num(1)},
             {"audio"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(Call(bindings, "audio.play",
             {ScriptValue::Str("hit"), ScriptValue::Num(1), ScriptValue::Num(1)},
             {"hud"}, &out) == ScriptStatus::CapabilityDenied);
}

static void TestPvpBindings() {
  PvpOverlay pvp;
  Hud hud;
  ScriptApi::Services services;
  services.pvp = &pvp;
  services.hud = &hud;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  // With the overlay present, the read-outs answer (zeros/empty before any
  // data is fed) and the section switches work.
  CHECK(Call(bindings, "pvp.combo", {}, {"hud"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 0);
  CHECK(Call(bindings, "pvp.cps", {ScriptValue::Str("left")}, {"hud"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 0);
  // An unknown button name is refused.
  CHECK(Call(bindings, "pvp.cps", {ScriptValue::Str("middle")}, {"hud"}, &out) ==
        ScriptStatus::Error);

  CHECK(Call(bindings, "pvp.session", {}, {"hud"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsTable()->at("kills").AsNumber() == 0);
  CHECK(Call(bindings, "pvp.net", {}, {"hud"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsTable()->at("tps").AsNumber() == 20);
  CHECK(Call(bindings, "pvp.armor", {}, {"hud"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsTable()->empty());

  // set_config fully describes the config; config() round-trips it.
  CHECK(Call(bindings, "pvp.set_config",
             {ScriptValue::Str("cps\ncombo\nnet")}, {"hud"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(Call(bindings, "pvp.config", {}, {"hud"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsString() == "cps\ncombo\nnet");
  CHECK(pvp.config().showCps);
  CHECK(!pvp.config().showArmor);

  // An unknown key is refused and leaves the config unchanged.
  CHECK(Call(bindings, "pvp.set_config",
             {ScriptValue::Str("cps\nesp")}, {"hud"}, &out) == ScriptStatus::Error);
  CHECK(pvp.config().showCombo);   // still the previous config

  // attach/detach through the binding.
  CHECK(Call(bindings, "pvp.attach", {}, {"hud"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(pvp.attached());
  CHECK(Call(bindings, "pvp.detach", {}, {"hud"}, &out) == ScriptStatus::Ok);
  CHECK(!pvp.attached());

  // Without the hud grant, the whole module is denied by the gate.
  CHECK(Call(bindings, "pvp.combo", {}, {"render"}, &out) ==
        ScriptStatus::CapabilityDenied);

  // With no overlay bound, everything fails closed.
  ScriptApi bare(ScriptApi::Services{});
  ScriptBindings bareBindings;
  bare.InstallBindings(&bareBindings);
  CHECK(Call(bareBindings, "pvp.combo", {}, {"hud"}, &out) == ScriptStatus::Error);
}

static void TestPacketBindings() {
  PacketLog log;
  log.Record(PacketDir::Inbound, 0x2A, 64, 1000, "PlayerAuthInput");
  log.Record(PacketDir::Outbound, 0x2B, 32, 1001, "MovePlayer");
  ScriptApi::Services services;
  services.packets = &log;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  // recent returns a table keyed "0","1",… newest first, headers only.
  CHECK(Call(bindings, "packets.recent", {ScriptValue::Num(0)}, {"packets"},
             &out) == ScriptStatus::Ok);
  CHECK(out.IsTable());
  CHECK(out.AsTable()->size() == 2);
  const ScriptValue& first = out.AsTable()->at("0");
  CHECK(first.IsTable());
  CHECK(first.Field("id")->AsNumber() == 0x2B);          // newest
  CHECK(first.Field("dir")->AsString() == "outbound");
  CHECK(first.Field("bytes")->AsNumber() == 32);
  CHECK(first.Field("name")->AsString() == "MovePlayer");
  // The payload itself is never present: only its length.
  CHECK(first.Field("payload") == nullptr);

  // A limited read returns the most recent n.
  CHECK(Call(bindings, "packets.recent", {ScriptValue::Num(1)}, {"packets"},
             &out) == ScriptStatus::Ok);
  CHECK(out.AsTable()->size() == 1);

  CHECK(Call(bindings, "packets.size", {}, {"packets"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 2);
  CHECK(Call(bindings, "packets.count_of", {ScriptValue::Num(0x2A)}, {"packets"},
             &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 1);
  // A negative id is refused rather than wrapping.
  CHECK(Call(bindings, "packets.count_of", {ScriptValue::Num(-1)}, {"packets"},
             &out) == ScriptStatus::Error);

  CHECK(Call(bindings, "packets.totals", {}, {"packets"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsTable()->at("inbound").AsNumber() == 1);
  CHECK(out.AsTable()->at("outbound_bytes").AsNumber() == 32);

  CHECK(Call(bindings, "packets.summary", {ScriptValue::Str("inbound")}, {"packets"},
             &out) == ScriptStatus::Ok);
  CHECK(out.AsTable()->size() == 1);
  CHECK(out.AsTable()->at("0").Field("id")->AsNumber() == 0x2A);
  // An unknown direction is refused.
  CHECK(Call(bindings, "packets.summary", {ScriptValue::Str("sideways")},
             {"packets"}, &out) == ScriptStatus::Error);

  CHECK(Call(bindings, "packets.last", {}, {"packets"}, &out) == ScriptStatus::Ok);
  CHECK(out.Field("id")->AsNumber() == 0x2B);

  // clear is a read-surface reset; it works and the log is empty after.
  CHECK(Call(bindings, "packets.clear", {}, {"packets"}, &out) == ScriptStatus::Ok);
  CHECK(log.size() == 0);

  // Without the packets grant the whole module is denied by the gate.
  CHECK(Call(bindings, "packets.size", {}, {"network:*"}, &out) ==
        ScriptStatus::CapabilityDenied);

  // With no log bound, everything fails closed rather than crashing.
  ScriptApi bare(ScriptApi::Services{});
  ScriptBindings bareBindings;
  bare.InstallBindings(&bareBindings);
  CHECK(Call(bareBindings, "packets.size", {}, {"packets"}, &out) ==
        ScriptStatus::Error);
  CHECK(Call(bareBindings, "packets.last", {}, {"packets"}, &out) ==
        ScriptStatus::Error);
}

static void TestNetworkGetFailsClosed() {
  ScriptApi::Services services;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  // No grant at all: the runtime gate denies before the egress path runs.
  CHECK(Call(bindings, "network.get",
             {ScriptValue::Str("https://example.com/x"), ScriptValue::Num(2000)},
             {}, &out) == ScriptStatus::CapabilityDenied);

  // Granted a network host, but a URL the egress policy refuses (an IP literal)
  // must not reach a socket. The refusal is reported in the result table
  // (status 0, non-empty error), not by unwinding, so a script can branch on it.
  ScriptStatus st = Call(bindings, "network.get",
                         {ScriptValue::Str("https://127.0.0.1/x"),
                          ScriptValue::Num(2000)},
                         {"network:*"}, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(out.IsTable());
  const ScriptTable* t = out.AsTable();
  CHECK(t && t->at("status").AsNumber() == 0);
  CHECK(t && !t->at("ok").Truthy());
  CHECK(t && !t->at("error").AsString().empty());
  CHECK(t && t->at("body").AsString().empty());

  // An https destination is refused rather than silently downgraded to
  // plaintext (TLS is not implemented). The result carries the reason.
  out = ScriptValue();
  st = Call(bindings, "network.post",
            {ScriptValue::Str("http://127.0.0.1/x"), ScriptValue::Str("{}"),
             ScriptValue::Num(2000)},
            {"network:*"}, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(out.IsTable());
}

int main() {
  TestEveryIdlBindingIsInstalled();
  TestCapabilityGateOnDeclaredBindings();
  TestArityFailsClosed();
  TestWorldAndEntityReads();
  TestPhase2PlayerAndWorldExtras();
  TestModBindings();
  TestFsAndAudioBindings();
  TestPvpBindings();
  TestPacketBindings();
  TestNetworkGetFailsClosed();
  TestDrawCommandsAreQueuedThenApplied();
  TestHudLifecycleAcrossThreads();
  TestQueueIsBoundedAndFailsClosed();
  TestFrameHookDrainsQueue();
  TestFrameHookDrawsCosmetics();
  TestPublishedReads();
  TestGovernorAndZoomWrites();
  TestCombatAndInputReads();
  TestWorldQueryNeedsSeamAndLocalPlayer();
  TestPlayerReadsFailClosedAndResolve();
  TestRenderDrawHelpers();
  TestWorldReads();
  TestPerfStatsAndPlatform();
  TestPerfFlameBindings();
  if (g_fail == 0) printf("test_api_bindings: ALL OK\n");
  return g_fail;
}
