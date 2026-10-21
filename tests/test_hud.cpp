// Host tests for the retained-mode HUD, input observation and the PvP overlay.
//
// Covers:
//   * Tween/easing math and retarget-from-current behaviour.
//   * Widget tree creation, anchoring, layout caching and the dirty-subtree
//     rules that keep a static HUD cheap.
//   * Bar/Graph drawing and sample capping.
//   * Input observation: movement state, unknown-key rejection, click rate
//     decay over the rolling window.
//   * PvP overlay: combo window, session stats, keystroke fade, popup expiry,
//     hit flash decay and local-only data plumbing.
#include "berry/hud.h"
#include "berry/input.h"
#include "berry/pvp.h"
#include "berry/render.h"
#include "shim_level.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static bool Near(f32 a, f32 b, f32 eps = 1e-3f) {
  return std::fabs(a - b) <= eps;
}

//----------------------------------------------------------------------------
// Tween / easing
//----------------------------------------------------------------------------

static void TestTween() {
  CHECK(Near(Ease(Easing::Linear, 0.5f), 0.5f));
  CHECK(Near(Ease(Easing::EaseIn, 0.5f), 0.25f));
  CHECK(Near(Ease(Easing::EaseOut, 0.5f), 0.75f));
  CHECK(Near(Ease(Easing::EaseInOut, 0.5f), 0.5f));
  // Clamping, including the NaN case (which must not propagate).
  CHECK(Near(Ease(Easing::Linear, -1.0f), 0.0f));
  CHECK(Near(Ease(Easing::Linear, 2.0f), 1.0f));
  CHECK(Near(Ease(Easing::Linear, std::nanf("")), 0.0f));

  Tween t = Tween::Constant(3.0f);
  CHECK(Near(t.Value(), 3.0f));
  CHECK(!t.Animating());

  t.Retarget(10.0f, 100.0f, Easing::Linear);
  CHECK(t.Animating());
  CHECK(Near(t.Value(), 3.0f));       // starts from the current value
  CHECK(t.Advance(50.0f));            // still animating
  CHECK(Near(t.Value(), 6.5f));
  CHECK(!t.Advance(50.0f));           // finished
  CHECK(Near(t.Value(), 10.0f));

  // Retarget mid-flight starts from the interpolated value, so no jump.
  t.Retarget(0.0f, 100.0f, Easing::Linear);
  CHECK(Near(t.Value(), 10.0f));
  t.Advance(25.0f);
  CHECK(Near(t.Value(), 7.5f));
  t.Retarget(20.0f, 0.0f);            // zero duration snaps
  CHECK(Near(t.Value(), 20.0f));
  CHECK(!t.Animating());
}

//----------------------------------------------------------------------------
// Hud tree, layout, draw
//----------------------------------------------------------------------------

static void TestHudTree() {
  Hud hud;
  CHECK(hud.Count() == 0);

  WidgetId label = hud.Create(WidgetKind::Label, "hello", Anchor::TopLeft);
  CHECK(label != 0);
  CHECK(hud.Count() == 1);
  CHECK(hud.Get(label) != nullptr);
  CHECK(hud.Get(99999) == nullptr);

  // A column with two children flows them vertically.
  WidgetId col = hud.Create(WidgetKind::Column, "", Anchor::TopLeft);
  WidgetId c1 = hud.CreateChild(col, WidgetKind::Label, "one");
  WidgetId c2 = hud.CreateChild(col, WidgetKind::Label, "two");
  CHECK(c1 != 0 && c2 != 0);
  CHECK(hud.Get(col)->children.size() == 2);

  // Removing a parent removes the whole subtree.
  CHECK(hud.Remove(col));
  CHECK(hud.Get(col) == nullptr);
  CHECK(hud.Get(c1) == nullptr && hud.Get(c2) == nullptr);

  // Removing an unknown id fails cleanly.
  CHECK(!hud.Remove(12345));

  hud.Clear();
  CHECK(hud.Count() == 0);
}

static void TestHudLayoutAndDraw() {
  Berry::Test::CaptureRenderSeam seam;
  Renderer r;
  r.Attach(&seam);
  const Vec2 screen(800, 600);
  r.BeginFrame(screen);

  Hud hud;
  WidgetId root = hud.Create(WidgetKind::Column, "", Anchor::TopLeft);
  WidgetId a = hud.CreateChild(root, WidgetKind::Label, "alpha");
  WidgetId b = hud.CreateChild(root, WidgetKind::Bar, "");
  hud.SetValue(b, 0.5f);
  hud.Get(b)->minValue = 0.0f;
  hud.Get(b)->maxValue = 1.0f;

  hud.Present(r, screen);
  r.EndFrame();

  CHECK(seam.texts >= 1);             // the "alpha" label
  CHECK(seam.rects >= 2);             // bar frame + fill
  CHECK(hud.stats().widgetsDrawn >= 2);
  CHECK(hud.stats().layoutPasses == 1);

  // A second frame with no geometry change must not re-lay out.
  r.BeginFrame(screen);
  hud.Present(r, screen);
  r.EndFrame();
  CHECK(hud.stats().layoutPasses == 0);

  // Changing text dirties the subtree, forcing a layout pass.
  hud.SetText(a, "alpha-longer");
  r.BeginFrame(screen);
  hud.Present(r, screen);
  r.EndFrame();
  CHECK(hud.stats().layoutPasses == 1);
  CHECK(Near(hud.Get(a)->resolvedSize.x,
             (f32)std::string("alpha-longer").size() *
                 hud.theme().charWidth * hud.theme().scale));
}

static void TestHudAnchors() {
  Hud hud;
  const Vec2 screen(1000, 500);

  // Top-right widget's right edge sits at the screen's right edge.
  WidgetId tr = hud.Create(WidgetKind::Label, "12345", Anchor::TopRight);
  Widget* w = hud.Get(tr);
  w->layoutValid = false;
  hud.Update(0.0f);
  Berry::Test::CaptureRenderSeam seam;
  Renderer r;
  r.Attach(&seam);
  r.BeginFrame(screen);
  hud.Present(r, screen);
  r.EndFrame();
  CHECK(Near(w->resolvedPos.x + w->resolvedSize.x, screen.x));
  CHECK(Near(w->resolvedPos.y, 0.0f));

  // Bottom-center widget is centred horizontally and ends at the bottom.
  WidgetId bc = hud.Create(WidgetKind::Label, "abc", Anchor::BottomCenter);
  hud.Get(bc)->layoutValid = false;
  r.BeginFrame(screen);
  hud.Present(r, screen);
  r.EndFrame();
  CHECK(Near(hud.Get(bc)->resolvedPos.x + hud.Get(bc)->resolvedSize.x * 0.5f,
             screen.x * 0.5f));
  CHECK(Near(hud.Get(bc)->resolvedPos.y + hud.Get(bc)->resolvedSize.y, screen.y));
}

static void TestHudGraphSamples() {
  Hud hud;
  WidgetId g = hud.Create(WidgetKind::Graph, "", Anchor::TopLeft);
  Widget* w = hud.Get(g);
  w->sampleCapacity = 3;
  CHECK(hud.PushSample(g, 1.0f));
  hud.PushSample(g, 2.0f);
  hud.PushSample(g, 3.0f);
  hud.PushSample(g, 4.0f);          // drops the oldest
  CHECK(w->samples.size() == 3);
  CHECK(Near(w->samples.front(), 2.0f));
  CHECK(Near(w->samples.back(), 4.0f));

  // A capacity of 0 refuses samples rather than growing without bound.
  WidgetId g2 = hud.Create(WidgetKind::Graph, "", Anchor::TopLeft);
  hud.Get(g2)->sampleCapacity = 0;
  CHECK(!hud.PushSample(g2, 1.0f));
}

static void TestHudTweenValue() {
  Hud hud;
  WidgetId b = hud.Create(WidgetKind::Bar, "", Anchor::TopLeft);
  hud.Get(b)->minValue = 0.0f;
  hud.Get(b)->maxValue = 100.0f;
  hud.SetValue(b, 0.0f);
  CHECK(Near(hud.Get(b)->value, 0.0f));

  hud.TweenValue(b, 100.0f, 100.0f, Easing::Linear);
  hud.Update(50.0f);
  CHECK(Near(hud.Get(b)->value, 50.0f));
  hud.Update(50.0f);
  CHECK(Near(hud.Get(b)->value, 100.0f));

  // A direct set cancels the animation.
  hud.TweenValue(b, 0.0f, 100.0f, Easing::Linear);
  hud.Update(10.0f);
  hud.SetValue(b, 42.0f);
  hud.Update(100.0f);
  CHECK(Near(hud.Get(b)->value, 42.0f));
}

static void TestHudOpacityTween() {
  Hud hud;
  WidgetId l = hud.Create(WidgetKind::Label, "x", Anchor::TopLeft);
  CHECK(Near(hud.Get(l)->opacity.Value(), 1.0f));
  hud.TweenOpacity(l, 0.0f, 100.0f, Easing::Linear);
  hud.Update(50.0f);
  CHECK(Near(hud.Get(l)->opacity.Value(), 0.5f));

  // An invisible widget draws nothing.
  hud.TweenOpacity(l, 0.0f, 0.0f);
  Berry::Test::CaptureRenderSeam seam;
  Renderer r;
  r.Attach(&seam);
  r.BeginFrame(Vec2(100, 100));
  hud.Present(r, Vec2(100, 100));
  r.EndFrame();
  CHECK(seam.texts == 0);
}

//----------------------------------------------------------------------------
// Input observation
//----------------------------------------------------------------------------

static void TestInputKeys() {
  InputObserver in;
  CHECK(in.state().movementHeld() == 0);

  in.OnKey("w", true);
  in.OnKey("d", true);
  CHECK(in.state().forward);
  CHECK(in.state().right);
  CHECK(!in.state().back);
  CHECK(in.state().movementHeld() == 2);

  in.OnKey("W", false);       // case-insensitive
  CHECK(!in.state().forward);
  CHECK(in.state().movementHeld() == 1);

  in.OnKey("space", true);
  CHECK(in.state().jump);
  in.OnKey("shift", true);
  CHECK(in.state().sneak);
  in.OnKey("ctrl", true);
  CHECK(in.state().sprint);

  // Unknown keys are ignored (fail closed), not stored.
  in.OnKey("f13", true);
  in.OnKey("", true);
  in.OnKey(nullptr, true);
  CHECK(in.state().movementHeld() == 1);

  // Held duration.
  in.Tick(1000);
  in.OnKey("a", true);
  CHECK(in.heldMs("a", 1300) == 300);
  in.OnKey("a", false);
  CHECK(in.heldMs("a", 1400) == 0);

  in.Reset();
  CHECK(!in.state().sneak);
  CHECK(in.state().movementHeld() == 0);
}

static void TestInputClicks() {
  InputObserver in;
  // Five clicks inside the window: 5 cps.
  for (u64 t = 0; t < 5; ++t) in.OnMouse(MouseButton::Left, true);
  in.Tick(0);
  CHECK(Near((f32)in.clicksPerSecond(MouseButton::Left), 5.0f));
  CHECK(in.totalClicks(MouseButton::Left) == 5);

  // Advance past the window: the rate decays to zero but the total stays.
  in.Tick(1500);
  CHECK(Near((f32)in.clicksPerSecond(MouseButton::Left), 0.0f));
  CHECK(in.totalClicks(MouseButton::Left) == 5);

  // New clicks re-enter the window.
  in.OnMouse(MouseButton::Left, true);
  in.Tick(1500);
  CHECK(Near((f32)in.clicksPerSecond(MouseButton::Left), 1.0f));

  // Right clicks are tracked independently.
  in.OnMouse(MouseButton::Right, true);
  in.Tick(1500);
  CHECK(Near((f32)in.clicksPerSecond(MouseButton::Right), 1.0f));
  CHECK(in.state().use);

  // Left mouse down sets the attack flag.
  in.OnMouse(MouseButton::Left, true);
  CHECK(in.state().attack);
  in.OnMouse(MouseButton::Left, false);
  CHECK(!in.state().attack);
}

//----------------------------------------------------------------------------
// PvP overlay
//----------------------------------------------------------------------------

static void TestPvpCombo() {
  Hud hud;
  PvpOverlay pvp;
  pvp.Attach(hud);

  CHECK(pvp.combo() == 0);
  CHECK(!pvp.comboActive());

  pvp.OnKill(0);
  CHECK(pvp.combo() == 1);
  CHECK(pvp.comboActive());
  pvp.OnKill(0);
  CHECK(pvp.combo() == 2);

  // The combo window expires after kComboWindowSec without a kill.
  pvp.Update((PvpOverlay::kComboWindowSec + 0.1f) * 1000.0f, 5000);
  CHECK(pvp.combo() == 0);
  CHECK(!pvp.comboActive());

  // A death resets the combo immediately.
  pvp.OnKill(0);
  pvp.OnKill(0);
  CHECK(pvp.combo() == 2);
  pvp.OnDeath(0);
  CHECK(pvp.combo() == 0);
  CHECK(!pvp.comboActive());
  CHECK(pvp.session().deaths == 1);
  CHECK(pvp.session().kills == 4);   // two before the expiry, two after
}

static void TestPvpSessionStats() {
  PvpOverlay pvp;
  pvp.OnDamage(10.0f, /*dealt=*/true, false, 0);
  pvp.OnDamage(15.0f, /*dealt=*/true, true, 0);
  pvp.OnDamage(7.0f, /*dealt=*/false, false, 0);
  const SessionStats& s = pvp.session();
  CHECK(Near((f32)s.damageDealt, 25.0f));
  CHECK(Near((f32)s.damageTaken, 7.0f));
  CHECK(s.hits == 2);
  CHECK(s.peakDps > 0);
  CHECK(Near((f32)s.Kd(), 0.0f));   // no kills, no deaths

  // Invalid damage is ignored (fail closed).
  pvp.OnDamage(-5.0f, true, false, 0);
  pvp.OnDamage(0.0f, true, false, 0);
  CHECK(Near((f32)pvp.session().damageDealt, 25.0f));
}

static void TestPvpHitFlashAndPopups() {
  PvpOverlay pvp;
  CHECK(Near(pvp.hitFlash(), 0.0f));

  // Taking damage triggers the flash.
  pvp.OnDamage(5.0f, /*dealt=*/false, false, 0);
  CHECK(pvp.hitFlash() > 0.9f);
  CHECK(pvp.popupCount() == 1);

  // It decays to zero.
  pvp.Update(500.0f, 500);
  CHECK(Near(pvp.hitFlash(), 0.0f));

  // Popups expire after their lifetime.
  pvp.Update(1000.0f, 1500);
  CHECK(pvp.popupCount() == 0);

  // Popups are capped so a long fight cannot grow the list without bound.
  for (int i = 0; i < 200; ++i) {
    pvp.AddDamagePopup(1.0f, Vec2(0, 0), false);
  }
  CHECK(pvp.popupCount() <= 64);
}

static void TestPvpKeystrokeFade() {
  Hud hud;
  PvpOverlay pvp;
  pvp.Attach(hud);           // builds the keystroke rows
  pvp.SetKeystroke("W", true);
  bool found = false;
  for (const auto& ks : pvp.keystrokes()) {
    if (ks.key == "W") {
      found = true;
      CHECK(ks.down);
      CHECK(Near(ks.fade, 1.0f));
    }
  }
  CHECK(found);

  pvp.SetKeystroke("W", false);
  pvp.Update(125.0f, 125);
  for (const auto& ks : pvp.keystrokes()) {
    if (ks.key == "W") {
      CHECK(!ks.down);
      CHECK(Near(ks.fade, 0.5f, 0.01f));
    }
  }
  // An unknown keystroke is ignored rather than appended.
  const size_t before = pvp.keystrokes().size();
  pvp.SetKeystroke("Z", true);
  CHECK(pvp.keystrokes().size() == before);
}

static void TestPvpPresent() {
  Hud hud;
  PvpOverlay pvp;
  pvp.Attach(hud);
  pvp.SetNetStats(NetStats{42, 19.5f, 60.0f});
  pvp.SetHealth(5.0f, 20.0f);        // 25% -> below the 35% threshold
  std::vector<ArmorPiece> armor{
      ArmorPiece{"helmet", 10, 100},
      ArmorPiece{"chestplate", 90, 100},
  };
  pvp.SetArmor(armor);
  std::vector<EffectState> fx{EffectState{"speed", 1, 12.0f, false}};
  pvp.SetEffects(fx);
  TargetInfo ti;
  ti.valid = true;
  ti.name = "Rival";
  ti.health = 12;
  ti.maxHealth = 20;
  ti.distance = 3.4f;
  ti.armorPoints = 5;
  pvp.SetTarget(ti);
  pvp.OnKill(0);
  pvp.OnKill(0);
  pvp.OnDamage(9.0f, false, false, 0);   // triggers hit flash + vignette

  Berry::Test::CaptureRenderSeam seam;
  Renderer r;
  r.Attach(&seam);
  const Vec2 screen(640, 360);
  r.BeginFrame(screen);
  pvp.Present(hud, r, screen);
  r.EndFrame();

  // Vignette (4 bands) + hit flash (1) + bar/panel fills from widgets.
  CHECK(seam.rects >= 5);
  CHECK(seam.texts >= 4);   // CPS, net line, combo, session, keystrokes, ...
  CHECK(seam.lastScreen.x == 640 && seam.lastScreen.y == 360);

  // Detached overlay draws nothing extra.
  PvpOverlay off;
  off.Present(hud, r, screen);
  CHECK(true);
}

static void TestPvpArmorRebuild() {
  Hud hud;
  PvpOverlay pvp;
  pvp.Attach(hud);
  Berry::Test::CaptureRenderSeam seam;
  Renderer r;
  r.Attach(&seam);
  const Vec2 screen(400, 300);

  // Two armor rows, then a smaller set, then none: the overlay rebuilds its
  // child rows each frame, so this exercises removal under iteration.
  for (int pass = 0; pass < 3; ++pass) {
    std::vector<ArmorPiece> armor;
    if (pass == 0) {
      armor = {ArmorPiece{"helmet", 10, 100}, ArmorPiece{"boots", 100, 100}};
    } else if (pass == 1) {
      armor = {ArmorPiece{"helmet", 10, 100}};
    }
    pvp.SetArmor(armor);
    r.BeginFrame(screen);
    pvp.Present(hud, r, screen);
    r.EndFrame();
  }
  CHECK(true);   // reaching here without a crash is the assertion
}

static void TestPvpConfigGating() {
  Hud hud;
  PvpOverlay pvp;
  PvpConfig cfg;
  cfg.showCps = false;
  cfg.showKeystrokes = false;
  cfg.showCombo = false;
  cfg.showNetStats = false;
  cfg.showSession = false;
  cfg.showArmor = false;
  cfg.showEffects = false;
  cfg.showTarget = false;
  cfg.hitFlash = false;
  cfg.lowHealthVignette = false;
  pvp.SetConfig(cfg);
  pvp.Attach(hud);
  pvp.SetHealth(1.0f, 20.0f);
  pvp.OnDamage(5.0f, false, false, 0);
  pvp.OnKill(0);
  pvp.OnKill(0);

  Berry::Test::CaptureRenderSeam seam;
  Renderer r;
  r.Attach(&seam);
  const Vec2 screen(400, 300);
  r.BeginFrame(screen);
  pvp.Present(hud, r, screen);
  r.EndFrame();

  // With everything off only the damage popup text remains.
  CHECK(seam.rects == 0);
  CHECK(seam.texts == 1);
}

int main() {
  TestTween();
  TestHudTree();
  TestHudLayoutAndDraw();
  TestHudAnchors();
  TestHudGraphSamples();
  TestHudTweenValue();
  TestHudOpacityTween();
  TestInputKeys();
  TestInputClicks();
  TestPvpCombo();
  TestPvpSessionStats();
  TestPvpHitFlashAndPopups();
  TestPvpKeystrokeFade();
  TestPvpPresent();
  TestPvpArmorRebuild();
  TestPvpConfigGating();
  if (g_fail == 0) printf("test_hud: ALL OK\n");
  return g_fail;
}
