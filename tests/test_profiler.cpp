// Host tests for the profiler flame graph.
//
// Covers:
//   * flat span stats (total, count, max) still accumulate as before,
//   * nesting: a span opened inside another becomes its child,
//   * self time excludes child time, so a parent's total is the time actually
//     spent inside it,
//   * children are ordered by total time descending,
//   * the flat `spans` view is ordered by total time descending,
//   * a repeat of the same name aggregates into one node rather than two,
//   * reset drops the tree and the stats,
//   * a disabled profiler records nothing and Begin returns the null token.
#include "berry/event.h"

#include <cstdio>
#include <thread>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

// Burn a measurable slice of wall time so a span is non-zero.
static void SpinMs(int ms) {
  const u64 until = NowNs() + (u64)ms * 1000000ull;
  while (NowNs() < until) {}
}

static const Profiler::FlameNode* Find(const Profiler::FlameNode& n,
                                       const std::string& name) {
  if (n.name == name) return &n;
  for (const auto& c : n.children) {
    if (const Profiler::FlameNode* hit = Find(c, name)) return hit;
  }
  return nullptr;
}

static void TestFlatStats() {
  Profiler& p = Profiler::Instance();
  p.FlameReset();
  p.SetEnabled(true);

  { ProfilerScope s("work"); SpinMs(2); }
  { ProfilerScope s("work"); SpinMs(3); }

  const Profiler::SpanStats st = p.Stats("work");
  CHECK(st.count == 2);
  CHECK(st.totalMs >= 5.0);
  CHECK(st.maxMs >= 3.0);
  CHECK(st.maxMs <= st.totalMs);
  // An unknown name is an empty record, not a crash.
  CHECK(p.Stats("nope").count == 0);
}

static void TestNestingAndSelfTime() {
  Profiler& p = Profiler::Instance();
  p.FlameReset();
  p.SetEnabled(true);

  {
    ProfilerScope outer("outer");
    SpinMs(5);
    { ProfilerScope inner("inner"); SpinMs(10); }
    SpinMs(5);
  }

  Profiler::FlameNode root = p.Flame();
  const Profiler::FlameNode* outer = Find(root, "outer");
  CHECK(outer != nullptr);
  const Profiler::FlameNode* inner = Find(root, "inner");
  CHECK(inner != nullptr);

  // inner is a child of outer, not a sibling at the root.
  CHECK(outer->children.size() == 1);
  CHECK(outer->children[0].name == "inner");
  CHECK(root.children.size() == 1);

  // A parent's total covers its children; its self time excludes them.
  CHECK(outer->totalMs >= inner->totalMs);
  CHECK(outer->selfMs < outer->totalMs);
  CHECK(outer->selfMs >= 9.0);       // the two 5ms sleeps, ~10ms
  CHECK(inner->selfMs == inner->totalMs);   // leaf: all self
  CHECK(inner->totalMs >= 10.0);
  CHECK(outer->count == 1);
  CHECK(inner->count == 1);
}

static void TestChildrenSortedByTotal() {
  Profiler& p = Profiler::Instance();
  p.FlameReset();
  p.SetEnabled(true);

  {
    ProfilerScope parent("parent");
    { ProfilerScope cheap("cheap"); SpinMs(1); }
    { ProfilerScope pricey("pricey"); SpinMs(8); }
    { ProfilerScope mid("mid"); SpinMs(4); }
  }

  Profiler::FlameNode root = p.Flame();
  const Profiler::FlameNode* parent = Find(root, "parent");
  CHECK(parent != nullptr);
  CHECK(parent->children.size() == 3);
  CHECK(parent->children[0].name == "pricey");
  CHECK(parent->children[1].name == "mid");
  CHECK(parent->children[2].name == "cheap");
}

static void TestRepeatsAggregateIntoOneNode() {
  Profiler& p = Profiler::Instance();
  p.FlameReset();
  p.SetEnabled(true);

  for (int i = 0; i < 3; ++i) {
    ProfilerScope parent("loop");
    { ProfilerScope body("body"); SpinMs(1); }
  }

  Profiler::FlameNode root = p.Flame();
  const Profiler::FlameNode* loop = Find(root, "loop");
  CHECK(loop != nullptr);
  CHECK(loop->count == 3);
  // One `body` child, not three: the same name aggregates.
  CHECK(loop->children.size() == 1);
  CHECK(loop->children[0].name == "body");
  CHECK(loop->children[0].count == 3);
  CHECK(root.children.size() == 1);
}

static void TestFlatSpansSorted() {
  Profiler& p = Profiler::Instance();
  p.FlameReset();
  p.SetEnabled(true);

  { ProfilerScope s("small"); SpinMs(1); }
  { ProfilerScope s("big"); SpinMs(8); }
  { ProfilerScope s("medium"); SpinMs(4); }

  const auto rows = p.Spans();
  CHECK(rows.size() == 3);
  CHECK(rows[0].first == "big");
  CHECK(rows[1].first == "medium");
  CHECK(rows[2].first == "small");
  for (size_t i = 1; i < rows.size(); ++i) {
    CHECK(rows[i - 1].second.totalMs >= rows[i].second.totalMs);
  }
}

static void TestReset() {
  Profiler& p = Profiler::Instance();
  p.FlameReset();
  p.SetEnabled(true);
  { ProfilerScope s("gone"); SpinMs(1); }
  CHECK(p.Stats("gone").count == 1);
  CHECK(!p.Flame().children.empty());

  p.FlameReset();
  CHECK(p.Stats("gone").count == 0);
  CHECK(p.Flame().children.empty());
  CHECK(p.Spans().empty());
}

static void TestDisabledRecordsNothing() {
  Profiler& p = Profiler::Instance();
  p.FlameReset();
  p.SetEnabled(false);
  u64 token = p.Begin("ignored");
  CHECK(token == 0);
  p.End(token);
  CHECK(p.Stats("ignored").count == 0);
  CHECK(p.Flame().children.empty());
  p.SetEnabled(true);
}

static void TestCrossThreadSpanKeepsStackBalanced() {
  Profiler& p = Profiler::Instance();
  p.FlameReset();
  p.SetEnabled(true);

  // A span opened on another thread lands under the root, and its End on this
  // thread must not pop this thread's stack.
  u64 token = 0;
  std::thread other([&] { token = p.Begin("worker"); SpinMs(2); });
  other.join();
  { ProfilerScope mine("mine"); SpinMs(1); }
  p.End(token);   // ends the worker span from this thread

  Profiler::FlameNode root = p.Flame();
  CHECK(Find(root, "worker") != nullptr);
  CHECK(Find(root, "mine") != nullptr);
  // "mine" must still be a direct child of the root, not nested under worker.
  CHECK(root.children.size() == 2);
  // The stack is balanced: a fresh span is again a root child.
  { ProfilerScope after("after"); SpinMs(1); }
  CHECK(p.Flame().children.size() == 3);
}

int main() {
  TestFlatStats();
  TestNestingAndSelfTime();
  TestChildrenSortedByTotal();
  TestRepeatsAggregateIntoOneNode();
  TestFlatSpansSorted();
  TestReset();
  TestDisabledRecordsNothing();
  TestCrossThreadSpanKeepsStackBalanced();
  if (g_fail == 0) printf("test_profiler: ALL OK\n");
  return g_fail == 0 ? 0 : 1;
}
