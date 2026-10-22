// Host tests for reactive world queries: the spatial grid, the tick diff, and
// the seam driver.
//
// Covers:
//   * Grid cell mapping, including negative coordinates (floor, not truncate).
//   * Query: radius, type filter, aliveOnly, maxResults, and the deterministic
//     id-sorted order.
//   * Diff: entered, left and updated, and that an unchanged entity produces no
//     event.
//   * Overflow: a full grid refuses further adds rather than growing.
//   * The diff bus: fan-out, unsubscribe, and re-entrancy (a listener may
//     unsubscribe from inside its own callback).
//   * UpdateWorldGrid against a LevelSeam.
#include "berry/world_query.h"

#include "shim_level.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

EntityId Id(u64 raw, u16 runtime = 0) {
  EntityId id;
  id.raw = raw;
  id.runtime = runtime;
  return id;
}

EntitySnapshot Snap(u64 raw, const Vec3& pos, EntityType type = EntityType::Mob,
                    bool alive = true, f32 health = 20) {
  EntitySnapshot s;
  s.id = Id(raw, (u16)raw);
  s.position = pos;
  s.type = type;
  s.alive = alive;
  s.health = health;
  s.maxHealth = 20;
  return s;
}

}  // namespace

//----------------------------------------------------------------------------
// Grid basics
//----------------------------------------------------------------------------

static void TestGridBasics() {
  SpatialGrid g(16.0f);
  CHECK(g.cellSize() == 16.0f);
  CHECK(g.Count() == 0);

  g.BeginTick(1);
  CHECK(g.Add(Snap(1, Vec3(0, 0, 0))));
  CHECK(g.Add(Snap(2, Vec3(20, 0, 0))));
  CHECK(g.Add(Snap(3, Vec3(-20, 0, 0))));
  std::vector<WorldDiffEvent> d = g.EndTick();
  CHECK(g.Count() == 3);
  CHECK(d.size() == 3);   // all entered
  for (const auto& e : d) CHECK(e.kind == WorldDiff::Entered);

  // Get round-trips.
  EntitySnapshot got;
  CHECK(g.Get(Id(2, 2), &got));
  CHECK(got.position.x == 20);
  CHECK(!g.Get(Id(99, 99), &got));

  // A fresh grid with a non-positive cell size falls back to the default.
  SpatialGrid bad(0.0f);
  CHECK(bad.cellSize() == SpatialGrid::kDefaultCell);
  SpatialGrid nan(-1.0f);
  CHECK(nan.cellSize() == SpatialGrid::kDefaultCell);
}

//----------------------------------------------------------------------------
// Query
//----------------------------------------------------------------------------

static void TestQuery() {
  SpatialGrid g(16.0f);
  g.BeginTick(1);
  // Player at origin, two mobs near, one far, one dead.
  g.Add(Snap(1, Vec3(0, 0, 0), EntityType::Player));
  g.Add(Snap(2, Vec3(5, 0, 0), EntityType::Mob));
  g.Add(Snap(3, Vec3(0, 0, 10), EntityType::Mob, false));   // dead
  g.Add(Snap(4, Vec3(100, 0, 0), EntityType::Mob));
  g.Add(Snap(5, Vec3(3, 0, 3), EntityType::Item));
  g.EndTick();

  WorldQuery q;
  q.center = Vec3(0, 0, 0);

  // No radius: everything, sorted by id.
  q.within = 0;
  std::vector<EntitySnapshot> all = g.Query(q);
  CHECK(all.size() == 5);
  CHECK(all[0].id.raw == 1 && all[4].id.raw == 5);

  // Radius 20: the far one (id 4) drops out.
  q.within = 20;
  std::vector<EntitySnapshot> near = g.Query(q);
  CHECK(near.size() == 4);
  for (const auto& s : near) CHECK(s.id.raw != 4);

  // Radius 20 and alive only: the dead one drops out too.
  q.aliveOnly = true;
  std::vector<EntitySnapshot> alive = g.Query(q);
  CHECK(alive.size() == 3);

  // Type filter.
  q.aliveOnly = false;
  q.filter = EntityType::Mob;
  std::vector<EntitySnapshot> mobs = g.Query(q);
  CHECK(mobs.size() == 2);
  for (const auto& s : mobs) CHECK(s.type == EntityType::Mob);

  // maxResults caps the result, keeping the lowest ids.
  q.filter = EntityType::Unknown;
  q.maxResults = 2;
  std::vector<EntitySnapshot> capped = g.Query(q);
  CHECK(capped.size() == 2);
  CHECK(capped[0].id.raw == 1 && capped[1].id.raw == 2);

  // QueryIds matches Query order.
  q.maxResults = 0;
  std::vector<EntityId> ids = g.QueryIds(q);
  CHECK(ids.size() == near.size());
  CHECK(ids[0].raw == near[0].id.raw);

  // A query centred on a negative coordinate finds the right entities: this is
  // where floor-vs-truncate cell mapping would break.
  WorldQuery neg;
  neg.center = Vec3(-24, 0, 0);
  neg.within = 5;
  g.BeginTick(2);
  g.Add(Snap(10, Vec3(-24, 0, 0)));
  g.Add(Snap(11, Vec3(-30, 0, 0)));
  g.EndTick();
  std::vector<EntitySnapshot> negres = g.Query(neg);
  CHECK(negres.size() == 1);
  CHECK(negres[0].id.raw == 10);
}

//----------------------------------------------------------------------------
// Diff
//----------------------------------------------------------------------------

static void TestDiff() {
  SpatialGrid g(16.0f);

  // Tick 1: three entities enter.
  g.BeginTick(1);
  g.Add(Snap(1, Vec3(0, 0, 0)));
  g.Add(Snap(2, Vec3(5, 0, 0)));
  g.Add(Snap(3, Vec3(10, 0, 0)));
  std::vector<WorldDiffEvent> d1 = g.EndTick();
  CHECK(d1.size() == 3);

  // Tick 2: 2 moves, 3 leaves, 4 enters. 1 is unchanged.
  g.BeginTick(2);
  g.Add(Snap(1, Vec3(0, 0, 0)));      // unchanged
  g.Add(Snap(2, Vec3(6, 0, 0)));      // moved
  g.Add(Snap(4, Vec3(1, 0, 0)));      // entered
  std::vector<WorldDiffEvent> d2 = g.EndTick();

  int entered = 0, left = 0, updated = 0;
  for (const auto& e : d2) {
    if (e.kind == WorldDiff::Entered) { ++entered; CHECK(e.id.raw == 4); CHECK(e.after.position.x == 1); }
    if (e.kind == WorldDiff::Left) { ++left; CHECK(e.id.raw == 3); CHECK(e.before.position.x == 10); }
    if (e.kind == WorldDiff::Updated) { ++updated; CHECK(e.id.raw == 2); CHECK(e.before.position.x == 5); CHECK(e.after.position.x == 6); }
  }
  CHECK(entered == 1 && left == 1 && updated == 1);

  // Tick 3: nothing changes; no events at all.
  g.BeginTick(3);
  g.Add(Snap(1, Vec3(0, 0, 0)));
  g.Add(Snap(2, Vec3(6, 0, 0)));
  g.Add(Snap(4, Vec3(1, 0, 0)));
  std::vector<WorldDiffEvent> d3 = g.EndTick();
  CHECK(d3.empty());

  // A health change counts as Updated.
  g.BeginTick(4);
  g.Add(Snap(1, Vec3(0, 0, 0), EntityType::Mob, true, 5));
  g.Add(Snap(2, Vec3(6, 0, 0)));
  g.Add(Snap(4, Vec3(1, 0, 0)));
  std::vector<WorldDiffEvent> d4 = g.EndTick();
  CHECK(d4.size() == 1);
  CHECK(d4[0].kind == WorldDiff::Updated && d4[0].id.raw == 1);

  // Clearing reports the survivors as Left on the next tick.
  g.Clear();
  CHECK(g.Count() == 0);
  g.BeginTick(5);
  std::vector<WorldDiffEvent> d5 = g.EndTick();
  CHECK(d5.empty());
}

//----------------------------------------------------------------------------
// Overflow
//----------------------------------------------------------------------------

static void TestOverflow() {
  SpatialGrid g(16.0f);
  g.BeginTick(1);
  // Fill to the cap, then one more.
  for (size_t i = 0; i < SpatialGrid::kMaxEntities; ++i) {
    CHECK(g.Add(Snap(i + 1, Vec3((f32)(i % 64), 0, 0))));
  }
  CHECK(!g.overflowed());
  // Re-adding an existing id at the cap is fine (an update, not a new entity).
  CHECK(g.Add(Snap(1, Vec3(1, 2, 3))));
  // A genuinely new id is refused.
  CHECK(!g.Add(Snap(999999, Vec3(0, 0, 0))));
  CHECK(g.overflowed());
  g.EndTick();
}

//----------------------------------------------------------------------------
// Diff bus
//----------------------------------------------------------------------------

static void TestDiffBus() {
  WorldDiffBus bus;
  int count = 0;
  WorldDiff last;
  const WorldDiffBus::ListenerId id = bus.On([&](const WorldDiffEvent& e) {
    ++count;
    last = e.kind;
  });
  CHECK(bus.ListenerCount() == 1);

  std::vector<WorldDiffEvent> events;
  WorldDiffEvent e;
  e.kind = WorldDiff::Entered;
  e.id = Id(7, 7);
  events.push_back(e);
  CHECK(bus.Publish(events) == 1);
  CHECK(count == 1 && last == WorldDiff::Entered);

  // Fan-out: a second listener also sees it.
  int second = 0;
  bus.On([&](const WorldDiffEvent&) { ++second; });
  CHECK(bus.ListenerCount() == 2);
  bus.Publish(events);
  CHECK(count == 2 && second == 1);

  CHECK(bus.Off(id));
  CHECK(!bus.Off(id));   // already gone
  bus.Publish(events);
  CHECK(count == 2 && second == 2);

  // A listener that unsubscribes itself does not deadlock or crash.
  WorldDiffBus reentrant;
  WorldDiffBus::ListenerId self = 0;
  self = reentrant.On([&](const WorldDiffEvent&) { reentrant.Off(self); });
  reentrant.Publish(events);
  CHECK(reentrant.ListenerCount() == 0);

  bus.Clear();
  CHECK(bus.ListenerCount() == 0);
}

//----------------------------------------------------------------------------
// Seam driver
//----------------------------------------------------------------------------

static void TestSeamDriver() {
  Berry::Test::ShimLevel level;
  level.AddEntity(Id(1, 1), Snap(1, Vec3(0, 0, 0), EntityType::Player));
  level.AddEntity(Id(2, 2), Snap(2, Vec3(4, 0, 0)));

  SpatialGrid grid;
  WorldDiffBus bus;
  int published = 0;
  bus.On([&](const WorldDiffEvent&) { ++published; });

  std::vector<WorldDiffEvent> d1 = UpdateWorldGrid(grid, level, 1, &bus);
  CHECK(d1.size() == 2);   // both entered
  CHECK(published == 2);
  CHECK(grid.Count() == 2);

  // No change: no diff.
  std::vector<WorldDiffEvent> d2 = UpdateWorldGrid(grid, level, 2, &bus);
  CHECK(d2.empty());

  // Remove one, add one.
  level.RemoveEntity(Id(2, 2));
  level.AddEntity(Id(3, 3), Snap(3, Vec3(9, 0, 0)));
  std::vector<WorldDiffEvent> d3 = UpdateWorldGrid(grid, level, 3, &bus);
  int left = 0, entered = 0;
  for (const auto& e : d3) {
    if (e.kind == WorldDiff::Left) ++left;
    if (e.kind == WorldDiff::Entered) ++entered;
  }
  CHECK(left == 1 && entered == 1);
  CHECK(grid.Count() == 2);

  // A null bus is fine.
  std::vector<WorldDiffEvent> d4 = UpdateWorldGrid(grid, level, 4, nullptr);
  CHECK(d4.empty());
}

static void TestEventBridge() {
  // EntityType names round-trip.
  EntityType t;
  CHECK(ParseEntityType("player", &t) && t == EntityType::Player);
  CHECK(ParseEntityType("mob", &t) && t == EntityType::Mob);
  CHECK(!ParseEntityType("dragon", &t));
  CHECK(std::string(EntityTypeName(EntityType::Projectile)) == "projectile");

  // A diff published to the EventBus reaches a listener, and the payload is the
  // short "<raw>:<type>" line, not a pointer into game memory.
  std::string got;
  auto id = EventBus::Instance().On("entityEntered", [&](const EventContext& e) {
    if (e.data) got = *(const char**)e.data;
    return EventResult::Continue;
  });
  CHECK(EventBus::Instance().HasListeners("entityEntered"));

  std::vector<WorldDiffEvent> events;
  WorldDiffEvent ev;
  ev.kind = WorldDiff::Entered;
  ev.id = Id(42, 42);
  ev.after = Snap(42, Vec3(0, 0, 0), EntityType::Player);
  events.push_back(ev);
  // An Entered publishes twice: the entityEntered channel and the derived
  // entitySpawn lifecycle channel.
  std::string spawnPayload;
  auto spawnId = EventBus::Instance().On("entitySpawn", [&](const EventContext& e) {
    if (e.data) spawnPayload = *(const char**)e.data;
    return EventResult::Continue;
  });
  CHECK(PublishWorldDiffToEvents(events) == 2);
  CHECK(got == "42:player");
  CHECK(spawnPayload == "42:player");
  EventBus::Instance().Off("entitySpawn", spawnId);

  // A death is an Updated transition from alive to not-alive; it publishes the
  // entityUpdated channel and the derived entityDeath channel.
  std::string deathPayload;
  auto deathId = EventBus::Instance().On("entityDeath", [&](const EventContext& e) {
    if (e.data) deathPayload = *(const char**)e.data;
    return EventResult::Continue;
  });
  std::vector<WorldDiffEvent> deaths;
  WorldDiffEvent de;
  de.kind = WorldDiff::Updated;
  de.id = Id(9, 9);
  de.before = Snap(9, Vec3(0, 0, 0), EntityType::Mob);
  de.before.alive = true;
  de.after = Snap(9, Vec3(0, 0, 0), EntityType::Mob);
  de.after.alive = false;
  deaths.push_back(de);
  CHECK(PublishWorldDiffToEvents(deaths) == 2);
  CHECK(deathPayload == "9:mob");
  EventBus::Instance().Off("entityDeath", deathId);

  // A Left event publishes the before-snapshot's payload.
  std::vector<WorldDiffEvent> lefts;
  WorldDiffEvent le;
  le.kind = WorldDiff::Left;
  le.id = Id(7, 7);
  le.before = Snap(7, Vec3(0, 0, 0), EntityType::Mob);
  lefts.push_back(le);
  std::string leftPayload;
  EventBus::Instance().On("entityLeft", [&](const EventContext& e) {
    if (e.data) leftPayload = *(const char**)e.data;
    return EventResult::Continue;
  });
  CHECK(PublishWorldDiffToEvents(lefts) == 1);
  CHECK(leftPayload == "7:mob");
  EventBus::Instance().Off("entityEntered", id);

  // Block changes go out on their own channel.
  std::string blockPayload;
  EventBus::Instance().On(BlockChangedChannel(), [&](const EventContext& e) {
    if (e.data) blockPayload = *(const char**)e.data;
    return EventResult::Continue;
  });
  std::vector<BlockChange> changes;
  BlockChange bc;
  bc.pos = Vec3(3, 64, -5);
  bc.before = 1;
  bc.after = 9;
  changes.push_back(bc);
  CHECK(PublishBlockChangesToEvents(changes) == 1);
  CHECK(blockPayload == "3,64,-5,9");
  EventBus::Instance().Clear(BlockChangedChannel());
  EventBus::Instance().Clear("entityLeft");

  // The combat bridge: a CombatEvent published by Berry::Combat reaches the
  // script-visible entityHurt channel with a plain "<id>:<damage>" payload.
  std::string hurtPayload;
  EventBus::Instance().On(EntityHurtChannel(), [&](const EventContext& e) {
    if (e.data) hurtPayload = *(const char**)e.data;
    return EventResult::Continue;
  });
  CombatEvent ce;
  ce.victim = Id(42, 42);
  ce.attacker = Id(1, 1);
  ce.finalDamage = 3.5f;
  CHECK(PublishCombatToEvents(ce) == 1);
  CHECK(hurtPayload == "42:3.500");
  EventBus::Instance().Clear(EntityHurtChannel());

  // A kill routed through Berry::Combat itself publishes both the internal
  // "berry:combat" channel and the script-visible entityHurt channel.
  Combat combat;
  std::string liveHurt;
  EventBus::Instance().On(EntityHurtChannel(), [&](const EventContext& e) {
    if (e.data) liveHurt = *(const char**)e.data;
    return EventResult::Continue;
  });
  CombatEvent hit;
  hit.victim = Id(7, 7);
  hit.attacker = Id(1, 1);
  hit.baseDamage = 4.0f;
  hit.finalDamage = 4.0f;
  CHECK(combat.Observe(hit) == CombatOutcome::Observed);
  CHECK(liveHurt == "7:4.000");
  EventBus::Instance().Clear(EntityHurtChannel());
}

int main() {
  TestGridBasics();
  TestQuery();
  TestDiff();
  TestOverflow();
  TestDiffBus();
  TestSeamDriver();
  TestEventBridge();
  if (g_fail == 0) printf("test_world_query: ALL OK\n");
  return g_fail;
}
