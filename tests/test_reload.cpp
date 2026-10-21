// Host tests for hot reload + state handoff (Phase 3).
//
// Covers:
//   * Atomic reload: a broken revision never displaces a working script.
//   * Revision tracking: an unchanged source does not reload; ForceReload does.
//   * State handoff across a reload (counters survive).
//   * Overlay data for the HUD + clearing on a successful reload.
//   * FileScriptSource content-hash revision + missing-file handling.
#include "berry/reload.h"
#include "berry/render.h"
#include "shim_level.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static CapabilitySet Grant(const std::vector<std::string>& caps) {
  CapabilitySet s;
  for (const auto& c : caps) s.Add(c);
  return s;
}

static const char* kGoodV1 =
    "func on_load\n"
    "  push 0\n"
    "  store counter\n"
    "  ret 0\n"
    "end\n"
    "func bump\n"
    "  load counter\n"
    "  push 1\n"
    "  add\n"
    "  store counter\n"
    "  load counter\n"
    "  ret 1\n"
    "end\n"
    "func get\n"
    "  load counter\n"
    "  ret 1\n"
    "end\n";

// Same state shape, different behavior: bump adds 10.
static const char* kGoodV2 =
    "func on_load\n"
    "  push 0\n"
    "  store counter\n"
    "  ret 0\n"
    "end\n"
    "func bump\n"
    "  load counter\n"
    "  push 10\n"
    "  add\n"
    "  store counter\n"
    "  load counter\n"
    "  ret 1\n"
    "end\n"
    "func get\n"
    "  load counter\n"
    "  ret 1\n"
    "end\n";

static const char* kBroken =
    "func on_load\n"
    "  frobnicate\n"
    "end\n";

static void TestAtomicReload() {
  MemoryScriptSource source;
  source.Set(kGoodV1);
  ScriptReloader reloader("hot", Grant({"render"}), &source, nullptr);
  ScriptError err;
  CHECK(reloader.InitialLoad(&err));
  CHECK(reloader.host() != nullptr);
  CHECK(reloader.host()->loaded());
  CHECK(reloader.lastRevision() == 1);

  std::vector<ScriptValue> ret;
  CHECK(reloader.host()->RunHook("on_load") == ScriptStatus::Ok);
  CHECK(reloader.host()->RunHook("bump", {}, &ret) == ScriptStatus::Ok);
  CHECK(ret[0].AsNumber() == 1);

  // Poll with no change: nothing happens.
  ReloadResult r = reloader.Poll();
  CHECK(r.unchanged);
  CHECK(!r.reloaded);
  CHECK(reloader.reloads() == 0);

  // A good new revision reloads and hands off state (counter == 1).
  source.Set(kGoodV2);
  r = reloader.Poll();
  CHECK(r.attempted);
  CHECK(r.reloaded);
  CHECK(!r.compileFailed);
  CHECK(r.stateRestored);
  CHECK(reloader.reloads() == 1);
  CHECK(reloader.lastRevision() == 2);
  CHECK(reloader.host()->RunHook("get", {}, &ret) == ScriptStatus::Ok);
  CHECK(ret[0].AsNumber() == 1);   // carried over
  // New behavior is live: bump adds 10 now.
  CHECK(reloader.host()->RunHook("bump", {}, &ret) == ScriptStatus::Ok);
  CHECK(ret[0].AsNumber() == 11);

  // A broken revision must NOT displace the working script.
  source.Set(kBroken);
  r = reloader.Poll();
  CHECK(r.attempted);
  CHECK(r.compileFailed);
  CHECK(!r.reloaded);
  CHECK(r.error.occurred);
  CHECK(reloader.failedReloads() == 1);
  // Still running the previous revision.
  CHECK(reloader.host() != nullptr);
  CHECK(reloader.host()->loaded());
  CHECK(reloader.host()->RunHook("get", {}, &ret) == ScriptStatus::Ok);
  CHECK(ret[0].AsNumber() == 11);

  // Overlay is visible with the failure.
  CHECK(reloader.overlay().visible());
  auto lines = reloader.overlay().Lines();
  CHECK(!lines.empty());
  CHECK(lines[0].isError);

  // A subsequent good revision recovers and clears the overlay.
  source.Set(kGoodV1);
  r = reloader.Poll();
  CHECK(r.reloaded);
  CHECK(!reloader.overlay().visible());
  CHECK(reloader.reloads() == 2);
}

static void TestForceReload() {
  MemoryScriptSource source;
  source.Set(kGoodV1);
  ScriptReloader reloader("forced", Grant({"render"}), &source, nullptr);
  CHECK(reloader.InitialLoad(nullptr));

  // Force reload with an unchanged revision still swaps.
  ReloadResult r = reloader.ForceReload();
  CHECK(r.attempted);
  CHECK(r.reloaded);
  CHECK(!r.unchanged);
  CHECK(reloader.reloads() == 1);

  // Normal Poll is unchanged again.
  CHECK(reloader.Poll().unchanged);
}

static void TestUnavailableSource() {
  MemoryScriptSource source;   // never Set(): Read() fails
  ScriptReloader reloader("missing", Grant({"render"}), &source, nullptr);
  ScriptError err;
  CHECK(!reloader.InitialLoad(&err));
  CHECK(err.occurred);
  CHECK(reloader.overlay().visible());

  // Poll on an unavailable source is a failed reload, not a crash.
  ReloadResult r = reloader.Poll();
  CHECK(r.attempted);
  CHECK(r.compileFailed);
}

static void TestReloadAcrossStateShapeChange() {
  MemoryScriptSource source;
  source.Set(kGoodV1);
  ScriptReloader reloader("shape", Grant({"render"}), &source, nullptr);
  CHECK(reloader.InitialLoad(nullptr));
  CHECK(reloader.host()->RunHook("on_load") == ScriptStatus::Ok);
  std::vector<ScriptValue> ret;
  CHECK(reloader.host()->RunHook("bump", {}, &ret) == ScriptStatus::Ok);

  // A revision with a different state shape still reloads successfully; the
  // handoff is best-effort and the new revision runs.
  const char* stateless =
      "func on_load\n  ret 0\nend\n"
      "func get\n  push 0\n  ret 1\nend\n";
  source.Set(stateless);
  ReloadResult r = reloader.Poll();
  CHECK(r.reloaded);
  CHECK(reloader.host()->loaded());
  CHECK(reloader.host()->RunHook("get", {}, &ret) == ScriptStatus::Ok);
  CHECK(ret[0].AsNumber() == 0);

  // A state string the VM cannot parse is reported, not fatal: drive the
  // restore path directly to confirm the reloader would skip the handoff.
  ScriptError err;
  CHECK(!reloader.host()->RestoreState("garbage", &err));
  CHECK(err.occurred);
  CHECK(reloader.host()->loaded());   // still usable after a bad handoff
}

static void TestOverlayLines() {
  ScriptErrorOverlay overlay;
  CHECK(!overlay.visible());
  CHECK(overlay.Lines().empty());

  overlay.Set("myscript", ScriptError::Make("undefined symbol 'foo'", "main", 12));
  CHECK(overlay.visible());
  CHECK(overlay.script() == "myscript");
  auto lines = overlay.Lines();
  CHECK(lines.size() == 3);
  CHECK(lines[0].isError);
  CHECK(lines[0].text.find("myscript") != std::string::npos);
  CHECK(lines[1].text.find("main:12") != std::string::npos);
  CHECK(lines[1].text.find("undefined symbol 'foo'") != std::string::npos);
  CHECK(!lines[1].isError);

  overlay.Clear();
  CHECK(!overlay.visible());
  CHECK(overlay.Lines().empty());
}

static void TestFileSource() {
  const std::string path = "/tmp/berry_test_reload_source.luau";
  {
    FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (f) {
      std::fputs(kGoodV1, f);
      std::fclose(f);
    }
  }
  FileScriptSource src(path);
  std::string content;
  u64 rev1 = 0;
  CHECK(src.Read(&content, &rev1));
  CHECK(content == kGoodV1);
  CHECK(rev1 != 0);

  // Re-reading the same content yields the same revision (touch-safe).
  u64 rev2 = 0;
  CHECK(src.Read(&content, &rev2));
  CHECK(rev1 == rev2);

  // Changing the content changes the revision.
  {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f) {
      std::fputs(kGoodV2, f);
      std::fclose(f);
    }
  }
  u64 rev3 = 0;
  CHECK(src.Read(&content, &rev3));
  CHECK(rev3 != rev1);
  CHECK(content == kGoodV2);

  // A missing file fails closed.
  FileScriptSource missing("/tmp/berry_no_such_file_12345.luau");
  CHECK(!missing.Read(&content, &rev1));

  std::remove(path.c_str());
}

static void TestDrawErrorOverlay() {
  // Rendering the overlay must issue text draws through the Renderer.
  ScriptErrorOverlay overlay;
  overlay.Set("drawtest", ScriptError::Make("boom", "main", 3));
  Renderer r;
  auto capture = std::make_shared<Berry::Test::CaptureRenderSeam>();
  r.BindSeam(capture.get());
  r.BeginFrame(Vec2(1280, 720));
  DrawErrorOverlay(r, overlay, Vec2(1280, 720));
  r.EndFrame();
  CHECK(capture->texts == 3);
  CHECK(capture->textContents.size() == 3);
}

int main() {
  TestAtomicReload();
  TestForceReload();
  TestUnavailableSource();
  TestReloadAcrossStateShapeChange();
  TestOverlayLines();
  TestFileSource();
  TestDrawErrorOverlay();
  if (g_fail == 0) printf("test_reload: ALL OK\n");
  return g_fail;
}
