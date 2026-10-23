// Host tests for the replay mod service and its script surface (ReplayMod).
//
// The service is the one object the engine owns; the engine drives its tick and
// frame, and the `replaymod.*` bindings drive the rest. These tests cover both
// halves of Rule 9:
//
//   * the production caller — a real Engine tick feeds the capture and a real
//     Engine frame pumps the server and advances the review, with no script in
//     the picture; and
//   * the script surface — every binding is exercised through the real
//     capability gate and returns a real result, not a placeholder.
#include "berry/replay_service.h"

#include "berry/api_bindings.h"
#include "berry/engine.h"
#include "berry/script.h"
#include "berry/services.h"

#include "shim_level.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;
using Berry::Test::ShimLevel;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

EntityId Id(u64 raw) {
  EntityId id;
  id.raw = raw;
  id.runtime = (u16)raw;
  return id;
}

EntitySnapshot Snap(u64 raw, const Vec3& pos, EntityType type = EntityType::Player) {
  EntitySnapshot s;
  s.id = Id(raw);
  s.type = type;
  s.position = pos;
  s.velocity = Vec3(0.1f, 0, 0);
  s.health = 20;
  s.maxHealth = 20;
  s.alive = true;
  s.name = "P" + std::to_string(raw);
  return s;
}

// Records the tape the service produced by driving the capture directly (the
// same call the World tick makes).
void RecordTape(ReplayService& svc, ShimLevel& level, u64 ticks) {
  CHECK(svc.capture().Start());
  for (u64 t = 1; t <= ticks; ++t) {
    level.AddEntity(Id(1), Snap(1, Vec3((f32)t, 0, 0)));
    level.AddEntity(Id(2), Snap(2, Vec3(0, (f32)t, 0), EntityType::Mob));
    svc.Tick(t, level, {});
  }
  CHECK(svc.capture().Stop());
}

//----------------------------------------------------------------------------
// Production caller: the engine drives the service with no script bound.
//----------------------------------------------------------------------------

static void TestEngineDrivesService() {
  Engine& eng = Engine::Instance();
  CHECK(eng.Bootstrap());
  CHECK(eng.Attach());

  WorldHandle* world = WorldFrom(eng.Modules());
  CHECK(world != nullptr);
  auto shim = std::make_shared<ShimLevel>();
  shim->AddEntity(Id(1), Snap(1, Vec3(0, 0, 0)));
  world->SetSeam(shim);

  ReplayService& svc = eng.Replay();
  CHECK(!svc.capture().recording());

  // The engine's tick feeds the capture once a session is started. Nothing
  // else calls it, so this is the production caller.
  CHECK(svc.capture().Start());
  eng.OnGameTick(2);
  eng.OnGameTick(3);
  CHECK(svc.capture().sectionCount() > 0);
  CHECK(svc.capture().Stop());

  // The engine's frame pumps the server (a no-op when stopped) and advances
  // the review. A frame with no review active must not fault.
  eng.OnFrame();
  eng.OnFrame();

  // Save/load round-trips through the service: the engine-owned file survives.
  std::string tape;
  CHECK(svc.Save(&tape) == ReplayError::Ok);
  CHECK(!tape.empty());
  ReplayError err = ReplayError::Ok;
  CHECK(svc.Load(tape, &err));
  CHECK(svc.loaded());
  CHECK(svc.file() != nullptr);
  CHECK(svc.file()->sectionCount() > 0);
  CHECK(svc.world() != nullptr);

  // A malformed document is refused as a whole and leaves the replay loaded.
  ReplayError bad = ReplayError::Ok;
  CHECK(!svc.Load("not a replay at all", &bad));
  CHECK(bad != ReplayError::Ok);
  CHECK(svc.loaded());

  svc.Unload();
  CHECK(!svc.loaded());

  world->SetSeam(nullptr);
  eng.Detach();
}

//----------------------------------------------------------------------------
// The review draws through the real renderer when active (production caller).
//----------------------------------------------------------------------------

static void TestServiceReviewDraws() {
  ReplayService svc;
  ShimLevel level;
  RecordTape(svc, level, 4);

  std::string tape;
  CHECK(svc.Save(&tape) == ReplayError::Ok);
  CHECK(svc.Load(tape, nullptr));

  Renderer renderer;
  Berry::Test::CaptureRenderSeam seam;
  renderer.BindSeam(&seam);
  CameraState cam;
  cam.width = 1280;
  cam.height = 720;
  cam.view = Mat4::LookAt(Vec3(0, 0, 30), Vec3(0, 0, 0), Vec3(0, 1, 0));
  cam.projection = Mat4::Perspective(70, 1280.0f / 720.0f, 0.1f, 500.0f);
  cam.valid = true;
  renderer.SetCamera(cam);

  // Inactive: nothing is drawn, and the return is 0.
  CHECK(!svc.review().active());
  renderer.BeginFrame(Vec2(1280, 720));
  CHECK(svc.PresentReview(cam, renderer, Vec2(1280, 720)) == 0);
  renderer.EndFrame();
  CHECK(seam.rects == 0);

  // Active over a loaded replay: the review draws its boxes through the one
  // RenderSeam. This is the production caller of ReplayReview::Present.
  CHECK(svc.review().Play(svc.world()));
  svc.review().SetActive(true);
  svc.review().Seek(2);
  renderer.BeginFrame(Vec2(1280, 720));
  const size_t drawn = svc.PresentReview(cam, renderer, Vec2(1280, 720));
  renderer.EndFrame();
  CHECK(drawn > 0);
  CHECK(seam.rects > 0);
}

//----------------------------------------------------------------------------
// Bindings through the real gate.
//----------------------------------------------------------------------------

namespace {

ScriptStatus CallBinding(ScriptBindings& bindings, const std::string& name,
                         const std::vector<ScriptValue>& args,
                         const std::vector<std::string>& grants,
                         ScriptValue* out) {
  CapabilitySet s;
  for (const auto& c : grants) s.Add(c);
  ScriptHost host("replaymod-test", s, nullptr);
  host.SetBindings(&bindings);
  return host.CallBinding(name, args, out);
}

}  // namespace

static void TestReplayModBindings() {
  ReplayService svc;
  ScriptApi::Services services;
  services.replay = &svc;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;

  // Capture session control.
  CHECK(CallBinding(bindings, "replaymod.capture_start", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsBool() && out.Truthy());
  CHECK(CallBinding(bindings, "replaymod.capturing", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());

  // Feed the tape the way the engine does, then read it back. The entity
  // moves each tick so the deltas carry a record for every tick.
  ShimLevel level;
  for (u64 t = 1; t <= 3; ++t) {
    level.AddEntity(Id(1), Snap(1, Vec3((f32)t, 0, 0)));
    svc.Tick(t, level, {});
  }

  CHECK(CallBinding(bindings, "replaymod.section_count", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() > 0);
  CHECK(CallBinding(bindings, "replaymod.bytes", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() > 0);
  CHECK(CallBinding(bindings, "replaymod.first_tick", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 1.0);
  CHECK(CallBinding(bindings, "replaymod.last_tick", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 3.0);

  CHECK(CallBinding(bindings, "replaymod.save", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsString() && !out.AsString().empty());
  const std::string tape = out.AsString();

  CHECK(CallBinding(bindings, "replaymod.capture_stop", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());

  // Load it back through the binding and answer world reads from the tape.
  CHECK(CallBinding(bindings, "replaymod.load", {ScriptValue::Str(tape)},
                    {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.IsBool() && out.Truthy());
  CHECK(CallBinding(bindings, "replaymod.loaded", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(CallBinding(bindings, "replaymod.last_error", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsString() && out.AsString().empty());
  CHECK(CallBinding(bindings, "replaymod.loaded_sections", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() > 0);
  CHECK(CallBinding(bindings, "replaymod.loaded_first_tick", {}, {"world"},
                    &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 1.0);

  CHECK(CallBinding(bindings, "replaymod.tick_range", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsTable());
  CHECK(out.AsTable()->at("first").AsNumber() == 1.0);
  CHECK(out.AsTable()->at("last").AsNumber() == 3.0);

  // World reads come from the reconstructed replay, not the live world.
  CHECK(CallBinding(bindings, "replaymod.world_entity_count",
                    {ScriptValue::Num(2.0)}, {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 1.0);
  CHECK(CallBinding(bindings, "replaymod.world_entity",
                    {ScriptValue::Num(2.0), ScriptValue::Num(0.0)}, {"world"},
                    &out) == ScriptStatus::Ok);
  CHECK(out.IsTable());
  CHECK(out.AsTable()->count("id") == 1);
  CHECK(out.AsTable()->count("type") == 1);
  CHECK(out.AsTable()->at("x").AsNumber() == 2.0);

  // An out-of-range entity index is an empty table, not a crash.
  CHECK(CallBinding(bindings, "replaymod.world_entity",
                    {ScriptValue::Num(2.0), ScriptValue::Num(99.0)}, {"world"},
                    &out) == ScriptStatus::Ok);
  CHECK(out.IsTable() && out.AsTable()->empty());

  // Review control.
  CHECK(CallBinding(bindings, "replaymod.set_review_active",
                    {ScriptValue::Bool(true)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(CallBinding(bindings, "replaymod.review_active", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(CallBinding(bindings, "replaymod.review_play", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(CallBinding(bindings, "replaymod.review_playing", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(CallBinding(bindings, "replaymod.review_seek", {ScriptValue::Num(2.0)},
                    {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(CallBinding(bindings, "replaymod.review_tick", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 2.0);

  // A non-finite or non-positive speed is refused, and the old speed holds.
  CHECK(CallBinding(bindings, "replaymod.set_review_speed",
                    {ScriptValue::Num(-1.0)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsBool() && !out.Truthy());
  CHECK(CallBinding(bindings, "replaymod.set_review_speed",
                    {ScriptValue::Num(2.0)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(CallBinding(bindings, "replaymod.review_speed", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() == 2.0);
  CHECK(CallBinding(bindings, "replaymod.review_pause", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(CallBinding(bindings, "replaymod.review_playing", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(!out.Truthy());

  // Loopback server: a non-loopback host is refused (the invariant that keeps
  // a recording on the device), and a loopback one binds.
  CHECK(CallBinding(bindings, "replaymod.server_start",
                    {ScriptValue::Str("0.0.0.0"), ScriptValue::Num(0.0)},
                    {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.IsBool() && !out.Truthy());
  CHECK(CallBinding(bindings, "replaymod.last_error", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsString() && !out.AsString().empty());

  CHECK(CallBinding(bindings, "replaymod.server_start",
                    {ScriptValue::Str("127.0.0.1"), ScriptValue::Num(0.0)},
                    {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.IsBool() && out.Truthy());
  CHECK(CallBinding(bindings, "replaymod.server_running", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(CallBinding(bindings, "replaymod.server_port", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsNumber() > 0);
  CHECK(CallBinding(bindings, "replaymod.server_client_connected", {},
                    {"world"}, &out) == ScriptStatus::Ok);
  CHECK(!out.Truthy());
  // Seek without a client is refused.
  CHECK(CallBinding(bindings, "replaymod.server_seek", {ScriptValue::Num(1.0)},
                    {"world"}, &out) == ScriptStatus::Ok);
  CHECK(!out.Truthy());
  CHECK(CallBinding(bindings, "replaymod.server_set_paused",
                    {ScriptValue::Bool(true)}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(CallBinding(bindings, "replaymod.server_paused", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(CallBinding(bindings, "replaymod.server_packets_sent", {}, {"world"},
                    &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 0.0);
  CHECK(CallBinding(bindings, "replaymod.server_stop", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(CallBinding(bindings, "replaymod.server_running", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(!out.Truthy());

  // Status is a real, non-empty line.
  CHECK(CallBinding(bindings, "replaymod.status", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsString() && !out.AsString().empty());

  // Unload clears the loaded replay.
  CHECK(CallBinding(bindings, "replaymod.unload", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(CallBinding(bindings, "replaymod.loaded", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(!out.Truthy());

  // A bad document fails closed: false with a reason in `last_error` (not an
  // exception, so a script can branch on it).
  CHECK(CallBinding(bindings, "replaymod.load", {ScriptValue::Str("garbage")},
                    {"world"}, &out) == ScriptStatus::Ok);
  CHECK(out.IsBool() && !out.Truthy());
  CHECK(CallBinding(bindings, "replaymod.last_error", {}, {"world"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.IsString() && !out.AsString().empty());

  // Not granted: denied, and nothing changes.
  ScriptValue denied;
  CHECK(CallBinding(bindings, "replaymod.capture_start", {}, {}, &denied) ==
        ScriptStatus::CapabilityDenied);
  CHECK(CallBinding(bindings, "replaymod.save", {}, {}, &denied) ==
        ScriptStatus::CapabilityDenied);

  // No service: fail closed on the calls that act, neutral on the reads.
  ScriptApi::Services none;
  ScriptApi bare(none);
  ScriptBindings bareBindings;
  bare.InstallBindings(&bareBindings);
  ScriptValue bad;
  CHECK(CallBinding(bareBindings, "replaymod.capture_start", {}, {"world"},
                    &bad) == ScriptStatus::Error);
  CHECK(CallBinding(bareBindings, "replaymod.loaded", {}, {"world"}, &bad) ==
        ScriptStatus::Ok);
  CHECK(!bad.Truthy());
  CHECK(CallBinding(bareBindings, "replaymod.status", {}, {"world"}, &bad) ==
        ScriptStatus::Ok);
  CHECK(bad.IsString() && !bad.AsString().empty());
}

// The frame hook draws the review through the service (the script-driven
// production path), proving the hook and the service agree.
static void TestFrameHookDrawsReview() {
  ReplayService svc;
  ShimLevel level;
  RecordTape(svc, level, 4);
  std::string tape;
  CHECK(svc.Save(&tape) == ReplayError::Ok);
  CHECK(svc.Load(tape, nullptr));
  CHECK(svc.review().Play(svc.world()));
  svc.review().SetActive(true);
  svc.review().Seek(2);

  Renderer renderer;
  Berry::Test::CaptureRenderSeam seam;
  renderer.BindSeam(&seam);
  CameraState cam;
  cam.width = 1280;
  cam.height = 720;
  cam.view = Mat4::LookAt(Vec3(0, 0, 30), Vec3(0, 0, 0), Vec3(0, 1, 0));
  cam.projection = Mat4::Perspective(70, 1280.0f / 720.0f, 0.1f, 500.0f);
  cam.valid = true;
  renderer.SetCamera(cam);

  ScriptApi::Services services;
  services.renderer = &renderer;
  services.replay = &svc;
  ScriptApi api(services);
  api.PublishFrame(16.0f, Vec2(1280, 720));

  Engine& eng = Engine::Instance();
  eng.Bootstrap();
  CHECK(eng.Attach());

  renderer.BeginFrame(Vec2(1280, 720));
  eng.OnFrame();          // emits berry:frame; the hook draws the review
  renderer.EndFrame();
  CHECK(seam.rects > 0);

  eng.Detach();
}

}  // namespace

int main() {
  TestEngineDrivesService();
  TestServiceReviewDraws();
  TestReplayModBindings();
  TestFrameHookDrawsReview();

  if (g_fail == 0) {
    printf("test_replay_service: ALL OK\n");
    return 0;
  }
  printf("test_replay_service: %d FAILURES\n", g_fail);
  return 1;
}
