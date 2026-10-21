// Host tests for the IDL -> service bindings (review fix 4).
//
// The gap these close: `api/berry.idl` declared 20 functions, but nothing
// registered them, so the documented surface was not callable. These tests pin
// that:
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
  TestPublishedReads();
  TestGovernorAndZoomWrites();
  TestCombatAndInputReads();
  TestWorldQueryNeedsSeamAndLocalPlayer();
  if (g_fail == 0) printf("test_api_bindings: ALL OK\n");
  return g_fail;
}
