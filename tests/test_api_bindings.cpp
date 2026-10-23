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
#include "berry/session_recorder.h"
#include "shim_level.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static bool Near(f64 a, f64 b, f64 eps = 1e-3) { return std::fabs(a - b) <= eps; }

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

static void TestFrameHookAdvancesAnimator() {
  Renderer renderer;
  Animator animator;
  Test::ShimSkeleton base;
  AnimatedSkeletonSource seam(&base, &animator);

  ScriptApi::Services services;
  services.renderer = &renderer;
  services.animator = &animator;
  services.animatorSeam = &seam;
  ScriptApi api(services);

  CHECK(animator.AddClip("clip", 10.0f, true));
  CHECK(animator.AddKey("clip", Bone::Head, 0.0f, Vec3::Zero(), Vec3::Zero(), 1.0f));

  Engine& eng = Engine::Instance();
  eng.Bootstrap();
  CHECK(eng.Attach());

  // A frame with nothing playing does not advance.
  eng.OnFrame();
  CHECK(animator.time() == 0.0f);

  // While playing, the frame clock advances the clip: the first frame sets the
  // baseline, the next measures a real delta.
  CHECK(animator.Play("clip"));
  eng.OnFrame();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  eng.OnFrame();
  CHECK(animator.time() > 0.0f);
  CHECK(animator.time() < 0.1f);

  // A long stall (a breakpoint, a paused client) is clamped to one step, so it
  // cannot fast-forward the whole clip in a single frame.
  const f32 before = animator.time();
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  eng.OnFrame();
  const f32 step = animator.time() - before;
  CHECK(step > 0.0f);
  CHECK(step <= 0.1f + 1e-4f);

  eng.Detach();
}

// The replay camera bindings: authoring through the real capability gate, and
// the frame hook actually adopting the pose (the real caller, not just the
// service). Nothing here writes world state; it only moves the camera.
static void TestReplayCameraBindings() {
  Renderer renderer;
  Berry::Test::CaptureRenderSeam seam;
  renderer.BindSeam(&seam);
  ReplayCamera cam;

  ScriptApi::Services services;
  services.renderer = &renderer;
  services.replayCamera = &cam;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  const std::vector<std::string> grants{"world"};

  // Every declared replay binding is callable with the world capability.
  CHECK(bindings.Has("replay.add_key"));
  CHECK(bindings.Has("replay.activate"));
  CHECK(bindings.Has("replay.export_begin"));

  // Authoring through the binding lands in the service.
  ScriptValue out;
  ScriptStatus st = Call(bindings, "replay.add_key",
                         {ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Num(64),
                          ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Num(0),
                          ScriptValue::Num(0), ScriptValue::Num(70)},
                         grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(out.Truthy());
  st = Call(bindings, "replay.add_key",
            {ScriptValue::Num(40), ScriptValue::Num(10), ScriptValue::Num(64),
             ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Num(90),
             ScriptValue::Num(0), ScriptValue::Num(70)},
            grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(cam.keyCount() == 2);

  // A duplicate tick is refused by the service, reported as false not an error.
  st = Call(bindings, "replay.add_key",
            {ScriptValue::Num(0), ScriptValue::Num(1), ScriptValue::Num(1),
             ScriptValue::Num(1), ScriptValue::Num(0), ScriptValue::Num(0),
             ScriptValue::Num(0), ScriptValue::Num(70)},
            grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(!out.Truthy());

  // A negative tick cannot name a frame.
  st = Call(bindings, "replay.add_key",
            {ScriptValue::Num(-1), ScriptValue::Num(0), ScriptValue::Num(0),
             ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Num(0),
             ScriptValue::Num(0), ScriptValue::Num(70)},
            grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(!out.Truthy());

  // The reads answer from the service.
  st = Call(bindings, "replay.key_count", {}, grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 2);
  st = Call(bindings, "replay.first_tick", {}, grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 0);
  st = Call(bindings, "replay.last_tick", {}, grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 40);
  st = Call(bindings, "replay.has_key", {ScriptValue::Num(40)}, grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(out.Truthy());

  // sample returns a pose table with the fields the docs promise.
  st = Call(bindings, "replay.sample", {ScriptValue::Num(20)}, grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(out.IsTable());
  const ScriptTable* t = out.AsTable();
  CHECK(t && t->at("valid").Truthy());
  CHECK(t && Near(t->at("x").AsNumber(), 5.0));
  CHECK(t && Near(t->at("yaw").AsNumber(), 45.0));

  // Smoothing is a name, and a bad one is refused.
  st = Call(bindings, "replay.set_smoothing", {ScriptValue::Str("smooth")}, grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(out.Truthy());
  st = Call(bindings, "replay.set_smoothing", {ScriptValue::Str("cubic")}, grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(!out.Truthy());

  // Activating with keys succeeds and turns playback on.
  st = Call(bindings, "replay.activate", {}, grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(cam.active());
  CHECK(cam.playing());

  // The frame hook now adopts the replay camera: a frame with a published size
  // sets the renderer's camera from the timeline. Before activation the camera
  // was invalid; after, it is valid and points where the first key says.
  api.PublishFrame(16.0f, Vec2(1280.0f, 720.0f));
  Engine& eng = Engine::Instance();
  eng.Bootstrap();
  CHECK(eng.Attach());
  eng.OnFrame();
  const CameraState frameCam = renderer.camera();
  CHECK(frameCam.valid);
  CHECK(Near(frameCam.width, 1280.0f));
  CHECK(Near(frameCam.height, 720.0f));
  eng.Detach();

  // Deactivating hands the camera back (the hook stops writing it).
  st = Call(bindings, "replay.deactivate", {}, grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(!cam.active());
  CHECK(!cam.playing());

  // With no service bound every binding fails closed rather than crashing.
  ScriptApi none((ScriptApi::Services{}));
  ScriptBindings nb;
  none.InstallBindings(&nb);
  st = Call(nb, "replay.key_count", {}, grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 0);
  st = Call(nb, "replay.add_key",
            {ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Num(0),
             ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Num(0),
             ScriptValue::Num(0), ScriptValue::Num(70)},
            grants, &out);
  CHECK(st == ScriptStatus::Error);   // no camera: raises, like every other module
  st = Call(nb, "replay.activate", {}, grants, &out);
  CHECK(st == ScriptStatus::Error);   // raises rather than hijacking the camera
}

// An export through the frame hook: each frame yields one video frame at the
// fixed rate, so the number of frames is the timeline's, not the wall clock's.
static void TestReplayCameraExportThroughFrameHook() {
  Renderer renderer;
  Berry::Test::CaptureRenderSeam seam;
  renderer.BindSeam(&seam);
  ReplayCamera cam;
  // 40 ticks is a 2-second timeline at 20 ticks/s.
  CHECK(cam.AddKey(CameraKey{0, Vec3(0, 64, 0), 0, 0, 0, 70}));
  CHECK(cam.AddKey(CameraKey{40, Vec3(0, 64, 40), 0, 0, 0, 70}));

  ScriptApi::Services services;
  services.renderer = &renderer;
  services.replayCamera = &cam;
  ScriptApi api(services);

  api.PublishFrame(16.0f, Vec2(1920.0f, 1080.0f));
  CHECK(cam.ExportBegin(20.0f, 1920.0f / 1080.0f, 1920.0f, 1080.0f));
  CHECK(cam.exporting());

  Engine& eng = Engine::Instance();
  eng.Bootstrap();
  CHECK(eng.Attach());
  // 40 ticks at 20 fps export yields 41 video frames (ticks 0..40); the next
  // frame finds the end, ends the export and yields nothing, so the loop body
  // runs 42 times and 41 of those set a camera.
  u64 frames = 0;
  while (cam.exporting()) {
    eng.OnFrame();
    ++frames;
    CHECK(frames < 1000);   // guard: the loop must terminate
  }
  CHECK(frames == 42);
  CHECK(cam.exportFrame() == 41);
  CHECK(renderer.camera().valid);
  eng.Detach();
}

// Follow-tape mode: with both a camera and a recorder bound, the frame hook
// seeks the camera's playhead to the replay tape's tick, so a key authored at
// tick N shows at tick N of the recording. This is the join that makes the
// camera a path over the tape rather than an independent clock.
static void TestReplayCameraFollowsTape() {
  Renderer renderer;
  Berry::Test::CaptureRenderSeam seam;
  renderer.BindSeam(&seam);
  ReplayCamera cam;
  // Two keys far apart, so which tick the tape is on is unambiguous in the
  // sampled x. Key 0 at x=0, key 100 at x=100.
  CHECK(cam.AddKey(CameraKey{0, Vec3(0, 64, 0), 0, 0, 0, 70}));
  CHECK(cam.AddKey(CameraKey{100, Vec3(100, 64, 0), 0, 0, 0, 70}));

  SessionRecorder rec;
  Berry::Test::ShimLevel level;
  CHECK(rec.Start());
  // A three-frame tape at ticks 0, 50, 100.
  for (u64 tick : {0ull, 50ull, 100ull}) rec.CaptureTick(tick, level);
  CHECK(rec.Stop());
  CHECK(rec.frames() == 3);

  ScriptApi::Services services;
  services.renderer = &renderer;
  services.replayCamera = &cam;
  services.recorder = &rec;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);
  const std::vector<std::string> grants{"world"};

  // Turn on follow-tape through the binding.
  ScriptValue out;
  ScriptStatus st = Call(bindings, "replay.set_follow_tape", {ScriptValue::Bool(true)},
                         grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(cam.followTape());
  st = Call(bindings, "replay.activate", {}, grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(out.Truthy());

  api.PublishFrame(16.0f, Vec2(1280.0f, 720.0f));
  Engine& eng = Engine::Instance();
  eng.Bootstrap();
  CHECK(eng.Attach());

  // Frame the tape at each frame; the camera's playhead must land on the tape's
  // tick, and the sampled position must be the timeline sampled at that tick
  // (x=0, 50, 100 for keys at 0 and 100), which is what "a path over the tape"
  // means.
  CHECK(rec.ReplayStep());   // tick 0
  eng.OnFrame();
  CHECK(Near(cam.playhead(), 0.0));
  st = Call(bindings, "replay.sample_now", {}, grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(out.IsTable() && Near(out.AsTable()->at("x").AsNumber(), 0.0));

  CHECK(rec.ReplayStep());   // tick 50
  eng.OnFrame();
  CHECK(Near(cam.playhead(), 50.0));
  st = Call(bindings, "replay.sample_now", {}, grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(out.IsTable() && Near(out.AsTable()->at("x").AsNumber(), 50.0));

  CHECK(rec.ReplayStep());   // tick 100
  eng.OnFrame();
  CHECK(Near(cam.playhead(), 100.0));
  st = Call(bindings, "replay.sample_now", {}, grants, &out);
  CHECK(st == ScriptStatus::Ok);
  CHECK(out.IsTable() && Near(out.AsTable()->at("x").AsNumber(), 100.0));

  // With the tape at its end, ReplayTick holds the last frame's tick, so the
  // camera holds the last key rather than jumping to 0.
  eng.OnFrame();
  CHECK(Near(cam.playhead(), 100.0));

  eng.Detach();
}

static void TestFrameHookPresentsUi() {
  Renderer renderer;
  Berry::Test::CaptureRenderSeam seam;
  renderer.BindSeam(&seam);
  CameraState cam;
  cam.width = 1280;
  cam.height = 720;
  cam.valid = true;
  renderer.SetCamera(cam);

  UiLayer ui;
  InputObserver input;
  ScriptApi::Services services;
  services.renderer = &renderer;
  services.ui = &ui;
  services.input = &input;
  ScriptApi api(services);

  // The worker builds a window with one button.
  ui.BeginFrame();
  ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 150));
  ui.Button("Go");
  ui.EndWindow();
  ui.EndFrame();

  renderer.BeginFrame(Vec2(1280, 720));
  Engine& eng = Engine::Instance();
  eng.Bootstrap();
  CHECK(eng.Attach());
  // The embedder publishes the frame size each frame; the overlay hooks skip a
  // zero-sized frame, so this must be set before OnFrame.
  api.PublishFrame(16.0f, Vec2(1280, 720));
  eng.OnFrame();  // emits berry:frame; the hook presents the UI
  renderer.EndFrame();
  eng.Detach();

  // The window background, the button and the caption all reached the seam.
  CHECK(seam.rects >= 2);
  CHECK(seam.texts >= 1);
  // Presenting published the button's result, which the next build can read.
  const UiStats st = ui.stats();
  CHECK(st.widgets >= 3);
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

static void TestRenderOcclusionBindings() {
  Berry::Test::CaptureRenderSeam seam;
  Renderer renderer;
  renderer.Attach(&seam);

  ScriptApi::Services services;
  services.renderer = &renderer;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  CameraState cam;
  cam.width = 800;
  cam.height = 600;
  cam.view = Mat4::LookAt(Vec3(0, 0, 0), Vec3(0, 0, 1), Vec3(0, 1, 0));
  cam.projection = Mat4::Perspective(70, 800.0f / 600.0f, 0.1f, 200.0f);
  cam.valid = true;
  renderer.SetCamera(cam);
  renderer.BeginFrame(Vec2(800, 600));

  ScriptValue out;
  auto box = [](f32 mnx, f32 mny, f32 mnz, f32 mxx, f32 mxy, f32 mxz) {
    return std::vector<ScriptValue>{ScriptValue::Num(mnx), ScriptValue::Num(mny),
                                    ScriptValue::Num(mnz), ScriptValue::Num(mxx),
                                    ScriptValue::Num(mxy), ScriptValue::Num(mxz)};
  };

  // No occluder yet: nothing is occluded, and cull_box keeps the box (in view).
  CHECK(Call(bindings, "render.occluded", box(-1, -1, 19, 1, 1, 21),
             {"render"}, &out) == ScriptStatus::Ok);
  CHECK(!out.Truthy());
  CHECK(Call(bindings, "render.cull_box", box(-1, -1, 19, 1, 1, 21),
             {"render"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());   // frustum keeps it, no occluder hides it

  // Submit a wall, then the box behind it is occluded.
  CHECK(Call(bindings, "render.add_occluder", box(-3, -3, 9, 3, 3, 11),
             {"render"}, &out) == ScriptStatus::Ok);
  CHECK(Call(bindings, "render.occluded", box(-1, -1, 19, 1, 1, 21),
             {"render"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  // cull_box now drops it.
  CHECK(Call(bindings, "render.cull_box", box(-1, -1, 19, 1, 1, 21),
             {"render"}, &out) == ScriptStatus::Ok);
  CHECK(!out.Truthy());
  // A box in front of the wall is kept.
  CHECK(Call(bindings, "render.cull_box", box(-1, -1, 3, 1, 1, 5),
             {"render"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());

  // A malformed box (min > max) is refused rather than culling on garbage; the
  // cull path fails open.
  CHECK(Call(bindings, "render.occluded", box(1, 0, 0, -1, 0, 0),
             {"render"}, &out) == ScriptStatus::Error);
  CHECK(Call(bindings, "render.cull_box", box(1, 0, 0, -1, 0, 0),
             {"render"}, &out) == ScriptStatus::Error);

  // A fresh frame clears the grid: the box is no longer occluded.
  renderer.EndFrame();
  renderer.BeginFrame(Vec2(800, 600));
  CHECK(Call(bindings, "render.occluded", box(-1, -1, 19, 1, 1, 21),
             {"render"}, &out) == ScriptStatus::Ok);
  CHECK(!out.Truthy());
  renderer.EndFrame();

  // With no renderer bound the surface fails closed.
  ScriptApi bare(ScriptApi::Services{});
  ScriptBindings bareBindings;
  bare.InstallBindings(&bareBindings);
  CHECK(Call(bareBindings, "render.occluded", box(0, 0, 0, 1, 1, 1),
             {"render"}, &out) == ScriptStatus::Error);
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

static void TestPerfPipelineBindings() {
  Renderer renderer;
  ScriptApi::Services services;
  services.renderer = &renderer;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  CameraState cam;
  cam.width = 800;
  cam.height = 600;
  cam.view = Mat4::LookAt(Vec3(0, 0, -10), Vec3(0, 0, 0), Vec3(0, 1, 0));
  cam.projection = Mat4::Perspective(70, 800.0f / 600.0f, 0.1f, 200.0f);
  cam.valid = true;
  renderer.SetCamera(cam);

  // Drive a real frame so the pipeline records a batch. A fill-only rect is
  // two triangles: one draw call, six vertices.
  RenderSettings settings;
  settings.cullFarEntities = true;
  renderer.SetRenderSettings(settings);
  renderer.BeginFrame(Vec2(800, 600));
  Rect2D fill;
  fill.min = Vec2(0, 0);
  fill.max = Vec2(10, 10);
  fill.fill = Rgba::White();
  fill.filled = true;
  fill.thickness = 0.0f;   // no outline: just the two triangles
  renderer.DrawRect(fill);
  renderer.EndFrame();

  ScriptValue out;
  CHECK(Call(bindings, "perf.pipeline", {}, {"perf"}, &out) == ScriptStatus::Ok);
  CHECK(out.IsTable());
  CHECK(out.Field("frames")->AsNumber() == 1);
  CHECK(out.Field("draw_calls")->AsNumber() == 1);
  CHECK(out.Field("vertices")->AsNumber() == 6);
  CHECK(out.Field("batches")->AsNumber() == 1);
  // Dynamic resolution starts at native scale.
  CHECK(out.Field("resolution_scale")->AsNumber() == 1.0);

  // A rect with the default outline adds a line draw call and four line
  // segments, so the frame costs two calls and fourteen vertices.
  renderer.BeginFrame(Vec2(800, 600));
  Rect2D outlined = fill;
  outlined.thickness = 1.0f;
  renderer.DrawRect(outlined);
  renderer.EndFrame();
  CHECK(Call(bindings, "perf.pipeline", {}, {"perf"}, &out) == ScriptStatus::Ok);
  CHECK(out.Field("frames")->AsNumber() == 2);
  CHECK(out.Field("draw_calls")->AsNumber() == 1 + 2);   // fill + outline
  CHECK(out.Field("vertices")->AsNumber() == 6 + 14);

  // The LOD tier follows the policy the renderer culls with. With culling on,
  // full detail sits inside 16 blocks and past 64 the entity is culled.
  CHECK(Call(bindings, "perf.lod_tier", {ScriptValue::Num(1)},
             {"perf"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsString() == "full");
  CHECK(Call(bindings, "perf.lod_tier", {ScriptValue::Num(1000)},
             {"perf"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsString() == "culled");

  // With culling off, the far tier degrades to static instead of vanishing.
  // The policy is adopted at BeginFrame, so drive one.
  settings.cullFarEntities = false;
  renderer.SetRenderSettings(settings);
  renderer.BeginFrame(Vec2(800, 600));
  renderer.EndFrame();
  CHECK(Call(bindings, "perf.lod_tier", {ScriptValue::Num(1000)},
             {"perf"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsString() == "static");

  // A negative distance is refused rather than mapped to a tier.
  CHECK(Call(bindings, "perf.lod_tier", {ScriptValue::Num(-1)},
             {"perf"}, &out) == ScriptStatus::Error);

  // The tick-rate half of LOD: a near entity ticks every tick, a distant one is
  // throttled, and the phase spreads a crowd so they do not hitch together.
  CHECK(Call(bindings, "perf.lod_should_tick",
             {ScriptValue::Num(1), ScriptValue::Num(1), ScriptValue::Num(0)},
             {"perf"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());   // full: every tick
  CHECK(Call(bindings, "perf.lod_should_tick",
             {ScriptValue::Num(1000), ScriptValue::Num(1), ScriptValue::Num(0)},
             {"perf"}, &out) == ScriptStatus::Ok);
  CHECK(!out.Truthy());  // culled: never
  // Two phases at the same reduced distance must differ on the same tick.
  CHECK(Call(bindings, "perf.lod_should_tick",
             {ScriptValue::Num(24), ScriptValue::Num(0), ScriptValue::Num(0)},
             {"perf"}, &out) == ScriptStatus::Ok);
  const bool p0 = out.Truthy();
  CHECK(Call(bindings, "perf.lod_should_tick",
             {ScriptValue::Num(24), ScriptValue::Num(0), ScriptValue::Num(1)},
             {"perf"}, &out) == ScriptStatus::Ok);
  CHECK(p0 != out.Truthy());
  // A bad argument is refused rather than silently treated as 0.
  CHECK(Call(bindings, "perf.lod_should_tick",
             {ScriptValue::Num(-1), ScriptValue::Num(0), ScriptValue::Num(0)},
             {"perf"}, &out) == ScriptStatus::Error);

  // The perf grant gates the surface.
  CHECK(Call(bindings, "perf.pipeline", {}, {"hud"}, &out) ==
        ScriptStatus::CapabilityDenied);

  // With no renderer bound, the surface fails closed rather than crashing.
  ScriptApi bare(ScriptApi::Services{});
  ScriptBindings bareBindings;
  bare.InstallBindings(&bareBindings);
  CHECK(Call(bareBindings, "perf.pipeline", {}, {"perf"}, &out) ==
        ScriptStatus::Error);
  CHECK(Call(bareBindings, "perf.lod_tier", {ScriptValue::Num(1)},
             {"perf"}, &out) == ScriptStatus::Error);
}

/// The frame publisher is what keeps the pipeline following the governor: each
/// published frame adopts the governor's settings and feeds it the frame time.
static void TestPublishFrameDrivesThePipeline() {
  Renderer renderer;
  PerformanceGovernor governor;
  ScriptApi::Services services;
  services.renderer = &renderer;
  services.governor = &governor;
  ScriptApi api(services);

  // Pin the governor to a known platform so the assertion does not depend on
  // its default ramp.
  governor.SetPlatform(PlatformClass::MobileLow);
  const RenderSettings expected = governor.settings();

  api.PublishFrame(16.0f, Vec2(800, 600));
  CHECK(renderer.renderSettings() == expected);

  // The frame time reaches the resolution controller, which stays at native
  // scale while every frame meets the target.
  for (int i = 0; i < 20; ++i) api.PublishFrame(16.0f, Vec2(800, 600));
  CHECK(renderer.pipelineStats().resolutionScale == 1.0f);

  // A sustained slow streak makes it step down, so the controller is really
  // being fed rather than just holding its default.
  for (int i = 0; i < 40; ++i) api.PublishFrame(40.0f, Vec2(800, 600));
  CHECK(renderer.pipelineStats().resolutionScale < 1.0f);
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

//----------------------------------------------------------------------------
// CombatState bindings (Phase 3)
//----------------------------------------------------------------------------

static void TestCombatStateBindings() {
  Renderer renderer;
  std::shared_ptr<BerryModule> worldMod = CreateWorldModule();
  WorldHandle* world = worldMod->Service<WorldHandle>();
  auto shim = std::make_shared<Berry::Test::ShimLevel>();
  CombatStateTracker cs;

  const EntityId self{1, 1};
  EntitySnapshot me;
  me.id = self;
  me.type = EntityType::Player;
  me.position = Vec3(0, 64, 0);
  me.alive = true;
  shim->AddEntity(self, me);

  const EntityId victim{2, 2};
  EntitySnapshot v;
  v.id = victim;
  v.type = EntityType::Player;
  v.position = Vec3(2, 64, 0);   // 2 blocks away: within the default reach
  v.alive = true;
  shim->AddEntity(victim, v);

  world->SetSeam(shim);

  ScriptApi::Services services;
  services.world = world;
  services.renderer = &renderer;
  services.localPlayer = self;
  services.combatState = &cs;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;

  //--- reach configuration and reads ---
  CHECK(Call(bindings, "combat_state.max_reach", {}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(std::fabs(out.AsNumber() - 3.0) < 1e-6);
  CHECK(Call(bindings, "combat_state.set_max_reach", {ScriptValue::Num(4.5)}, {"combat"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(std::fabs(cs.maxReach() - 4.5f) < 1e-5f);
  // A non-positive reach is refused and leaves the limit unchanged.
  CHECK(Call(bindings, "combat_state.set_max_reach", {ScriptValue::Num(0)}, {"combat"}, &out) ==
        ScriptStatus::Error);
  CHECK(std::fabs(cs.maxReach() - 4.5f) < 1e-5f);

  // last_reach before any hit is invalid, not a fabricated zero-distance hit.
  CHECK(Call(bindings, "combat_state.last_reach", {}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(!out.AsTable()->at("valid").Truthy());
  CHECK(Call(bindings, "combat_state.reach_violations", {}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 0);

  //--- hitbox history from the seam feed ---
  // The berry:tick hook records hitboxes; emulate the tick directly.
  cs.ObserveEntity(me, 100);
  cs.ObserveEntity(v, 100);
  CHECK(Call(bindings, "combat_state.hitbox_count", {ScriptValue::Num(2)}, {"combat"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 1);

  CHECK(Call(bindings, "combat_state.hitbox_ticks", {ScriptValue::Num(2)}, {"combat"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsTable()->at("0").AsNumber() == 100);

  CHECK(Call(bindings, "combat_state.hitbox_at",
             {ScriptValue::Num(2), ScriptValue::Num(100)}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsTable()->at("valid").Truthy());
  CHECK(std::fabs(out.AsTable()->at("min_x").AsNumber() - 1.7) < 1e-4);

  // An exact tick with no sample reports valid=false.
  CHECK(Call(bindings, "combat_state.hitbox_at",
             {ScriptValue::Num(2), ScriptValue::Num(999)}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(!out.AsTable()->at("valid").Truthy());

  //--- reach validation through the combat event ---
  CombatEvent hit;
  hit.attacker = self;
  hit.victim = victim;
  hit.finalDamage = 5.0f;
  hit.tick = 100;
  cs.ObserveHit(hit, 100);
  CHECK(Call(bindings, "combat_state.last_reach", {}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsTable()->at("valid").Truthy());
  CHECK(out.AsTable()->at("within_reach").Truthy());
  CHECK(out.AsTable()->at("victim").AsNumber() == 2);

  //--- swing timing ---
  CHECK(Call(bindings, "combat_state.swing",
             {ScriptValue::Num(1), ScriptValue::Num(100)}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(!out.AsTable()->at("valid").Truthy());   // first swing
  CHECK(Call(bindings, "combat_state.swing",
             {ScriptValue::Num(1), ScriptValue::Num(104)}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsTable()->at("valid").Truthy());
  CHECK(std::fabs(out.AsTable()->at("cps").AsNumber() - 5.0) < 1e-6);
  CHECK(Call(bindings, "combat_state.last_swing", {}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsTable()->at("tick").AsNumber() == 104);
  CHECK(Call(bindings, "combat_state.average_swing_cps", {ScriptValue::Num(1)}, {"combat"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() > 0);

  //--- projectile prediction ---
  CHECK(Call(bindings, "combat_state.predict_projectile",
             {ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Num(0),
              ScriptValue::Num(20), ScriptValue::Num(0), ScriptValue::Num(0),
              ScriptValue::Num(0), ScriptValue::Num(10)}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(std::fabs(out.AsTable()->at("x").AsNumber() - 10.0) < 1e-4);

  CHECK(Call(bindings, "combat_state.note_projectile",
             {ScriptValue::Num(7), ScriptValue::Num(10), ScriptValue::Num(0),
              ScriptValue::Num(0), ScriptValue::Num(10)}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(Call(bindings, "combat_state.resolve_projectile",
             {ScriptValue::Num(7), ScriptValue::Num(11), ScriptValue::Num(0),
              ScriptValue::Num(0), ScriptValue::Num(15)}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(std::fabs(out.AsNumber() - 1.0) < 1e-5);
  CHECK(Call(bindings, "combat_state.prediction_count", {}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 1);
  CHECK(Call(bindings, "combat_state.prediction_error", {}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(std::fabs(out.AsNumber() - 1.0) < 1e-5);

  //--- hit-prediction accuracy ---
  cs.PredictHit(self, victim, 200);
  cs.ConfirmHit(hit, 200);
  CHECK(Call(bindings, "combat_state.predicted_hits", {}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 1);
  CHECK(Call(bindings, "combat_state.confirmed_hits", {}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 1);
  CHECK(Call(bindings, "combat_state.missed_hits", {}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 0);
  CHECK(Call(bindings, "combat_state.hit_accuracy", {}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(std::fabs(out.AsNumber() - 1.0) < 1e-9);

  //--- click budget ---
  CHECK(Call(bindings, "combat_state.set_click_budget",
             {ScriptValue::Num(20), ScriptValue::Num(20)}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(Call(bindings, "combat_state.set_click_budget",
             {ScriptValue::Num(0), ScriptValue::Num(20)}, {"combat"}, &out) == ScriptStatus::Error);

  CHECK(Call(bindings, "combat_state.note_click",
             {ScriptValue::Str("left"), ScriptValue::Num(1000)}, {"combat"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(Call(bindings, "combat_state.click_rate", {ScriptValue::Str("left")}, {"combat"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() >= 1.0);
  CHECK(Call(bindings, "combat_state.click_limited", {}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(!out.Truthy());
  CHECK(Call(bindings, "combat_state.click_tokens", {ScriptValue::Str("left")}, {"combat"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() >= 19.0);
  // An unknown button is refused.
  CHECK(Call(bindings, "combat_state.note_click",
             {ScriptValue::Str("middle"), ScriptValue::Num(1001)}, {"combat"}, &out) ==
        ScriptStatus::Error);

  //--- clear ---
  CHECK(Call(bindings, "combat_state.clear", {}, {"combat"}, &out) == ScriptStatus::Ok);
  CHECK(cs.TrackedEntities() == 0);
  CHECK(cs.PredictedHits() == 0);

  //--- a bad argument fails closed ---
  CHECK(Call(bindings, "combat_state.hitbox_count", {ScriptValue::Num(-1)}, {"combat"}, &out) ==
        ScriptStatus::Error);
}

//----------------------------------------------------------------------------
// CombatState fails closed without the service
//----------------------------------------------------------------------------

static void TestCombatStateFailsClosedWithoutService() {
  ScriptApi::Services services;   // no combatState, no world
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  // Every read returns a neutral value and raises rather than fabricating
  // combat state.
  CHECK(Call(bindings, "combat_state.max_reach", {}, {"combat"}, &out) == ScriptStatus::Error);
  CHECK(Call(bindings, "combat_state.hit_accuracy", {}, {"combat"}, &out) == ScriptStatus::Error);
  CHECK(Call(bindings, "combat_state.last_reach", {}, {"combat"}, &out) == ScriptStatus::Error);
  CHECK(Call(bindings, "combat_state.reach_violations", {}, {"combat"}, &out) == ScriptStatus::Error);
  CHECK(Call(bindings, "combat_state.prediction_error", {}, {"combat"}, &out) == ScriptStatus::Error);
  CHECK(Call(bindings, "combat_state.note_click",
             {ScriptValue::Str("left"), ScriptValue::Num(0)}, {"combat"}, &out) == ScriptStatus::Error);
  // clear is a no-op that still reports success-shaped nil.
  CHECK(Call(bindings, "combat_state.clear", {}, {"combat"}, &out) == ScriptStatus::Error);

  // Without the grant, the capability gate denies before the body runs.
  CHECK(Call(bindings, "combat_state.max_reach", {}, {"world"}, &out) == ScriptStatus::CapabilityDenied);
}



static void TestUiBindings() {
  UiLayer ui;
  Renderer renderer;
  InputObserver input;
  ScriptApi::Services services;
  services.ui = &ui;
  services.renderer = &renderer;
  services.input = &input;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  const std::vector<std::string> grant = {"hud"};

  // A script's UI calls run inside an explicit frame: begin_frame ... end_frame.
  CHECK(Call(bindings, "ui.begin_frame", {}, grant, &out) == ScriptStatus::Ok);

  // begin_window opens and returns true for a fresh (expanded) window.
  CHECK(Call(bindings, "ui.begin_window",
             {ScriptValue::Str("W"), ScriptValue::Num(0), ScriptValue::Num(0),
              ScriptValue::Num(200), ScriptValue::Num(150)},
             grant, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());

  // A widget returns a table with every field present.
  CHECK(Call(bindings, "ui.button", {ScriptValue::Str("Go")}, grant, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsTable());
  CHECK(out.AsTable()->count("hovered") == 1);
  CHECK(out.AsTable()->count("clicked") == 1);
  CHECK(out.AsTable()->count("value") == 1);
  CHECK(out.AsTable()->count("text") == 1);

  CHECK(Call(bindings, "ui.end_window", {}, grant, &out) == ScriptStatus::Ok);
  CHECK(Call(bindings, "ui.end_frame", {}, grant, &out) == ScriptStatus::Ok);

  // The frame the script built is now ready to present.
  const auto widgets = ui.Widgets();
  CHECK(!widgets.empty());

  // The present half publishes results, and the next build reads them: simulate
  // a click on the button by driving the layer's own hit-test.
  UiWidget button;
  bool found = false;
  for (const auto& w : widgets) {
    if (w.kind == UiKind::Button) { button = w; found = true; }
  }
  CHECK(found);
  const Vec2 centre((button.min.x + button.max.x) * 0.5f,
                    (button.min.y + button.max.y) * 0.5f);
  UiLayer::Input down;
  down.pointer = centre;
  down.mouseDown = true;
  ui.Update(down);
  UiLayer::Input up;
  up.pointer = centre;
  ui.Update(up);

  // Rebuild the same frame; the button now reports clicked.
  ui.BeginFrame();
  CHECK(ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 150)));
  const UiResult r = ui.Button("Go");
  ui.EndWindow();
  ui.EndFrame();
  CHECK(r.clicked);

  // set_theme accepts the two known names and refuses anything else.
  CHECK(Call(bindings, "ui.set_theme", {ScriptValue::Str("glass")}, grant, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(ui.theme().rounding == UiTheme::Glass().rounding);
  CHECK(Call(bindings, "ui.set_theme", {ScriptValue::Str("nope")}, grant, &out) ==
        ScriptStatus::Ok);
  CHECK(!out.Truthy());  // refused, theme unchanged
  CHECK(ui.theme().rounding == UiTheme::Glass().rounding);
  CHECK(Call(bindings, "ui.theme", {}, grant, &out) == ScriptStatus::Ok);
  CHECK(out.ToDisplay() == "glass");
  CHECK(Call(bindings, "ui.set_theme", {ScriptValue::Str("dark")}, grant, &out) ==
        ScriptStatus::Ok);
  CHECK(Call(bindings, "ui.theme", {}, grant, &out) == ScriptStatus::Ok);
  CHECK(out.ToDisplay() == "dark");

  // stats and any_window_dragging answer.
  CHECK(Call(bindings, "ui.stats", {}, grant, &out) == ScriptStatus::Ok);
  CHECK(out.IsTable());
  CHECK(Call(bindings, "ui.any_window_dragging", {}, grant, &out) == ScriptStatus::Ok);
  CHECK(!out.Truthy());
  CHECK(Call(bindings, "ui.reset", {}, grant, &out) == ScriptStatus::Ok);

  // text_colored refuses an unknown role rather than silently drawing nothing.
  CHECK(Call(bindings, "ui.text_colored",
             {ScriptValue::Str("nonsense"), ScriptValue::Str("hi")}, grant, &out) ==
        ScriptStatus::Error);

  // Without the hud grant the gate denies before the body runs.
  CHECK(Call(bindings, "ui.button", {ScriptValue::Str("Go")}, {"render"}, &out) ==
        ScriptStatus::CapabilityDenied);
}

static void TestUiBindingsFailClosedWithoutService() {
  ScriptApi::Services services;   // no ui layer
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  const std::vector<std::string> grant = {"hud"};
  // begin_window reports false and nothing is built; every widget returns an
  // inert table; the no-op calls still succeed so a script can run unguarded.
  CHECK(Call(bindings, "ui.begin_window",
             {ScriptValue::Str("W"), ScriptValue::Num(0), ScriptValue::Num(0),
              ScriptValue::Num(100), ScriptValue::Num(100)},
             grant, &out) == ScriptStatus::Ok);
  CHECK(!out.Truthy());
  CHECK(Call(bindings, "ui.button", {ScriptValue::Str("Go")}, grant, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsTable());
  CHECK(!out.AsTable()->at("clicked").Truthy());
  CHECK(Call(bindings, "ui.end_window", {}, grant, &out) == ScriptStatus::Ok);
  CHECK(Call(bindings, "ui.text", {ScriptValue::Str("hi")}, grant, &out) ==
        ScriptStatus::Ok);
  CHECK(Call(bindings, "ui.set_theme", {ScriptValue::Str("dark")}, grant, &out) ==
        ScriptStatus::Ok);
  CHECK(!out.Truthy());  // no layer -> refused
  CHECK(Call(bindings, "ui.theme", {}, grant, &out) == ScriptStatus::Ok);
  CHECK(out.ToDisplay() == "none");
}

static void TestUiThroughLuauScript() {
  UiLayer ui;
  Renderer renderer;
  Berry::Test::CaptureRenderSeam seam;
  renderer.BindSeam(&seam);
  CameraState cam;
  cam.width = 1280;
  cam.height = 720;
  cam.valid = true;
  renderer.SetCamera(cam);

  ScriptApi::Services services;
  services.ui = &ui;
  services.renderer = &renderer;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  // A real Luau chunk drives the whole surface through the namespace tables.
  auto vm = std::unique_ptr<IScriptVm>(CreateLuauVm());
  ScriptHost host("ui-script", Grant({"hud"}), std::move(vm));
  host.SetBindings(&bindings);

  const char* src =
      "local clicks = 0\n"
      "local M = {}\n"
      "function M.draw()\n"
      "  ui.begin_frame()\n"
      "  local open = ui.begin_window('Panel', 10, 10, 220, 160)\n"
      "  if open then\n"
      "    ui.text('hello')\n"
      "    local r = ui.button('Go')\n"
      "    if r.clicked then clicks = clicks + 1 end\n"
      "    ui.separator()\n"
      "  end\n"
      "  ui.end_window()\n"
      "  ui.end_frame()\n"
      "  return clicks\n"
      "end\n"
      "return M\n";
  ScriptError err;
  CHECK(host.Load(src, "ui-script", &err));

  std::vector<ScriptValue> ret;
  CHECK(host.RunHook("draw", {}, &ret) == ScriptStatus::Ok);
  CHECK(ret.size() == 1 && ret[0].AsNumber() == 0);

  // The frame reached the layer.
  const auto widgets = ui.Widgets();
  CHECK(widgets.size() >= 4);  // window bg, text, button, separator

  // The binding layer bracketed the frame: it is closed after the hook.
  CHECK(!ui.building());

  // Present with a click on the button, then re-run the hook: the script sees
  // the click through `r.clicked` and bumps its counter.
  UiWidget button;
  bool found = false;
  for (const auto& w : widgets) {
    if (w.kind == UiKind::Button) { button = w; found = true; }
  }
  CHECK(found);
  const Vec2 centre((button.min.x + button.max.x) * 0.5f,
                    (button.min.y + button.max.y) * 0.5f);
  UiLayer::Input down;
  down.pointer = centre;
  down.mouseDown = true;
  ui.Update(down);
  UiLayer::Input up;
  up.pointer = centre;
  ui.Update(up);

  ret.clear();
  CHECK(host.RunHook("draw", {}, &ret) == ScriptStatus::Ok);
  CHECK(ret.size() == 1 && ret[0].AsNumber() == 1);
}

static void TestUiFrameRecoveryAfterFault() {
  UiLayer ui;
  ScriptApi::Services services;
  services.ui = &ui;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  const std::vector<std::string> grant = {"hud"};

  // A hook opens a frame, builds a window, then faults before end_frame: the
  // frame is left open.
  CHECK(Call(bindings, "ui.begin_frame", {}, grant, &out) == ScriptStatus::Ok);
  CHECK(Call(bindings, "ui.begin_window",
             {ScriptValue::Str("W"), ScriptValue::Num(0), ScriptValue::Num(0),
              ScriptValue::Num(100), ScriptValue::Num(100)},
             grant, &out) == ScriptStatus::Ok);
  CHECK(ui.building());

  // The next hook's begin_frame detects the stale frame, aborts it, and opens a
  // fresh one; the half-built window from the faulted hook never publishes.
  CHECK(Call(bindings, "ui.begin_frame", {}, grant, &out) == ScriptStatus::Ok);
  CHECK(ui.building());
  // Nothing was published, so the ready frame is still empty.
  CHECK(ui.Widgets().empty());

  // A window outside a frame is refused, so a script that forgets begin_frame
  // cannot corrupt the published frame.
  CHECK(Call(bindings, "ui.end_frame", {}, grant, &out) == ScriptStatus::Ok);
  CHECK(!ui.building());
  CHECK(Call(bindings, "ui.begin_window",
             {ScriptValue::Str("X"), ScriptValue::Num(0), ScriptValue::Num(0),
              ScriptValue::Num(100), ScriptValue::Num(100)},
             grant, &out) == ScriptStatus::Ok);
  CHECK(!out.Truthy());

  // end_frame with no open frame is a harmless no-op.
  CHECK(Call(bindings, "ui.end_frame", {}, grant, &out) == ScriptStatus::Ok);
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

static void TestAnimatorBindings() {
  Renderer renderer;
  Animator animator;
  Test::ShimSkeleton base;
  AnimatedSkeletonSource seam(&base, &animator);

  ScriptApi::Services services;
  services.renderer = &renderer;
  services.animator = &animator;
  services.animatorSeam = &seam;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  const std::vector<std::string> caps = {"cosmetic"};

  // Author a clip through the bindings.
  CHECK(Call(bindings, "animator.add_clip",
             {ScriptValue::Str("wave"), ScriptValue::Num(1.0), ScriptValue::Bool(true)},
             caps, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(Call(bindings, "animator.add_key",
             {ScriptValue::Str("wave"), ScriptValue::Str("right_arm"), ScriptValue::Num(0.0),
              ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Num(0),
              ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Num(0),
              ScriptValue::Num(1.0)},
             caps, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(Call(bindings, "animator.add_key",
             {ScriptValue::Str("wave"), ScriptValue::Str("right_arm"), ScriptValue::Num(1.0),
              ScriptValue::Num(0), ScriptValue::Num(2), ScriptValue::Num(0),
              ScriptValue::Num(0), ScriptValue::Num(0), ScriptValue::Num(0),
              ScriptValue::Num(1.0)},
             caps, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());

  CHECK(Call(bindings, "animator.clip_count", {}, caps, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 1);
  CHECK(Call(bindings, "animator.has_clip", {ScriptValue::Str("wave")}, caps, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(Call(bindings, "animator.key_count", {ScriptValue::Str("wave")}, caps, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 2);

  // Playback.
  CHECK(Call(bindings, "animator.play", {ScriptValue::Str("wave")}, caps, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(Call(bindings, "animator.playing", {}, caps, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(Call(bindings, "animator.current_clip", {}, caps, &out) == ScriptStatus::Ok);
  CHECK(out.AsString() == "wave");
  CHECK(Call(bindings, "animator.duration", {}, caps, &out) == ScriptStatus::Ok);
  CHECK(std::fabs(out.AsNumber() - 1.0) < 1e-6);

  CHECK(Call(bindings, "animator.seek", {ScriptValue::Num(0.5)}, caps, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());

  // sample returns a pose table with the interpolated offset.
  CHECK(Call(bindings, "animator.sample",
             {ScriptValue::Str("head"), ScriptValue::Num(0.5)}, caps, &out) == ScriptStatus::Ok);
  CHECK(out.IsTable());
  CHECK(out.Field("valid") && !out.Field("valid")->Truthy());   // head has no keys

  CHECK(Call(bindings, "animator.sample",
             {ScriptValue::Str("right_arm"), ScriptValue::Num(0.5)}, caps, &out) == ScriptStatus::Ok);
  CHECK(out.IsTable());
  CHECK(out.Field("valid") && out.Field("valid")->Truthy());
  CHECK(out.Field("py") && std::fabs(out.Field("py")->AsNumber() - 1.0) < 1e-4);

  // pose lists one entry per keyed bone.
  CHECK(Call(bindings, "animator.pose", {}, caps, &out) == ScriptStatus::Ok);
  CHECK(out.IsTable());
  CHECK(out.Field("0") != nullptr);

  // An unknown bone is an error, not a fabricated pose.
  CHECK(Call(bindings, "animator.sample",
             {ScriptValue::Str("elbow"), ScriptValue::Num(0.0)}, caps, &out) != ScriptStatus::Ok);
  // An unknown clip to play is an error.
  CHECK(Call(bindings, "animator.play", {ScriptValue::Str("nope")}, caps, &out) != ScriptStatus::Ok);

  // target is the seam's, defaulting to every entity.
  CHECK(Call(bindings, "animator.target", {}, caps, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 0);
  CHECK(Call(bindings, "animator.set_target", {ScriptValue::Num(42)}, caps, &out) == ScriptStatus::Ok);
  CHECK(seam.target().raw == 42);
  CHECK(Call(bindings, "animator.target", {}, caps, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 42);

  // save/load round trip through the bindings.
  CHECK(Call(bindings, "animator.save", {}, caps, &out) == ScriptStatus::Ok);
  const std::string doc = out.AsString();
  CHECK(!doc.empty());
  CHECK(Call(bindings, "animator.clear", {}, caps, &out) == ScriptStatus::Ok);
  CHECK(Call(bindings, "animator.clip_count", {}, caps, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 0);
  CHECK(Call(bindings, "animator.load", {ScriptValue::Str(doc)}, caps, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(Call(bindings, "animator.clip_count", {}, caps, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 1);
  // A malformed document is an error and leaves the clips alone.
  CHECK(Call(bindings, "animator.load", {ScriptValue::Str("{}")}, caps, &out) != ScriptStatus::Ok);
  // A document from a version this build does not write is refused too.
  CHECK(Call(bindings, "animator.load",
             {ScriptValue::Str(R"({"format":"berry-animator","version":2,"clips":[]})")},
             caps, &out) != ScriptStatus::Ok);
  CHECK(Call(bindings, "animator.clip_count", {}, caps, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 1);
}

static void TestAnimatorFailsClosedWithoutService() {
  Renderer renderer;
  ScriptApi::Services services;
  services.renderer = &renderer;   // no animator
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  const std::vector<std::string> caps = {"cosmetic"};
  // Every binding raises rather than fabricating animation state, matching the
  // combat-state convention.
  CHECK(Call(bindings, "animator.add_clip",
             {ScriptValue::Str("c"), ScriptValue::Num(1.0), ScriptValue::Bool(true)},
             caps, &out) == ScriptStatus::Error);
  CHECK(Call(bindings, "animator.play", {ScriptValue::Str("c")}, caps, &out) == ScriptStatus::Error);
  CHECK(Call(bindings, "animator.playing", {}, caps, &out) == ScriptStatus::Error);
  CHECK(Call(bindings, "animator.clip_count", {}, caps, &out) == ScriptStatus::Error);
  CHECK(Call(bindings, "animator.sample",
             {ScriptValue::Str("head"), ScriptValue::Num(0.0)}, caps, &out) == ScriptStatus::Error);
  CHECK(Call(bindings, "animator.target", {}, caps, &out) == ScriptStatus::Error);

  // The capability gate still applies: a caller without `cosmetic` is denied.
  Animator animator;
  Test::ShimSkeleton base;
  AnimatedSkeletonSource seam(&base, &animator);
  ScriptApi::Services svc2;
  svc2.renderer = &renderer;
  svc2.animator = &animator;
  svc2.animatorSeam = &seam;
  ScriptApi api2(svc2);
  ScriptBindings b2;
  api2.InstallBindings(&b2);
  CHECK(Call(b2, "animator.play", {ScriptValue::Str("c")}, {}, &out) ==
        ScriptStatus::CapabilityDenied);
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
  TestCombatStateBindings();
  TestCombatStateFailsClosedWithoutService();
  TestUiBindings();
  TestUiBindingsFailClosedWithoutService();
  TestUiThroughLuauScript();
  TestUiFrameRecoveryAfterFault();
  TestModBindings();
  TestFsAndAudioBindings();
  TestPvpBindings();
  TestAnimatorBindings();
  TestAnimatorFailsClosedWithoutService();
  TestPacketBindings();
  TestNetworkGetFailsClosed();
  TestDrawCommandsAreQueuedThenApplied();
  TestHudLifecycleAcrossThreads();
  TestQueueIsBoundedAndFailsClosed();
  TestFrameHookDrainsQueue();
  TestFrameHookAdvancesAnimator();
  TestReplayCameraBindings();
  TestReplayCameraExportThroughFrameHook();
  TestReplayCameraFollowsTape();
  TestFrameHookDrawsCosmetics();
  TestFrameHookPresentsUi();
  TestPublishedReads();
  TestGovernorAndZoomWrites();
  TestCombatAndInputReads();
  TestWorldQueryNeedsSeamAndLocalPlayer();
  TestPlayerReadsFailClosedAndResolve();
  TestRenderDrawHelpers();
  TestRenderOcclusionBindings();
  TestWorldReads();
  TestPerfStatsAndPlatform();
  TestPerfFlameBindings();
  TestPerfPipelineBindings();
  TestPublishFrameDrivesThePipeline();
  if (g_fail == 0) printf("test_api_bindings: ALL OK\n");
  return g_fail;
}
