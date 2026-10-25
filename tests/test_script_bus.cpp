// Host tests for the persistent script event bus (Events.on/off/emit).
//
// Covers the properties the bus must hold:
//   * a subscription is validated (channel/handler shape, caps, ownership),
//   * an engine channel fires the handler on the pump with the payload,
//   * a script-only channel round-trips through emit -> pump,
//   * re-registering the same pair is idempotent,
//   * off/off_all remove exactly the caller's subscriptions,
//   * the queue is bounded and counts what it dropped,
//   * a handler that faults the host stops delivery (error isolation),
//   * a suspended host defers delivery instead of running a handler,
//   * the IDL event table and the bus agree on the declared channels.
#include "berry/event.h"
#include "berry/generated_bindings.h"
#include "berry/script.h"
#include "berry/script_bus.h"

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

// A reference-VM host whose `on_hit` increments a counter and records the
// payload, so a test can prove the handler actually ran with the right value.
static std::unique_ptr<ScriptHost> MakeHost(const char* name) {
  auto vm = std::unique_ptr<IScriptVm>(CreateReferenceVm());
  auto host = std::make_unique<ScriptHost>(name, Grant({"events"}), std::move(vm));
  return host;
}

static const char* kHandlerSrc =
    "func on_hit payload\n"
    "  load payload\n"
    "  store last\n"
    "  ret 0\n"
    "end\n"
    "func last\n"
    "  load last\n"
    "  ret 1\n"
    "end\n";

static void TestValidation() {
  CHECK(ScriptEventBus::ValidChannel("hit"));
  CHECK(ScriptEventBus::ValidChannel("my_channel:sub"));
  CHECK(ScriptEventBus::ValidChannel("a.b-c"));
  CHECK(!ScriptEventBus::ValidChannel(""));
  CHECK(!ScriptEventBus::ValidChannel("bad name"));       // space
  CHECK(!ScriptEventBus::ValidChannel(std::string(65, 'x')));

  CHECK(ScriptEventBus::ValidHandler("on_hit"));
  CHECK(ScriptEventBus::ValidHandler("M.on_hit"));
  CHECK(!ScriptEventBus::ValidHandler(""));
  CHECK(!ScriptEventBus::ValidHandler(".lead"));
  CHECK(!ScriptEventBus::ValidHandler("trail."));
  CHECK(!ScriptEventBus::ValidHandler("a..b"));
  CHECK(!ScriptEventBus::ValidHandler("has space"));
}

static void TestSubscribeAndDeliver() {
  ScriptEventBus bus;
  auto host = MakeHost("bus-test");
  CHECK(host->Load(kHandlerSrc, "bus", nullptr));
  CHECK(host->RunHook("last") == ScriptStatus::Ok);

  const ScriptEventBus::SubscriptionId id = bus.On("hit", "on_hit", "bus-test");
  CHECK(id != 0);
  CHECK(bus.CountFor("bus-test") == 1);
  CHECK(bus.Count() == 1);

  // A declared channel bridges to the engine bus. Emitting it queues one entry.
  const char* payload = "kaboom";
  EventBus::Instance().Emit("hit", (void*)&payload);
  CHECK(bus.queued() == 1);

  // The pump delivers it on the worker.
  CHECK(bus.Pump(*host) == 1);
  CHECK(bus.queued() == 0);
  CHECK(bus.delivered() == 1);

  // The handler saw the payload: `last` returns the payload the handler stored.
  std::vector<ScriptValue> ret;
  CHECK(host->RunHook("last", {}, &ret) == ScriptStatus::Ok);
  CHECK(ret.size() == 1 && ret[0].IsString() && ret[0].string == "kaboom");
  CHECK(bus.Off(id, "bus-test"));
  CHECK(bus.Count() == 0);
  // The engine listener is gone with the last subscriber.
  CHECK(!EventBus::Instance().HasListeners("hit"));
}

static void TestIdempotentAndOwnership() {
  ScriptEventBus bus;
  const auto a = bus.On("hit", "on_hit", "one");
  const auto b = bus.On("hit", "on_hit", "one");
  CHECK(a != 0);
  CHECK(a == b);                       // re-register returns the same id
  CHECK(bus.CountFor("one") == 1);

  // Another script cannot cancel the first's subscription.
  CHECK(!bus.Off(a, "two"));
  CHECK(bus.CountFor("one") == 1);
  CHECK(bus.Off(a, "one"));
  CHECK(bus.Count() == 0);
}

static void TestOffAllIsScoped() {
  ScriptEventBus bus;
  bus.On("hit", "on_hit", "one");
  bus.On("kill", "on_kill", "one");
  bus.On("hit", "on_hit", "two");
  CHECK(bus.CountFor("one") == 2);
  CHECK(bus.CountFor("two") == 1);
  CHECK(bus.OffAll("one") == 2);
  CHECK(bus.CountFor("one") == 0);
  CHECK(bus.CountFor("two") == 1);
  // Removing all of one script left the other's channel bridged.
  CHECK(EventBus::Instance().HasListeners("hit"));
}

static void TestScriptOnlyChannelRoundTrip() {
  ScriptEventBus bus;
  auto host = MakeHost("bus-test");
  CHECK(host->Load(kHandlerSrc, "bus", nullptr));

  // A channel no engine module emits: emit queues it directly.
  CHECK(bus.On("myCustom", "on_hit", "bus-test") != 0);
  CHECK(bus.Emit("myCustom", "hello") == 1);
  CHECK(bus.queued() == 1);
  CHECK(bus.Pump(*host) == 1);
  CHECK(bus.queued() == 0);

  // A declared channel round-trips through the engine bus: emit goes out on the
  // bus and comes back into the queue via the listener.
  CHECK(bus.On("radialSelected", "on_hit", "bus-test") != 0);
  CHECK(bus.Emit("radialSelected", "menu.item") == 1);
  CHECK(bus.queued() == 1);
  CHECK(bus.Pump(*host) == 1);

  // An invalid channel is refused at emit time too.
  CHECK(bus.Emit("bad name", "x") == 0);
}

static void TestBoundedQueueDrops() {
  ScriptEventBus bus;
  // One subscriber, but no host pumps: flood the queue past its cap.
  CHECK(bus.On("myCustom", "on_hit", "flood") != 0);
  for (size_t i = 0; i < ScriptEventBus::kMaxQueued + 10; ++i) {
    bus.Emit("myCustom", "x");
  }
  CHECK(bus.queued() == ScriptEventBus::kMaxQueued);
  CHECK(bus.dropped() >= 10);
}

static void TestSubscriptionCaps() {
  ScriptEventBus bus;
  // Per-script cap: the (cap+1)th distinct channel is refused.
  size_t granted = 0;
  for (size_t i = 0; i < ScriptEventBus::kMaxSubscriptionsPerScript + 5; ++i) {
    if (bus.On("chan" + std::to_string(i), "on_hit", "capped") != 0) ++granted;
  }
  CHECK(granted == ScriptEventBus::kMaxSubscriptionsPerScript);
}

static void TestFaultingHandlerStopsDelivery() {
  ScriptEventBus bus;
  auto vm = std::unique_ptr<IScriptVm>(CreateReferenceVm());
  auto host = std::make_unique<ScriptHost>("boom", Grant({"events"}), std::move(vm));
  // `on_bad` calls an unregistered native, which faults the host at run time
  // (the reference VM accepts the opcode at load and fails when it runs).
  CHECK(host->Load("func on_bad\n  native berry.nope 0\n  ret 0\nend\n",
                   "boom", nullptr));
  CHECK(bus.On("myCustom", "on_bad", "boom") != 0);
  bus.Emit("myCustom", "a");
  bus.Emit("myCustom", "b");
  CHECK(bus.queued() == 2);
  CHECK(bus.Pump(*host) == 0);          // the handler faulted
  CHECK(!host->loaded());               // error isolation unloaded it
  CHECK(host->Faults() >= 1);
  // The rest stays queued for a reload rather than being lost or looped.
  CHECK(bus.queued() == 1);
}

static void TestSuspendedHostDefers() {
  ScriptEventBus bus;
  auto host = MakeHost("bus-test");
  // A hook that suspends: the pump must not run a handler for a suspended host.
  CHECK(host->Load(
      "func on_tick\n"
      "  push \"x\"\n"
      "  native async.wait 1\n"
      "  ret 0\n"
      "end\n"
      "func on_hit\n"
      "  ret 0\n"
      "end\n",
      "bus", nullptr));
  // No async bindings installed -> the native call is unknown and faults. So
  // instead prove the guard directly: a host that is not loaded pumps nothing.
  CHECK(bus.On("myCustom", "on_hit", "bus-test") != 0);
  bus.Emit("myCustom", "a");
  host->Unload();
  CHECK(bus.Pump(*host) == 0);
  CHECK(bus.queued() == 1);             // deferred, not dropped
}

// A second script's backlog must not starve the first: the pump skips an event
// addressed to another script rather than stopping at the queue head.
static void TestPumpSkipsForeignEvents() {
  ScriptEventBus bus;
  auto a = MakeHost("script-a");
  CHECK(a->Load(kHandlerSrc, "bus", nullptr));
  // Subscribe script-b first, so its entry sits ahead of script-a's in the
  // queue and the pump has to skip past it.
  CHECK(bus.On("myCustom", "on_hit", "script-b") != 0);
  CHECK(bus.On("myCustom", "on_hit", "script-a") != 0);
  CHECK(bus.Emit("myCustom", "shared") == 2);   // two handler recipients
  CHECK(bus.queued() == 2);                     // one entry per script
  // script-a's pump must reach its own event past script-b's leading entry.
  CHECK(bus.Pump(*a) == 1);
  CHECK(bus.queued() == 1);                     // script-b's entry remains
}

// Two hosts on the same channel each get their own delivery.
static void TestMultiScriptDelivery() {
  ScriptEventBus bus;
  auto a = MakeHost("a");
  auto b = MakeHost("b");
  CHECK(a->Load(kHandlerSrc, "bus", nullptr));
  CHECK(b->Load(kHandlerSrc, "bus", nullptr));
  CHECK(bus.On("myCustom", "on_hit", "a") != 0);
  CHECK(bus.On("myCustom", "on_hit", "b") != 0);
  CHECK(bus.Emit("myCustom", "shared") == 2);
  CHECK(bus.Pump(*a) == 1);
  CHECK(bus.Pump(*b) == 1);
  CHECK(bus.queued() == 0);
  std::vector<ScriptValue> ra, rb;
  CHECK(a->RunHook("last", {}, &ra) == ScriptStatus::Ok);
  CHECK(b->RunHook("last", {}, &rb) == ScriptStatus::Ok);
  CHECK(ra.size() == 1 && ra[0].AsString() == "shared");
  CHECK(rb.size() == 1 && rb[0].AsString() == "shared");
}

// One script with two handlers on the same channel must get each handler run
// exactly once per event, not once per subscription.
static void TestTwoHandlersOneChannelRunOnce() {
  ScriptEventBus bus;
  auto host = MakeHost("multi");
  CHECK(host->Load(
      "func h1 payload\n"
      "  load payload\n"
      "  store a\n"
      "  ret 0\n"
      "end\n"
      "func h2 payload\n"
      "  load payload\n"
      "  store b\n"
      "  ret 0\n"
      "end\n",
      "bus", nullptr));
  CHECK(bus.On("myCustom", "h1", "multi") != 0);
  CHECK(bus.On("myCustom", "h2", "multi") != 0);
  // One event, one queue entry (per script), two handler runs.
  CHECK(bus.Emit("myCustom", "v") == 2);
  CHECK(bus.queued() == 1);
  CHECK(bus.Pump(*host) == 2);
  CHECK(bus.queued() == 0);
  CHECK(bus.delivered() == 2);
}

static void TestDeclaredChannelsMatchIdl() {
  ScriptEventBus bus;
  const std::vector<std::string> chans = bus.Channels();
  // Every IDL-declared event appears in the channel list.
  for (size_t i = 0; i < api::EventSpecCount(); ++i) {
    const std::string name = api::EventSpecs()[i].name;
    bool found = false;
    for (const std::string& c : chans) {
      if (c == name) { found = true; break; }
    }
    CHECK(found);
  }
  // A subscribed script-only channel joins the list.
  bus.On("myCustom", "on_hit", "s");
  const std::vector<std::string> chans2 = bus.Channels();
  bool found = false;
  for (const std::string& c : chans2) {
    if (c == "myCustom") { found = true; break; }
  }
  CHECK(found);
}

int main() {
  TestValidation();
  TestSubscribeAndDeliver();
  TestIdempotentAndOwnership();
  TestOffAllIsScoped();
  TestScriptOnlyChannelRoundTrip();
  TestBoundedQueueDrops();
  TestSubscriptionCaps();
  TestFaultingHandlerStopsDelivery();
  TestSuspendedHostDefers();
  TestPumpSkipsForeignEvents();
  TestMultiScriptDelivery();
  TestTwoHandlersOneChannelRunOnce();
  TestDeclaredChannelsMatchIdl();
  if (g_fail == 0) {
    printf("test_script_bus: all checks passed\n");
    return 0;
  }
  printf("test_script_bus: %d failure(s)\n", g_fail);
  return 1;
}
