// Host tests for the client-side touch, gesture and on-screen button service.
//
// These drive the real service: the raw pointer feed, every gesture the
// recogniser produces (tap, double tap, long press, drag, swipe, pinch,
// rotate), the button hit-test with its policy-gated intent, and the draw path
// through a real Renderer into a capturing seam. Recognition is deterministic
// (a pure function of the feed and the caller's timestamps), so no device and
// no wall clock are needed.
#include "berry/touch.h"
#include "berry/render.h"
#include "berry/event.h"
#include "berry/intent.h"
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

// Captures the last gesture payload published on the touch channel.
struct GestureCapture {
  std::vector<std::string> payloads;
  EventBus::ListenerId id = 0;
  void Install() {
    id = EventBus::Instance().On("touchGesture", [this](const EventContext& e) {
      if (e.data) {
        const char* p = *(const char**)e.data;
        if (p) payloads.push_back(p);
      }
      return EventResult::Continue;
    });
  }
  ~GestureCapture() {
    if (id) EventBus::Instance().Off("touchGesture", id);
  }
};

//----------------------------------------------------------------------------
// Pure helpers + validation
//----------------------------------------------------------------------------

static void TestSwipeGeometry() {
  // The dominant axis wins; a short travel is None. Screen y grows downward.
  CHECK(TouchService::SwipeDirectionOf(Vec2(60, 10), 48) == SwipeDir::Right);
  CHECK(TouchService::SwipeDirectionOf(Vec2(-60, 10), 48) == SwipeDir::Left);
  CHECK(TouchService::SwipeDirectionOf(Vec2(5, -60), 48) == SwipeDir::Up);
  CHECK(TouchService::SwipeDirectionOf(Vec2(5, 60), 48) == SwipeDir::Down);
  CHECK(TouchService::SwipeDirectionOf(Vec2(10, 10), 48) == SwipeDir::None);
  CHECK(TouchService::SwipeDirectionOf(Vec2(60, 10), -1) == SwipeDir::None);
  CHECK(TouchService::SwipeDirectionOf(Vec2(std::nanf(""), 10), 48) == SwipeDir::None);

  // Names round-trip.
  CHECK(std::string(GestureKindName(GestureKind::DoubleTap)) == "double_tap");
  CHECK(GestureKindFromName("long_press") == GestureKind::LongPress);
  CHECK(GestureKindFromName("nope") == GestureKind::None);
  CHECK(GestureBit(GestureKind::Swipe) == (u32)GestureMask::Swipe);
  CHECK(GestureBit(GestureKind::None) == 0);
}

static void TestConfigValidation() {
  TouchService t;
  const TouchConfig d = t.config();
  CHECK(Near(d.tapMaxMove, 14.0f));
  CHECK(Near(d.longPressMs, 480.0f));

  CHECK(t.SetTapMaxMove(20.0f));
  CHECK(Near(t.config().tapMaxMove, 20.0f));
  CHECK(!t.SetTapMaxMove(0.0f));
  CHECK(!t.SetTapMaxMove(-1.0f));
  CHECK(!t.SetTapMaxMove(std::nanf("")));
  CHECK(Near(t.config().tapMaxMove, 20.0f));   // unchanged after a refusal

  CHECK(t.SetTapMaxMs(300.0f));
  CHECK(!t.SetTapMaxMs(0.0f));
  CHECK(t.SetLongPressMs(600.0f));
  CHECK(!t.SetLongPressMs(-5.0f));
  CHECK(t.SetDoubleTapMaxMs(400.0f));
  CHECK(!t.SetDoubleTapMaxMs(0.0f));
  CHECK(t.SetSwipeMinDist(64.0f));
  CHECK(!t.SetSwipeMinDist(0.0f));
  CHECK(t.SetSwipeMaxMs(600.0f));
  CHECK(!t.SetSwipeMaxMs(0.0f));
  CHECK(t.SetPinchMinScale(0.05f));
  CHECK(!t.SetPinchMinScale(0.0f));
  CHECK(!t.SetPinchMinScale(1.5f));   // a scale is a fraction <= 1
}

static void TestGestureToggle() {
  TouchService t;
  CHECK(t.gestureEnabled(GestureKind::Tap));
  CHECK(t.SetGestureEnabled(GestureKind::Tap, false));
  CHECK(!t.gestureEnabled(GestureKind::Tap));
  CHECK((t.gestureMask() & (u32)GestureMask::Tap) == 0);
  CHECK(!t.SetGestureEnabled(GestureKind::None, true));   // not a real gesture
  CHECK(t.SetGestureEnabled(GestureKind::Tap, true));
  CHECK(t.gestureEnabled(GestureKind::Tap));
}

//----------------------------------------------------------------------------
// Single-finger gestures
//----------------------------------------------------------------------------

static void TestTap() {
  TouchService t;
  GestureCapture cap;
  cap.Install();

  CHECK(t.OnPointerDown(1, Vec2(100, 100), 0));
  CHECK(t.state().pointers == 1);
  t.OnPointerMove(1, Vec2(102, 101), 40);      // tiny drift, still a tap
  t.OnPointerUp(1, Vec2(102, 101), 60);

  CHECK(t.lastGesture().kind == GestureKind::Tap);
  CHECK(cap.payloads.size() == 1);
  CHECK(cap.payloads[0].rfind("tap,", 0) == 0);
  CHECK(t.state().pointers == 0);
}

static void TestTapRejectedWhenMoved() {
  TouchService t;
  GestureCapture cap;
  cap.Install();
  // A press that moves far and holds long is neither a tap nor a swipe (slow).
  t.OnPointerDown(1, Vec2(0, 0), 0);
  t.OnPointerMove(1, Vec2(100, 0), 50);
  t.OnPointerUp(1, Vec2(100, 0), 900);         // slow release
  CHECK(t.lastGesture().kind == GestureKind::Drag);
  // No tap was emitted.
  for (const auto& p : cap.payloads) CHECK(p.rfind("tap,", 0) != 0);
}

static void TestDoubleTap() {
  TouchService t;
  GestureCapture cap;
  cap.Install();

  t.OnPointerDown(1, Vec2(50, 50), 0);
  t.OnPointerUp(1, Vec2(50, 50), 50);
  CHECK(t.lastGesture().kind == GestureKind::Tap);

  // A second tap within the window and near the first is a double tap.
  t.OnPointerDown(2, Vec2(52, 51), 200);
  t.OnPointerUp(2, Vec2(52, 51), 240);
  CHECK(t.lastGesture().kind == GestureKind::DoubleTap);

  // A third tap after the window is a fresh single tap.
  t.OnPointerDown(3, Vec2(50, 50), 900);
  t.OnPointerUp(3, Vec2(50, 50), 940);
  CHECK(t.lastGesture().kind == GestureKind::Tap);

  bool sawDouble = false;
  for (const auto& p : cap.payloads) if (p.rfind("double_tap,", 0) == 0) sawDouble = true;
  CHECK(sawDouble);
}

static void TestDoubleTapFarAwayIsSingle() {
  TouchService t;
  t.OnPointerDown(1, Vec2(0, 0), 0);
  t.OnPointerUp(1, Vec2(0, 0), 40);
  CHECK(t.lastGesture().kind == GestureKind::Tap);
  // A second tap far from the first does not form a double tap.
  t.OnPointerDown(2, Vec2(300, 300), 120);
  t.OnPointerUp(2, Vec2(300, 300), 160);
  CHECK(t.lastGesture().kind == GestureKind::Tap);
}

static void TestSwipe() {
  TouchService t;
  GestureCapture cap;
  cap.Install();
  t.OnPointerDown(1, Vec2(200, 200), 0);
  t.OnPointerMove(1, Vec2(140, 200), 60);
  t.OnPointerUp(1, Vec2(100, 200), 120);      // fast, far, leftwards
  CHECK(t.lastGesture().kind == GestureKind::Swipe);
  CHECK(t.lastGesture().dir == SwipeDir::Left);
  CHECK(Near(t.lastGesture().magnitude, 100.0f, 0.5f));
  CHECK(!cap.payloads.empty());
  CHECK(cap.payloads.back().rfind("swipe,left,", 0) == 0);
}

static void TestLongPress() {
  TouchService t;
  GestureCapture cap;
  cap.Install();
  t.OnPointerDown(1, Vec2(10, 10), 0);
  // Before the threshold: nothing fires.
  t.Update(100, nullptr);
  CHECK(t.lastGesture().kind == GestureKind::None);
  // Past the threshold: a long press fires once.
  t.Update(600, nullptr);
  CHECK(t.lastGesture().kind == GestureKind::LongPress);
  const size_t n = cap.payloads.size();
  // A later update does not re-fire it.
  t.Update(700, nullptr);
  CHECK(cap.payloads.size() == n);
}

static void TestDrag() {
  TouchService t;
  GestureCapture cap;
  cap.Install();
  t.OnPointerDown(1, Vec2(0, 0), 0);
  t.OnPointerMove(1, Vec2(50, 0), 30);
  t.OnPointerUp(1, Vec2(50, 0), 900);   // slow -> not a swipe
  CHECK(t.lastGesture().kind == GestureKind::Drag);
  bool sawDrag = false;
  for (const auto& p : cap.payloads) if (p.rfind("drag,", 0) == 0) sawDrag = true;
  CHECK(sawDrag);
}

//----------------------------------------------------------------------------
// Two-finger gestures
//----------------------------------------------------------------------------

static void TestPinch() {
  TouchService t;
  GestureCapture cap;
  cap.Install();
  t.OnPointerDown(1, Vec2(100, 100), 0);
  t.OnPointerDown(2, Vec2(200, 100), 10);   // 100px apart, baseline captured
  t.Update(20, nullptr);                    // no change -> no pinch yet
  // Widen the separation: a pinch out.
  t.OnPointerMove(2, Vec2(300, 100), 30);
  t.Update(40, nullptr);
  CHECK(t.lastGesture().kind == GestureKind::Pinch);
  CHECK(t.lastGesture().magnitude > 1.0f);  // scale > 1 = spread
  CHECK(t.lastGesture().pointers == 2);
  bool sawPinch = false;
  for (const auto& p : cap.payloads) if (p.rfind("pinch,", 0) == 0) sawPinch = true;
  CHECK(sawPinch);
}

static void TestRotate() {
  TouchService t;
  GestureCapture cap;
  cap.Install();
  t.SetGestureEnabled(GestureKind::Pinch, false);   // isolate rotate
  t.OnPointerDown(1, Vec2(0, 100), 0);
  t.OnPointerDown(2, Vec2(100, 100), 10);
  // Move the second finger so the pair's angle changes ~90 degrees.
  t.OnPointerMove(2, Vec2(0, 200), 30);
  t.Update(40, nullptr);
  CHECK(t.lastGesture().kind == GestureKind::Rotate);
  CHECK(std::fabs(t.lastGesture().magnitude) > 45.0f);
  CHECK(t.lastGesture().pointers == 2);
  bool sawRotate = false;
  for (const auto& p : cap.payloads) if (p.rfind("rotate,", 0) == 0) sawRotate = true;
  CHECK(sawRotate);
}

static void TestSecondFingerLiftEndsPinch() {
  TouchService t;
  t.OnPointerDown(1, Vec2(100, 100), 0);
  t.OnPointerDown(2, Vec2(200, 100), 10);
  t.OnPointerUp(2, Vec2(200, 100), 40);
  // A later move of the remaining finger must not produce a pinch.
  t.OnPointerMove(1, Vec2(50, 50), 50);
  t.Update(60, nullptr);
  CHECK(t.lastGesture().kind != GestureKind::Pinch);
  CHECK(t.state().pointers == 1);
}

static void TestPointerTableBound() {
  TouchService t;
  for (u32 i = 0; i < TouchService::kMaxPointers; ++i) {
    CHECK(t.OnPointerDown(i, Vec2((f32)i, 0), i));
  }
  CHECK(!t.OnPointerDown(99, Vec2(0, 0), 100));   // table full
  CHECK(t.state().pointers == TouchService::kMaxPointers);
  t.ClearPointers();
  CHECK(t.state().pointers == 0);
  // A repeated down for a live id does not add a second slot.
  CHECK(t.OnPointerDown(5, Vec2(1, 1), 0));
  CHECK(t.OnPointerDown(5, Vec2(2, 2), 1));
  CHECK(t.state().pointers == 1);
}

static void TestNonFiniteFeedRejected() {
  TouchService t;
  CHECK(!t.OnPointerDown(1, Vec2(std::nanf(""), 0), 0));
  CHECK(t.OnPointerDown(1, Vec2(0, 0), 0));
  t.OnPointerMove(1, Vec2(std::nanf(""), 0), 1);   // ignored
  CHECK(t.state().pointers == 1);
  CHECK(Near(t.pointers()[0].pos.x, 0.0f));
  t.OnPointerUp(1, Vec2(0, 0), 2);
  CHECK(t.lastGesture().kind == GestureKind::Tap);
}

//----------------------------------------------------------------------------
// On-screen buttons
//----------------------------------------------------------------------------

static void TestButtonAuthoring() {
  TouchService t;
  CHECK(t.buttonCount() == 0);
  CHECK(t.AddButton("Jump", Vec2(10, 10), Vec2(80, 60)));
  CHECK(t.AddButton("Sneak", Vec2(100, 10), Vec2(170, 60)));
  CHECK(t.buttonCount() == 2);
  CHECK(t.buttonLabel(0) == "Jump");
  CHECK(t.buttonLabel(5) == "");

  CHECK(!t.AddButton("", Vec2(0, 0), Vec2(10, 10)));           // empty label
  CHECK(!t.AddButton("X", Vec2(0, 0), Vec2(0, 10)));           // zero width
  CHECK(!t.AddButton("X", Vec2(10, 10), Vec2(0, 0)));          // inverted
  CHECK(!t.AddButton("X", Vec2(0, 0), Vec2(std::nanf(""), 10)));
  CHECK(!t.AddButton(std::string(TouchService::kMaxLabelLen + 1, 'x'),
                     Vec2(0, 0), Vec2(10, 10)));

  for (size_t i = t.buttonCount(); i < TouchService::kMaxButtons; ++i) {
    CHECK(t.AddButton("b", Vec2(0, 0), Vec2(10, 10)));
  }
  CHECK(t.buttonCount() == TouchService::kMaxButtons);
  CHECK(!t.AddButton("overflow", Vec2(0, 0), Vec2(10, 10)));

  CHECK(t.RemoveButton(0));
  CHECK(t.buttonCount() == TouchService::kMaxButtons - 1);
  CHECK(!t.RemoveButton(9999));
  t.ClearButtons();
  CHECK(t.buttonCount() == 0);
}

static void TestButtonPressAndToggle() {
  TouchService t;
  CHECK(t.AddButton("Jump", Vec2(100, 100), Vec2(200, 200)));

  // A press inside marks the button held and counts a press.
  t.OnPointerDown(1, Vec2(150, 150), 0);
  t.Update(10, nullptr);
  CHECK(t.buttonState(0).held);
  CHECK(t.buttonState(0).presses == 1);
  CHECK(t.buttonState(0).releases == 0);

  // A release inside toggles and counts a release.
  t.OnPointerUp(1, Vec2(150, 150), 20);
  t.Update(30, nullptr);
  CHECK(!t.buttonState(0).held);
  CHECK(t.buttonState(0).releases == 1);
  CHECK(t.buttonState(0).toggled);

  // A press outside does nothing.
  t.OnPointerDown(2, Vec2(0, 0), 40);
  t.Update(50, nullptr);
  CHECK(!t.buttonState(0).held);
  CHECK(t.buttonState(0).presses == 1);
  t.OnPointerUp(2, Vec2(0, 0), 60);
}

static void TestDisabledButtonNotPressed() {
  TouchService t;
  t.AddButton("Jump", Vec2(100, 100), Vec2(200, 200));
  CHECK(t.SetButtonEnabled(0, false));
  CHECK(!t.buttonEnabled(0));
  t.OnPointerDown(1, Vec2(150, 150), 0);
  t.Update(10, nullptr);
  CHECK(!t.buttonState(0).held);
  CHECK(t.buttonState(0).presses == 0);
  CHECK(!t.SetButtonEnabled(99, false));
}

static void TestButtonIntentPolicyGated() {
  TouchService t;
  t.AddButton("Zoom", Vec2(0, 0), Vec2(100, 100));
  CHECK(t.SetButtonIntent(0, "zoom"));    // a client-side intent
  CHECK(t.buttonIntent(0) == "zoom");
  CHECK(!t.SetButtonIntent(0, "bad intent"));   // not an identifier
  CHECK(!t.SetButtonIntent(99, "zoom"));

  // A policy that disables the camera group refuses the zoom intent.
  IntentPolicy policy;
  IntentValidator validator;
  ServerPolicyProfile strict;
  strict.name = "strict";
  strict.serverMatch = "*";
  strict.disabledGroups = IntentGroupMask(IntentGroup::Camera);
  validator.SetProfile(strict);

  t.OnPointerDown(1, Vec2(50, 50), 0);
  t.Update(10, &validator);
  CHECK(t.buttonState(0).held);
  CHECK(!t.buttonState(0).allowed);      // refused here
  t.OnPointerUp(1, Vec2(50, 50), 20);

  // With an open profile the same press is allowed.
  IntentValidator open;
  t.OnPointerDown(2, Vec2(50, 50), 100);
  t.Update(110, &open);
  CHECK(t.buttonState(0).held);
  CHECK(t.buttonState(0).allowed);
  t.OnPointerUp(2, Vec2(50, 50), 120);
}

static void TestButtonWithoutIntentDefaultsAllowed() {
  TouchService t;
  t.AddButton("Jump", Vec2(0, 0), Vec2(100, 100));
  t.OnPointerDown(1, Vec2(50, 50), 0);
  t.Update(10, nullptr);
  CHECK(t.buttonState(0).allowed);   // no intent -> nothing to refuse
}

//----------------------------------------------------------------------------
// Draw path
//----------------------------------------------------------------------------

static void TestDrawPath() {
  Renderer r;
  Berry::Test::CaptureRenderSeam seam;
  r.BindSeam(&seam);

  TouchService t;
  t.AddButton("Jump", Vec2(10, 10), Vec2(90, 60));
  t.AddButton("Sneak", Vec2(100, 10), Vec2(180, 60));

  r.BeginFrame(Vec2(400, 400));
  t.Draw(r);
  r.EndFrame();
  CHECK(seam.rects == 2);          // one rectangle per button
  CHECK(seam.texts == 2);          // one label per button
  bool sawJump = false, sawSneak = false;
  for (const auto& s : seam.textContents) {
    if (s == "Jump") sawJump = true;
    if (s == "Sneak") sawSneak = true;
  }
  CHECK(sawJump && sawSneak);
}

int main() {
  TestSwipeGeometry();
  TestConfigValidation();
  TestGestureToggle();
  TestTap();
  TestTapRejectedWhenMoved();
  TestDoubleTap();
  TestDoubleTapFarAwayIsSingle();
  TestSwipe();
  TestLongPress();
  TestDrag();
  TestPinch();
  TestRotate();
  TestSecondFingerLiftEndsPinch();
  TestPointerTableBound();
  TestNonFiniteFeedRejected();
  TestButtonAuthoring();
  TestButtonPressAndToggle();
  TestDisabledButtonNotPressed();
  TestButtonIntentPolicyGated();
  TestButtonWithoutIntentDefaultsAllowed();
  TestDrawPath();
  if (g_fail == 0) printf("test_touch: ALL OK\n");
  return g_fail;
}
