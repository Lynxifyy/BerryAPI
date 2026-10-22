// Host tests for the script module + settings system (Phase C).
//
// The registry is pure data: it never touches game memory, so it runs headless
// like every other service. These tests pin the properties a UI relies on:
//   * a module's name is unique and its settings are typed at declaration,
//   * every write is validated against the declared kind and range and fails
//     closed rather than clamping or storing something unrepresentable,
//   * a Button press is a one-shot edge, consumed by the read,
//   * the introspection strings are stable and round-trip the values.
#include "berry/scriptmod.h"
#include "berry/render.h"
#include "shim_level.h"

#include <cstdio>
#include <string>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static void TestCreateAndUniqueNames() {
  ScriptModuleRegistry reg;
  const u64 a = reg.Create("Combo", "counts combos", "1.2.0", "Lynx");
  const u64 b = reg.Create("Keystrokes", "", "", "");
  CHECK(a != 0);
  CHECK(b != 0);
  CHECK(a != b);
  CHECK(reg.Count() == 2);
  CHECK(reg.Has(a));

  // A duplicate name is refused; an empty name is refused.
  CHECK(reg.Create("Combo", "dup", "1.0.0", "") == 0);
  CHECK(reg.Create("", "", "", "") == 0);
  CHECK(reg.Count() == 2);

  ScriptModuleInfo info;
  CHECK(reg.GetModule(a, &info));
  CHECK(info.name == "Combo");
  CHECK(info.version == "1.2.0");
  CHECK(info.author == "Lynx");
  CHECK(!info.enabled);   // a fresh module starts disabled
  // A missing version defaults rather than staying empty.
  CHECK(reg.GetModule(b, &info));
  CHECK(info.version == "1.0.0");
}

static void TestTypedSettingsAndValidation() {
  ScriptModuleRegistry reg;
  const u64 m = reg.Create("HUD", "overlay", "1.0.0", "Lynx");

  CHECK(reg.AddToggle(m, "show_fps", "Show FPS", true));
  CHECK(reg.AddSlider(m, "scale", "Scale", 0.5, 2.0, 1.0));
  CHECK(reg.AddSelect(m, "anchor", "Anchor", {"tl", "tr", "bl", "br"}, 1));
  CHECK(reg.AddKeybind(m, "toggle", "Toggle key", "F4"));
  CHECK(reg.AddText(m, "label", "Label", "Berry"));
  CHECK(reg.AddButton(m, "reset", "Reset"));

  // A duplicate key or a bad key is refused.
  CHECK(!reg.AddToggle(m, "show_fps", "Dup", false));
  CHECK(!reg.AddToggle(m, "", "Empty", false));
  CHECK(!reg.AddToggle(m, "bad/key", "Sep", false));
  CHECK(!reg.AddToggle(m, "bad=key", "Sep", false));
  // A slider whose max is below its min is refused, not silently swapped.
  CHECK(!reg.AddSlider(m, "broken", "Broken", 5.0, 1.0, 3.0));
  // A select with no options is refused.
  CHECK(!reg.AddSelect(m, "empty", "Empty", {}, 0));

  f64 n = 0;
  CHECK(reg.GetNumber(m, "show_fps", &n));
  CHECK(n == 1.0);

  // Toggle accepts only 0/1; a slider rejects out-of-range; a select rejects an
  // out-of-range index. All fail closed rather than clamping.
  CHECK(reg.SetNumber(m, "show_fps", 0));
  CHECK(!reg.SetNumber(m, "show_fps", 2));
  CHECK(reg.SetNumber(m, "scale", 1.75));
  CHECK(!reg.SetNumber(m, "scale", 2.5));
  CHECK(!reg.SetNumber(m, "scale", 0.1));
  CHECK(reg.SetNumber(m, "anchor", 3));
  CHECK(!reg.SetNumber(m, "anchor", 4));
  CHECK(!reg.SetNumber(m, "anchor", -1));
  // A non-finite write never reaches the store.
  CHECK(!reg.SetNumber(m, "scale", 1.0 / 0.0 * 0.0));   // NaN
  CHECK(reg.SetNumber(m, "scale", 1.0));

  // Writing a number to a text setting (or vice versa) is refused.
  CHECK(!reg.SetNumber(m, "label", 5));
  CHECK(!reg.SetText(m, "scale", "big"));

  std::string s;
  CHECK(reg.GetText(m, "toggle", &s));
  CHECK(s == "F4");
  CHECK(reg.SetText(m, "toggle", "F5"));
  CHECK(reg.GetText(m, "toggle", &s));
  CHECK(s == "F5");

  // Unknown keys fail closed on both read and write.
  CHECK(!reg.GetNumber(m, "nope", &n));
  CHECK(!reg.SetNumber(m, "nope", 1));
  CHECK(!reg.GetText(m, "nope", &s));

  // A select default out of range falls back to the first option.
  const u64 m2 = reg.Create("Sel", "", "", "");
  CHECK(reg.AddSelect(m2, "mode", "Mode", {"a", "b"}, 9));
  CHECK(reg.GetNumber(m2, "mode", &n));
  CHECK(n == 0.0);
}

static void TestButtonIsOneShotEdge() {
  ScriptModuleRegistry reg;
  const u64 m = reg.Create("Mod", "", "", "");
  CHECK(reg.AddButton(m, "go", "Go"));

  SettingValue v;
  CHECK(reg.Get(m, "go", &v));
  CHECK(!v.pressed);   // idle before any press
  CHECK(reg.Press(m, "go"));
  CHECK(reg.Get(m, "go", &v));
  CHECK(v.pressed);
  // The press is consumed by the read: a second read sees idle.
  CHECK(reg.Get(m, "go", &v));
  CHECK(!v.pressed);

  // A press on a non-button (or unknown) setting is refused.
  CHECK(reg.AddToggle(m, "on", "On", false));
  CHECK(!reg.Press(m, "on"));
  CHECK(!reg.Press(m, "missing"));
}

static void TestEnableDisable() {
  ScriptModuleRegistry reg;
  const u64 m = reg.Create("Mod", "", "", "");
  CHECK(!reg.IsEnabled(m));
  CHECK(reg.Enable(m));
  CHECK(reg.IsEnabled(m));
  CHECK(reg.Disable(m));
  CHECK(!reg.IsEnabled(m));
  CHECK(reg.SetEnabled(m, true));
  CHECK(reg.IsEnabled(m));

  // An unknown handle is refused everywhere.
  CHECK(!reg.SetEnabled(999, true));
  CHECK(!reg.IsEnabled(999));
  CHECK(!reg.Enable(999));
}

static void TestIntrospection() {
  ScriptModuleRegistry reg;
  const u64 a = reg.Create("Alpha", "", "", "");
  const u64 b = reg.Create("Beta", "", "", "");
  reg.AddToggle(a, "on", "On", true);
  reg.AddSlider(a, "scale", "Scale", 0, 10, 2.5);
  reg.AddKeybind(b, "key", "Key", "R");
  reg.Enable(b);

  const std::string list = reg.List();
  CHECK(list.find(std::to_string(a) + ":Alpha:off") != std::string::npos);
  CHECK(list.find(std::to_string(b) + ":Beta:on") != std::string::npos);

  const std::string settings = reg.SettingsOf(a);
  CHECK(settings.find("on=1") != std::string::npos);
  CHECK(settings.find("scale=2.5") != std::string::npos);
  CHECK(reg.SettingsOf(999).empty());

  // The overlay defaults off and is a plain switch.
  CHECK(!reg.overlayVisible());
  reg.SetOverlayVisible(true);
  CHECK(reg.overlayVisible());
}

static void TestOverlayDraws() {
  ScriptModuleRegistry reg;
  reg.Create("Combo", "", "", "");
  const u64 b = reg.Create("Keys", "", "", "");
  reg.Enable(b);

  Renderer renderer;
  Berry::Test::CaptureRenderSeam seam;
  renderer.BindSeam(&seam);

  // Off by default: nothing is drawn even with modules present.
  renderer.BeginFrame(Vec2(1280, 720));
  reg.DrawOverlay(renderer, Vec2(1280, 720));
  renderer.EndFrame();
  CHECK(seam.rects == 0);
  CHECK(seam.texts == 0);

  // On: a panel plus a header plus one line per module.
  reg.SetOverlayVisible(true);
  renderer.BeginFrame(Vec2(1280, 720));
  reg.DrawOverlay(renderer, Vec2(1280, 720));
  renderer.EndFrame();
  CHECK(seam.rects == 1);
  CHECK(seam.texts == 3);   // "modules" + Combo + Keys

  // An empty registry draws nothing even when the overlay is on.
  reg.Clear();
  renderer.BeginFrame(Vec2(1280, 720));
  reg.DrawOverlay(renderer, Vec2(1280, 720));
  renderer.EndFrame();
  CHECK(seam.rects == 0);
  CHECK(seam.texts == 0);
}

int main() {
  TestCreateAndUniqueNames();
  TestTypedSettingsAndValidation();
  TestButtonIsOneShotEdge();
  TestEnableDisable();
  TestIntrospection();
  TestOverlayDraws();
  if (g_fail == 0) printf("test_scriptmod: ALL OK\n");
  return g_fail;
}
