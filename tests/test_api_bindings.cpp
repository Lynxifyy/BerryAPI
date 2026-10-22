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
  // must still fail the call rather than reach a socket.
  ScriptStatus st = Call(bindings, "network.get",
                         {ScriptValue::Str("https://127.0.0.1/x"),
                          ScriptValue::Num(2000)},
                         {"network:*"}, &out);
  CHECK(st == ScriptStatus::Error);
}

int main() {
  TestEveryIdlBindingIsInstalled();
  TestCapabilityGateOnDeclaredBindings();
  TestArityFailsClosed();
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
  if (g_fail == 0) printf("test_api_bindings: ALL OK\n");
  return g_fail;
}
