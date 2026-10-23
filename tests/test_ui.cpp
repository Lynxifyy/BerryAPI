// Host tests for the immediate-mode UI layer.
//
// The layer is deliberately split into a build half (worker) and a present half
// (render thread), so the interaction model is testable with no renderer at all:
// build a frame, run the present half's hit-test against a synthetic pointer and
// click state, then read the results back through the next build. These tests
// drive exactly that loop, plus the drawing path through a capture seam.
//
// The one rule every test follows: a present-half call publishes results, and the
// *next* build reads them. That one-frame lag is the layer's contract (a click is
// acted on the frame after the press), so the tests assert it rather than paper
// over it.
//
// Covers:
//   * ids: stable across frames, distinct per window and label, never 0.
//   * button: press then release inside clicks exactly once; release outside
//     does not.
//   * checkbox: a click toggles and reports the new value.
//   * slider: a drag maps the pointer to the range; a degenerate range is inert.
//   * input text: focus, typing, backspace (UTF-8 aware) and enter-to-commit.
//   * collapsing header and tabs: the state round-trips through the result.
//   * window drag, and the collapse toggle that must not fire after a drag.
//   * clipping: a widget inside a window carries the window's clip index.
//   * layout: stacked widgets advance; SameLine keeps one row.
//   * themes: Dark and Glass resolve every role; a swap restyles the draw.
//   * drawing: a built frame emits the expected primitive kinds.
#include "berry/ui.h"
#include "berry/input.h"
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

namespace {

/// Runs the worker half for one frame.
template <typename F>
static void BuildFrame(UiLayer& ui, F&& build) {
  ui.BeginFrame();
  build();
  ui.EndFrame();
}

/// A one-frame pointer with no click.
UiLayer::Input Hover(const Vec2& p) {
  UiLayer::Input in;
  in.pointer = p;
  return in;
}

/// A one-frame pointer with the mouse held.
UiLayer::Input Pressed(const Vec2& p) {
  UiLayer::Input in;
  in.pointer = p;
  in.mouseDown = true;
  return in;
}

/// The centre of a widget.
Vec2 Centre(const UiWidget& w) {
  return Vec2((w.min.x + w.max.x) * 0.5f, (w.min.y + w.max.y) * 0.5f);
}

/// Finds the first widget of a kind in the last built frame.
bool FindKind(UiLayer& ui, UiKind kind, UiWidget* out) {
  for (const auto& w : ui.Widgets()) {
    if (w.kind == kind) {
      if (out) *out = w;
      return true;
    }
  }
  return false;
}

/// A window id from a title, matching the layer's own derivation.
UiId WindowId(const std::string& title) { return UiLayer::HashId("window:" + title); }

}  // namespace

//----------------------------------------------------------------------------
// Ids
//----------------------------------------------------------------------------

static void TestIds() {
  UiLayer ui;
  const UiId win = WindowId("W");
  const UiId buttonId = UiLayer::HashId(std::to_string(win) + "|button:Go");

  UiId firstId = 0, laterId = 0;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    ui.Button("Go");
    ui.EndWindow();
  });
  for (const auto& w : ui.Widgets()) {
    if (w.kind == UiKind::Button) firstId = w.id;
  }
  CHECK(firstId == buttonId);
  CHECK(firstId != 0);

  // Stable across frames.
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    ui.Button("Go");
    ui.EndWindow();
  });
  for (const auto& w : ui.Widgets()) {
    if (w.kind == UiKind::Button) laterId = w.id;
  }
  CHECK(firstId == laterId);

  // A different window with the same label is a different id.
  BuildFrame(ui, [&] {
    ui.BeginWindow("Other", Vec2(0, 0), Vec2(200, 200));
    ui.Button("Go");
    ui.EndWindow();
  });
  UiId otherId = 0;
  for (const auto& w : ui.Widgets()) {
    if (w.kind == UiKind::Button) otherId = w.id;
  }
  CHECK(otherId != 0);
  CHECK(otherId != firstId);
}

//----------------------------------------------------------------------------
// Button
//----------------------------------------------------------------------------

static void TestButtonClick() {
  UiLayer ui;
  const Vec2 c(60, 30);

  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    ui.Button("Go");
    ui.EndWindow();
  });
  ui.Update(Hover(c));  // no click
  UiResult r0;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    r0 = ui.Button("Go");
    ui.EndWindow();
  });
  CHECK(!r0.clicked);

  // Press (not a click on its own).
  ui.Update(Pressed(c));
  UiResult r1;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    r1 = ui.Button("Go");
    ui.EndWindow();
  });
  CHECK(!r1.clicked);

  // Release inside -> clicked.
  ui.Update(Hover(c));
  UiResult r2;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    r2 = ui.Button("Go");
    ui.EndWindow();
  });
  CHECK(r2.clicked);

  // And it does not stick.
  ui.Update(Hover(c));
  UiResult r3;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    r3 = ui.Button("Go");
    ui.EndWindow();
  });
  CHECK(!r3.clicked);
}

static void TestButtonReleaseOutside() {
  UiLayer ui;
  const Vec2 c(60, 30);

  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    ui.Button("Go");
    ui.EndWindow();
  });
  ui.Update(Pressed(c));             // press on the button
  ui.Update(Hover(Vec2(500, 500)));  // release far away

  UiResult r;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    r = ui.Button("Go");
    ui.EndWindow();
  });
  CHECK(!r.clicked);
}

//----------------------------------------------------------------------------
// Checkbox
//----------------------------------------------------------------------------

static void TestCheckbox() {
  UiLayer ui;
  const Vec2 c(40, 30);

  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    ui.Checkbox("On", false);
    ui.EndWindow();
  });
  ui.Update(Pressed(c));
  ui.Update(Hover(c));  // release inside -> toggles

  UiResult r;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    r = ui.Checkbox("On", false);
    ui.EndWindow();
  });
  CHECK(r.toggled);
  CHECK(Near(r.value, 1.0f));  // false -> true
}

//----------------------------------------------------------------------------
// Slider
//----------------------------------------------------------------------------

static void TestSliderDrag() {
  UiLayer ui;
  UiWidget slider;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    ui.SliderFloat("Reach", 3.0f, 0.0f, 10.0f);
    ui.EndWindow();
  });
  CHECK(FindKind(ui, UiKind::Slider, &slider));

  const Vec2 left(slider.min.x + 1.0f, (slider.min.y + slider.max.y) * 0.5f);
  const Vec2 mid((slider.min.x + slider.max.x) * 0.5f, left.y);

  ui.Update(Pressed(left));  // press -> the slider becomes active
  ui.Update(Pressed(mid));   // drag -> publishes the new value

  UiResult r;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    r = ui.SliderFloat("Reach", 3.0f, 0.0f, 10.0f);
    ui.EndWindow();
  });
  CHECK(r.changed);
  CHECK(r.value > 4.0f && r.value < 6.0f);
}

static void TestSliderDegenerateRange() {
  UiLayer ui;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    // hi == lo: the slider must refuse to drag rather than divide by zero.
    const UiResult r = ui.SliderFloat("Bad", 5.0f, 5.0f, 5.0f);
    CHECK(!r.changed);
    CHECK(Near(r.value, 5.0f));
    ui.EndWindow();
  });
}

//----------------------------------------------------------------------------
// Input text
//----------------------------------------------------------------------------

static void TestInputText() {
  UiLayer ui;
  UiWidget field;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(220, 200));
    ui.InputText("Name", "");
    ui.EndWindow();
  });
  CHECK(FindKind(ui, UiKind::InputText, &field));
  const Vec2 c = Centre(field);

  ui.Update(Pressed(c));  // click to focus
  ui.Update(Hover(c));

  // Type "ab".
  UiLayer::Input typed = Hover(c);
  typed.typed = "ab";
  ui.Update(typed);
  UiResult r;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(220, 200));
    r = ui.InputText("Name", "");
    ui.EndWindow();
  });
  CHECK(r.changed);
  CHECK(r.text == "ab");

  // Backspace -> "a".
  UiLayer::Input bs = Hover(c);
  bs.backspace = true;
  ui.Update(bs);
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(220, 200));
    r = ui.InputText("Name", "ab");
    ui.EndWindow();
  });
  CHECK(r.changed);
  CHECK(r.text == "a");

  // Type "c" -> "ac".
  UiLayer::Input more = Hover(c);
  more.typed = "c";
  ui.Update(more);
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(220, 200));
    r = ui.InputText("Name", "a");
    ui.EndWindow();
  });
  CHECK(r.text == "ac");

  // Enter commits and drops focus.
  UiLayer::Input enter = Hover(c);
  enter.enter = true;
  ui.Update(enter);
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(220, 200));
    r = ui.InputText("Name", "ac");
    ui.EndWindow();
  });

  // A later keystroke is not applied: the field lost focus.
  UiLayer::Input after = Hover(c);
  after.typed = "z";
  ui.Update(after);
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(220, 200));
    r = ui.InputText("Name", "ac");
    ui.EndWindow();
  });
  CHECK(!r.changed);
}

static void TestInputTextBackspaceUtf8() {
  UiLayer ui;
  UiWidget field;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(220, 200));
    ui.InputText("Name", "");
    ui.EndWindow();
  });
  CHECK(FindKind(ui, UiKind::InputText, &field));
  const Vec2 c = Centre(field);

  ui.Update(Pressed(c));
  ui.Update(Hover(c));

  UiLayer::Input typed = Hover(c);
  typed.typed = "\xC3\xA9";  // "é" = 2 bytes
  ui.Update(typed);
  UiResult r;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(220, 200));
    r = ui.InputText("Name", "");
    ui.EndWindow();
  });
  CHECK(r.text == "\xC3\xA9");

  UiLayer::Input bs = Hover(c);
  bs.backspace = true;
  ui.Update(bs);
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(220, 200));
    r = ui.InputText("Name", "\xC3\xA9");
    ui.EndWindow();
  });
  CHECK(r.text.empty());  // the whole character went, not half of it
}

//----------------------------------------------------------------------------
// Header / tabs
//----------------------------------------------------------------------------

static void TestCollapsingHeader() {
  UiLayer ui;
  UiWidget header;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    ui.CollapsingHeader("Section", true);
    ui.EndWindow();
  });
  for (const auto& w : ui.Widgets()) {
    if (w.kind == UiKind::Header && !w.windowBar) header = w;
  }
  const Vec2 c = Centre(header);

  ui.Update(Pressed(c));
  ui.Update(Hover(c));  // release in place -> toggle

  bool open = true;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    open = ui.CollapsingHeader("Section", true);
    ui.EndWindow();
  });
  CHECK(!open);  // true -> false
}

static void TestTabs() {
  UiLayer ui;
  UiWidget tab0, tab1;
  int seen = 0;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(300, 200));
    ui.BeginTabBar("bar");
    ui.BeginTabItem("One");
    ui.EndTabItem();
    ui.BeginTabItem("Two");
    ui.EndTabItem();
    ui.EndTabBar();
    ui.EndWindow();
  });
  for (const auto& w : ui.Widgets()) {
    if (w.kind == UiKind::Tab) {
      if (seen == 0) tab0 = w;
      if (seen == 1) tab1 = w;
      ++seen;
    }
  }
  CHECK(seen == 2);
  CHECK(tab0.toggleOn);                 // first tab selected
  CHECK(!tab1.toggleOn);
  CHECK(Near(tab0.min.y, tab1.min.y));  // tabs share a row
  CHECK(tab1.min.x > tab0.min.x);       // and flow left to right

  const Vec2 c1 = Centre(tab1);
  ui.Update(Pressed(c1));
  ui.Update(Hover(c1));  // release inside tab 1 -> select it

  bool twoSelected = false;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(300, 200));
    ui.BeginTabBar("bar");
    ui.BeginTabItem("One");
    ui.EndTabItem();
    twoSelected = ui.BeginTabItem("Two");
    ui.EndTabItem();
    ui.EndTabBar();
    ui.EndWindow();
  });
  CHECK(twoSelected);
}

//----------------------------------------------------------------------------
// Window drag / collapse
//----------------------------------------------------------------------------

static void TestWindowDrag() {
  UiLayer ui;
  UiWidget bar;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(10, 20), Vec2(200, 150));
    ui.EndWindow();
  });
  for (const auto& w : ui.Widgets()) {
    if (w.windowBar) bar = w;
  }
  const Vec2 start = Centre(bar);
  const Vec2 moved(start.x + 50.0f, start.y + 30.0f);

  ui.Update(Pressed(start));
  ui.Update(Pressed(moved));  // drag past the toggle threshold
  CHECK(ui.anyWindowDragging());

  const auto ws = ui.Windows();
  CHECK(!ws.empty());
  CHECK(Near(ws[0].dragOffset.x, 50.0f));
  CHECK(Near(ws[0].dragOffset.y, 30.0f));

  // Release: no collapse toggle, because the pointer moved.
  ui.Update(Hover(moved));
  BuildFrame(ui, [&] {
    // The drag offset persists, so the window is built at its moved position.
    ui.BeginWindow("W", Vec2(10, 20), Vec2(200, 150));
    ui.EndWindow();
  });
  UiWidget bar2;
  for (const auto& w : ui.Widgets()) {
    if (w.windowBar) bar2 = w;
  }
  CHECK(Near(bar2.min.x, 60.0f));  // 10 + 50
  CHECK(Near(bar2.min.y, 50.0f));  // 20 + 30
  CHECK(!ui.anyWindowDragging());
}

static void TestWindowCollapse() {
  UiLayer ui;
  UiWidget bar;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(10, 20), Vec2(200, 150));
    ui.EndWindow();
  });
  for (const auto& w : ui.Widgets()) {
    if (w.windowBar) bar = w;
  }
  const Vec2 c = Centre(bar);

  ui.Update(Pressed(c));
  ui.Update(Hover(c));  // release in place -> collapse

  // The next build reports collapsed: BeginWindow returns false and no content
  // is built, but the window still draws its title bar.
  bool open = true;
  BuildFrame(ui, [&] {
    open = ui.BeginWindow("W", Vec2(10, 20), Vec2(200, 150));
    ui.Button("Hidden");
    ui.EndWindow();
  });
  CHECK(!open);
  bool sawBar = false, sawButton = false;
  for (const auto& w : ui.Widgets()) {
    if (w.windowBar) sawBar = true;
    if (w.kind == UiKind::Button) sawButton = true;
  }
  CHECK(sawBar);
  CHECK(!sawButton);

  // And the state persists: a further build is still collapsed.
  bool open2 = true;
  BuildFrame(ui, [&] {
    open2 = ui.BeginWindow("W", Vec2(10, 20), Vec2(200, 150));
    ui.EndWindow();
  });
  CHECK(!open2);

  // Clicking the title bar again expands it.
  UiWidget bar2;
  for (const auto& w : ui.Widgets()) {
    if (w.windowBar) bar2 = w;
  }
  const Vec2 c2 = Centre(bar2);
  ui.Update(Pressed(c2));
  ui.Update(Hover(c2));
  bool open3 = false;
  BuildFrame(ui, [&] {
    open3 = ui.BeginWindow("W", Vec2(10, 20), Vec2(200, 150));
    ui.EndWindow();
  });
  CHECK(open3);
}

//----------------------------------------------------------------------------
// Clipping / layout
//----------------------------------------------------------------------------

static void TestClipAndLayout() {
  UiLayer ui;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    ui.Button("A");
    ui.Button("B");
    ui.EndWindow();
  });
  int buttons = 0;
  f32 firstY = 0, secondY = 0;
  for (const auto& w : ui.Widgets()) {
    if (w.kind == UiKind::Button) {
      if (buttons == 0) firstY = w.min.y;
      if (buttons == 1) secondY = w.min.y;
      ++buttons;
      // Every content widget carries the window's clip.
      CHECK(w.clipIndex >= 0);
    }
  }
  CHECK(buttons == 2);
  CHECK(secondY > firstY);  // the second button is below the first
}

static void TestSameLine() {
  UiLayer ui;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(300, 200));
    ui.Button("A");
    ui.SameLine();
    ui.Button("B");
    ui.EndWindow();
  });
  UiWidget a, b;
  int n = 0;
  for (const auto& w : ui.Widgets()) {
    if (w.kind == UiKind::Button) {
      if (n == 0) a = w;
      if (n == 1) b = w;
      ++n;
    }
  }
  CHECK(n == 2);
  CHECK(Near(a.min.y, b.min.y));  // same row
  CHECK(b.min.x > a.min.x);       // B is to the right of A
}

//----------------------------------------------------------------------------
// Themes
//----------------------------------------------------------------------------

static void TestThemes() {
  const UiTheme dark = UiTheme::Dark();
  const UiTheme glass = UiTheme::Glass();
  // Every role resolves to something; the two themes differ.
  CHECK(dark.Role(UiColorRole::Accent).b != 0);
  CHECK(glass.Role(UiColorRole::Accent).b == 255);
  CHECK(glass.windowBg.a < dark.windowBg.a);  // glass is translucent
  CHECK(glass.rounding > dark.rounding);      // glass rounds more

  UiLayer ui;
  ui.SetTheme(glass);
  CHECK(ui.theme().rounding == glass.rounding);
}

//----------------------------------------------------------------------------
// Drawing
//----------------------------------------------------------------------------

static void TestDrawing() {
  UiLayer ui;
  auto capture = std::make_shared<Berry::Test::CaptureRenderSeam>();
  Renderer r;
  r.BindSeam(capture.get());

  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    ui.Text("hello");
    ui.Button("Go");
    ui.Separator();
    ui.EndWindow();
  });

  r.BeginFrame(Vec2(640, 480));
  const size_t drawn = ui.Present(r, Hover(Vec2(-100, -100)));
  r.EndFrame();

  CHECK(drawn > 0);
  CHECK(capture->texts >= 2);  // the label and the button caption
  CHECK(capture->rects >= 1);  // the window background
  CHECK(capture->lines >= 1);  // the separator
}

static void TestDrawingHoverRecolor() {
  UiLayer ui;
  auto capture = std::make_shared<Berry::Test::CaptureRenderSeam>();
  Renderer r;
  r.BindSeam(capture.get());

  UiWidget button;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    ui.Button("Go");
    ui.EndWindow();
  });
  CHECK(FindKind(ui, UiKind::Button, &button));
  const Vec2 c = Centre(button);

  r.BeginFrame(Vec2(640, 480));
  ui.Present(r, Hover(c));
  r.EndFrame();

  // The next build reads the hovered result.
  UiResult res;
  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    res = ui.Button("Go");
    ui.EndWindow();
  });
  CHECK(res.hovered);
}

static void TestGlassDrawsRounded() {
  UiLayer ui;
  ui.SetTheme(UiTheme::Glass());
  auto capture = std::make_shared<Berry::Test::CaptureRenderSeam>();
  Renderer r;
  r.BindSeam(capture.get());

  BuildFrame(ui, [&] {
    ui.BeginWindow("W", Vec2(0, 0), Vec2(200, 200));
    ui.EndWindow();
  });
  r.BeginFrame(Vec2(640, 480));
  ui.Present(r, Hover(Vec2(-100, -100)));
  r.EndFrame();
  // Rounded corners emit extra rect bands and chord lines beyond a flat theme.
  CHECK(capture->rects >= 3);
  CHECK(capture->lines >= 8);
}

//----------------------------------------------------------------------------
// Input observation extensions
//----------------------------------------------------------------------------

static void TestInputPointerAndText() {
  InputObserver in;
  in.OnPointerMove(120.0f, 80.0f);
  CHECK(Near(in.state().pointer.x, 120.0f));
  CHECK(Near(in.state().pointer.y, 80.0f));

  // A non-finite coordinate is refused.
  in.OnPointerMove(std::nanf(""), 10.0f);
  CHECK(Near(in.state().pointer.x, 120.0f));

  in.OnWheel(2.0f);
  in.OnWheel(1.0f);
  CHECK(Near(in.ConsumeWheel(), 3.0f));
  CHECK(Near(in.ConsumeWheel(), 0.0f));  // taken once

  // Control bytes are stripped from typed text.
  in.OnTextInput("a\tb\nc");
  CHECK(in.ConsumeText() == "abc");
  CHECK(in.ConsumeText().empty());

  // A one-shot key is seen exactly once.
  in.OnKey("backspace", true);
  in.OnKey("backspace", false);
  CHECK(in.ConsumeKeyPress("backspace"));
  CHECK(!in.ConsumeKeyPress("backspace"));
  CHECK(!in.ConsumeKeyPress("enter"));

  // Reset clears everything.
  in.OnPointerMove(1.0f, 1.0f);
  in.OnWheel(5.0f);
  in.OnTextInput("x");
  in.OnKey("enter", true);
  in.Reset();
  CHECK(Near(in.state().pointer.x, 0.0f));
  CHECK(Near(in.ConsumeWheel(), 0.0f));
  CHECK(in.ConsumeText().empty());
  CHECK(!in.ConsumeKeyPress("enter"));
}

//----------------------------------------------------------------------------

int main() {
  TestIds();
  TestButtonClick();
  TestButtonReleaseOutside();
  TestCheckbox();
  TestSliderDrag();
  TestSliderDegenerateRange();
  TestInputText();
  TestInputTextBackspaceUtf8();
  TestCollapsingHeader();
  TestTabs();
  TestWindowDrag();
  TestWindowCollapse();
  TestClipAndLayout();
  TestSameLine();
  TestThemes();
  TestDrawing();
  TestDrawingHoverRecolor();
  TestGlassDrawsRounded();
  TestInputPointerAndText();

  if (g_fail) {
    printf("test_ui: %d failure(s)\n", g_fail);
    return 1;
  }
  printf("test_ui: all OK\n");
  return 0;
}
