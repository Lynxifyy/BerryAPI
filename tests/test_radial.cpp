// Host tests for the client-side radial menu (Phase 6f).
//
// These drive the real menu: item authoring, the pure sector geometry, the
// open/close animation, the hover/commit interaction on the render thread, and
// the draw path through a real Renderer into a capturing seam.
#include "berry/radial.h"
#include "berry/render.h"
#include "berry/event.h"
#include "shim_level.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static bool Near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

//----------------------------------------------------------------------------
// Authoring + validation
//----------------------------------------------------------------------------

static void TestItemsAndValidation() {
  RadialMenu m;
  CHECK(m.itemCount() == 0);

  CHECK(m.AddItem("Teleport"));
  CHECK(m.AddItem("Friends"));
  CHECK(m.AddItem("Settings"));
  CHECK(m.itemCount() == 3);
  CHECK(m.itemLabel(0) == "Teleport");
  CHECK(m.itemLabel(2) == "Settings");
  CHECK(m.itemLabel(3) == "");          // out of range
  CHECK(m.itemEnabled(0));

  // A malformed label is refused before it can enter the menu.
  CHECK(!m.AddItem(""));
  CHECK(!m.AddItem(std::string(RadialMenu::kMaxLabelLen + 1, 'x')));
  CHECK(!m.AddItem("bad\nlabel"));
  CHECK(m.itemCount() == 3);

  // The item cap holds.
  for (size_t i = m.itemCount(); i < RadialMenu::kMaxItems; ++i) {
    CHECK(m.AddItem("item"));
  }
  CHECK(m.itemCount() == RadialMenu::kMaxItems);
  CHECK(!m.AddItem("overflow"));
  CHECK(m.itemCount() == RadialMenu::kMaxItems);

  // Enable/disable by index, with the out-of-range case failing closed.
  CHECK(m.SetItemEnabled(1, false));
  CHECK(!m.itemEnabled(1));
  CHECK(!m.SetItemEnabled(999, false));

  CHECK(m.RemoveItem(0));
  CHECK(m.itemCount() == RadialMenu::kMaxItems - 1);
  CHECK(!m.RemoveItem(999));

  m.Clear();
  CHECK(m.itemCount() == 0);
  CHECK(!m.open());
}

//----------------------------------------------------------------------------
// Pure sector geometry
//----------------------------------------------------------------------------

static void TestSectorGeometry() {
  const Vec2 c(100, 100);

  // Four sectors. Sector 0 is centred straight up, then they run clockwise.
  // Straight up is (0, -1) from the centre.
  CHECK(RadialMenu::SectorFor(Vec2(100, 50), c, 20.0f, 80.0f, 4) == 0);
  // Right (clockwise from up) is sector 1.
  CHECK(RadialMenu::SectorFor(Vec2(150, 100), c, 20.0f, 80.0f, 4) == 1);
  // Down is sector 2.
  CHECK(RadialMenu::SectorFor(Vec2(100, 150), c, 20.0f, 80.0f, 4) == 2);
  // Left is sector 3.
  CHECK(RadialMenu::SectorFor(Vec2(50, 100), c, 20.0f, 80.0f, 4) == 3);

  // Inside the dead zone is no sector.
  CHECK(RadialMenu::SectorFor(Vec2(105, 105), c, 20.0f, 80.0f, 4) == -1);
  // Outside the outer ring is no sector.
  CHECK(RadialMenu::SectorFor(Vec2(100, 300), c, 20.0f, 80.0f, 4) == -1);
  // Zero sectors is no sector.
  CHECK(RadialMenu::SectorFor(Vec2(100, 50), c, 20.0f, 80.0f, 0) == -1);
  // A non-finite ring fails closed.
  CHECK(RadialMenu::SectorFor(Vec2(100, 50), c, 0.0f, std::nanf(""), 4) == -1);

  // Every sector boundary maps to a valid index, never off the end.
  for (int deg = 0; deg < 360; ++deg) {
    const f32 a = (f32)deg * 3.14159265f / 180.0f;
    const Vec2 p(c.x + std::cos(a) * 50.0f, c.y + std::sin(a) * 50.0f);
    const i32 s = RadialMenu::SectorFor(p, c, 20.0f, 80.0f, 6);
    CHECK(s >= 0 && s < 6);
  }
}

//----------------------------------------------------------------------------
// Open / close animation
//----------------------------------------------------------------------------

static void TestOpenCloseAnimation() {
  RadialMenu m;
  m.AddItem("A");
  m.AddItem("B");

  // An empty menu cannot open.
  RadialMenu empty;
  CHECK(!empty.Open());

  CHECK(m.Open());
  CHECK(m.open());
  CHECK(!m.Open());                 // already open
  CHECK(Near(m.state().progress, 0.0f));

  const Vec2 c(100, 100);
  const Vec2 far(100, 40);
  // Step halfway through the open animation: progress rises toward 1.
  m.Update(0.07f, c, far, true);
  CHECK(m.state().progress > 0.0f && m.state().progress < 1.0f);
  // Finish opening.
  m.Update(0.2f, c, far, true);
  CHECK(Near(m.state().progress, 1.0f));

  // Close and watch it fall back to 0.
  m.Close();
  CHECK(!m.open());
  m.Update(0.5f, c, far, false);
  CHECK(Near(m.state().progress, 0.0f));
}

//----------------------------------------------------------------------------
// Hover + commit interaction
//----------------------------------------------------------------------------

static void TestHoverAndCommit() {
  RadialMenu m;
  m.AddItem("Up");
  m.AddItem("Right");
  m.AddItem("Down");
  m.AddItem("Left");
  CHECK(m.Open());

  const Vec2 c(100, 100);
  // Fully open so the sectors exist.
  m.Update(1.0f, c, Vec2(100, 40), true);
  CHECK(Near(m.state().progress, 1.0f));

  // Hovering straight up selects sector 0 and holds it while the button is down.
  m.Update(0.016f, c, Vec2(100, 40), true);
  CHECK(m.state().hovered == 0);
  CHECK(m.state().selected == -1);   // nothing committed while held

  // Releasing over sector 0 commits it exactly once.
  m.Update(0.016f, c, Vec2(100, 40), false);
  CHECK(m.state().selected == 0);
  // The next frame with the button still up commits nothing new.
  m.Update(0.016f, c, Vec2(100, 40), false);
  CHECK(m.state().selected == -1);

  // A hover over a disabled sector is not selectable.
  CHECK(m.SetItemEnabled(1, false));
  m.Update(0.016f, c, Vec2(160, 100), true);
  CHECK(m.state().hovered == -1);
  m.Update(0.016f, c, Vec2(160, 100), false);
  CHECK(m.state().selected == -1);

  // An enabled sector still works.
  m.Update(0.016f, c, Vec2(160, 100), true);
  CHECK(m.SetItemEnabled(1, true));
  m.Update(0.016f, c, Vec2(160, 100), true);
  CHECK(m.state().hovered == 1);
  m.Update(0.016f, c, Vec2(160, 100), false);
  CHECK(m.state().selected == 1);
}

//----------------------------------------------------------------------------
// The commit publishes the documented event
//----------------------------------------------------------------------------

static void TestCommitPublishesEvent() {
  RadialMenu m;
  m.AddItem("Alpha");
  m.AddItem("Beta");
  CHECK(m.Open());

  std::string got;
  EventBus::ListenerId id = EventBus::Instance().On(
      "radialSelected", [&got](const EventContext& e) {
        // The bus convention: `data` points at a `const char*`.
        if (e.data) got = *(const char* const*)e.data;
        return EventResult::Continue;
      });

  const Vec2 c(100, 100);
  m.Update(1.0f, c, Vec2(100, 40), true);
  m.Update(0.016f, c, Vec2(100, 40), true);
  CHECK(got.empty());
  m.Update(0.016f, c, Vec2(100, 40), false);   // release over sector 0
  CHECK(got == "Alpha");

  // Hovering sector 1 and releasing commits Beta.
  m.Update(0.016f, c, Vec2(160, 100), true);
  m.Update(0.016f, c, Vec2(160, 100), false);
  CHECK(got == "Beta");

  EventBus::Instance().Off("radialSelected", id);
}

//----------------------------------------------------------------------------
// Draw through a real Renderer
//----------------------------------------------------------------------------

static void TestDrawPath() {
  Renderer r;
  Berry::Test::CaptureRenderSeam seam;
  r.BindSeam(&seam);

  RadialMenu m;
  m.AddItem("One");
  m.AddItem("Two");
  m.AddItem("Three");

  const Vec2 c(200, 200);

  // A closed menu draws nothing.
  r.BeginFrame(Vec2(400, 400));
  m.Draw(r, c);
  r.EndFrame();
  CHECK(seam.lines == 0 && seam.texts == 0 && seam.circles == 0);

  // Open it and draw: the hub circle, the sector spokes (lines) and the labels.
  CHECK(m.Open());
  m.Update(1.0f, c, Vec2(200, 100), true);
  r.BeginFrame(Vec2(400, 400));
  m.Draw(r, c);
  r.EndFrame();
  CHECK(seam.circles >= 1);         // the centre hub
  CHECK(seam.lines > 0);            // the sector spokes
  CHECK(seam.texts == 3);           // one label per item
  bool sawOne = false, sawTwo = false, sawThree = false;
  for (const auto& t : seam.textContents) {
    if (t == "One") sawOne = true;
    if (t == "Two") sawTwo = true;
    if (t == "Three") sawThree = true;
  }
  CHECK(sawOne && sawTwo && sawThree);
}

//----------------------------------------------------------------------------
// Configuration validation
//----------------------------------------------------------------------------

static void TestConfigValidation() {
  RadialMenu m;
  const RadialConfig def = m.config();
  CHECK(Near(def.radius, 120.0f));

  CHECK(m.SetRadius(200.0f));
  CHECK(Near(m.config().radius, 200.0f));
  CHECK(!m.SetRadius(0.0f));
  CHECK(!m.SetRadius(-5.0f));
  CHECK(!m.SetRadius(std::nanf("")));
  CHECK(!m.SetRadius(m.config().innerRadius));   // must exceed the inner radius
  CHECK(Near(m.config().radius, 200.0f));        // unchanged after a refusal

  CHECK(m.SetInnerRadius(60.0f));
  CHECK(Near(m.config().innerRadius, 60.0f));
  CHECK(!m.SetInnerRadius(-1.0f));
  CHECK(!m.SetInnerRadius(m.config().radius));   // must be under the outer radius

  CHECK(m.SetOpenMs(250.0f));
  CHECK(!m.SetOpenMs(0.0f));
  CHECK(!m.SetOpenMs(-1.0f));
}

int main() {
  TestItemsAndValidation();
  TestSectorGeometry();
  TestOpenCloseAnimation();
  TestHoverAndCommit();
  TestCommitPublishesEvent();
  TestDrawPath();
  TestConfigValidation();
  if (g_fail == 0) printf("test_radial: ALL OK\n");
  return g_fail;
}
