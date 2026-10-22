#pragma once
// Reactive world queries: a per-tick spatial grid with diff events.
//
// A script asks "which entities are near me?" every frame, and the naive answer
// walks every entity every time. This keeps a uniform grid keyed by cell, so a
// radius query touches only the cells the radius overlaps, and a tick rebuild
// diffs against the previous tick to emit `entityEntered` / `entityLeft`.
//
// Two properties matter more than speed here:
//
//   * It is a pure data structure. It never reads game memory; the caller feeds
//     it snapshots through `BeginTick`/`Add`. That makes it testable on the host
//     with no game running, and keeps Berry::World read-only.
//   * Its output is deterministic. Entities come back sorted by id, and the
//     diff is computed against a stable previous set, so a script that logs
//     "entered/left" sees the same thing twice for the same input.
//
// The grid is client-side only: it caches what the seam already told us, it
// never becomes a second source of truth about the world.

#include "berry/combat.h"
#include "berry/event.h"
#include "berry/export.h"
#include "berry/game.h"
#include "berry/math.h"
#include "berry/types.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {

/// How an entity's membership changed between the last two ticks.
enum class WorldDiff : u32 {
  Entered = 0,   // present now, absent before
  Left,          // absent now, present before
  Updated,       // present both times, but its snapshot changed
};

const char* WorldDiffName(WorldDiff d);

/// One entity's transition. `before` is the previous snapshot for Left/Updated
/// (zero-valued for Entered), `after` the current one (zero-valued for Left).
struct BERRY_API WorldDiffEvent {
  WorldDiff kind = WorldDiff::Entered;
  EntityId id;
  EntitySnapshot before;
  EntitySnapshot after;
};

/// A query against the grid. `filter` of Unknown matches every type; `within`
/// of 0 means "no radius limit". `maxResults` of 0 means "no limit".
struct BERRY_API WorldQuery {
  Vec3 center;
  f32 within = 0;
  EntityType filter = EntityType::Unknown;
  bool aliveOnly = false;
  size_t maxResults = 0;
};

/// One block's change within a tick: where, and what it was and became. A
/// script reads this instead of polling `world.block_at` on a grid of points.
struct BERRY_API BlockChange {
  Vec3 pos;
  u32 before = 0;
  u32 after = 0;
};

/// A uniform spatial grid over entity snapshots. Thread-safe: a query may run
/// on the render thread while the sim thread rebuilds for the next tick.
class BERRY_API SpatialGrid {
 public:
  /// The default cell edge in blocks. 16 is a chunk-sized bucket: small enough
  /// that a 20-block query touches ~4 cells, large enough that the per-cell
  /// bookkeeping does not dominate.
  static constexpr f32 kDefaultCell = 16.0f;
  /// More entities than this and the grid refuses the tick rather than grow
  /// without bound; the caller keeps the previous tick's contents.
  static constexpr size_t kMaxEntities = 4096;
  /// How many diff events a single tick will buffer before it stops recording.
  static constexpr size_t kMaxDiffEvents = 4096;

  explicit SpatialGrid(f32 cellSize = kDefaultCell);

  f32 cellSize() const { return cell_; }

  /// Clears the "current" buffer and sets the tick number. Entities added after
  /// this belong to the new tick.
  void BeginTick(u64 tick);

  /// Adds or replaces an entity in the current tick. Returns false when the
  /// grid is full (the tick is then incomplete and `overflowed()` is set).
  bool Add(const EntitySnapshot& snap);

  /// Finishes the tick: computes the diff against the previous tick, swaps the
  /// current set into place, and returns the events. Safe to call with no
  /// entities.
  std::vector<WorldDiffEvent> EndTick();

  /// Entities matching `q`, sorted by (raw id, runtime id) so the order is
  /// stable across ticks. Only entities from the last completed tick are
  /// considered, so a query never sees a half-built tick.
  std::vector<EntitySnapshot> Query(const WorldQuery& q) const;

  /// Just the ids matching `q`, in the same order as Query.
  std::vector<EntityId> QueryIds(const WorldQuery& q) const;

  /// How many entities the last completed tick holds.
  size_t Count() const;

  /// The entity's snapshot from the last completed tick, if present.
  bool Get(EntityId id, EntitySnapshot* out) const;

  /// True when the last tick hit `kMaxEntities` and dropped entities.
  bool overflowed() const;

  /// Drops everything and resets the tick counter. The next EndTick reports
  /// every remaining entity as Left relative to the cleared set.
  void Clear();

  /// The cell edge is fixed at construction; changing it would invalidate the
  /// occupancy map, so there is no setter on purpose.

 private:
  struct Key {
    i64 x = 0, y = 0, z = 0;
    bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const;
  };

  Key CellOf(const Vec3& p) const;
  static bool Matches(const EntitySnapshot& s, const WorldQuery& q);

  mutable std::mutex mutex_;
  f32 cell_;
  u64 tick_ = 0;
  bool overflowed_ = false;
  // The completed tick's entities, keyed by id, plus a cell index for queries.
  std::unordered_map<u64, EntitySnapshot> current_;   // being built
  std::unordered_map<u64, EntitySnapshot> stable_;    // last completed
  std::unordered_map<Key, std::vector<u64>, KeyHash> cells_;
  std::vector<u64> stableIds_;                        // sorted, for the diff
};

//----------------------------------------------------------------------------
// Entity id helpers (the grid keys on the raw id).
//----------------------------------------------------------------------------

inline u64 EntityKey(const EntityId& id) { return id.raw; }

//----------------------------------------------------------------------------
// Diff bus: a thin fan-out so several subscribers (a script, a HUD, a recorder)
// can each see the tick's transitions without the grid holding listeners.
// ----------------------------------------------------------------------------

/// Receives one tick's diff. Called on the thread that calls `Publish`.
using WorldDiffListener = std::function<void(const WorldDiffEvent&)>;

/// Dispatches `WorldDiffEvent`s to listeners. Thread-safe. Listeners are called
/// outside the mutex so a listener may subscribe or unsubscribe from within its
/// own callback without deadlocking.
class BERRY_API WorldDiffBus {
 public:
  static WorldDiffBus& Instance();

  using ListenerId = u64;

  ListenerId On(WorldDiffListener fn);
  bool Off(ListenerId id);
  void Clear();

  /// Delivers every event in order. Returns how many were delivered.
  size_t Publish(const std::vector<WorldDiffEvent>& events);

  size_t ListenerCount() const;

 private:
  struct Entry { ListenerId id; WorldDiffListener fn; };
  mutable std::mutex mutex_;
  std::vector<Entry> listeners_;
  ListenerId nextId_ = 1;
};

/// Drives a grid from a LevelSeam: snapshots every entity the seam reports,
/// ends the tick, and publishes the diff. This is the one call a module makes
/// each tick. Returns the diff, so a caller can also consume it directly.
std::vector<WorldDiffEvent> UpdateWorldGrid(SpatialGrid& grid, LevelSeam& seam,
                                            u64 tick, WorldDiffBus* bus = nullptr);

/// The EventBus channels a world diff is published on, so a script can
/// `await Events.once("entityEntered")` without any new binding.
namespace world_event {
inline const char* EntityEntered() { return "entityEntered"; }
inline const char* EntityLeft() { return "entityLeft"; }
inline const char* EntityUpdated() { return "entityUpdated"; }
/// Lifecycle channels derived from the same diff: a spawn is an enter, a
/// death is a transition to not-alive. Kept distinct so a script can listen
/// for the narrower event without filtering every update.
inline const char* EntitySpawn() { return "entitySpawn"; }
inline const char* EntityDeath() { return "entityDeath"; }
}  // namespace world_event

/// The channel a diff kind is published on, or nullptr for an unknown kind.
const char* WorldDiffChannel(WorldDiff d);

/// The EventBus channel a block change is published on.
inline const char* BlockChangedChannel() { return "blockChanged"; }

/// The EventBus channel a combat observation is published on, for the
/// `entityHurt` lifecycle event. Distinct from the internal "berry:combat"
/// channel, whose payload is a struct pointer rather than a script string.
inline const char* EntityHurtChannel() { return "entityHurt"; }

/// Publishes a tick's block changes onto `blockChanged`. The payload is
/// "<x>,<y>,<z>,<after>", so a listener can split it without a parser and never
/// receives a pointer into game memory. Returns how many were published.
size_t PublishBlockChangesToEvents(const std::vector<BlockChange>& changes);

/// The payload published for an entity: "<raw id>:<type>". Kept to one short
/// line so a script can split it without a parser, and so the payload a
/// listener receives is never a pointer into game memory.
std::string WorldDiffPayload(const EntitySnapshot& snap);

/// Publishes a tick's diff onto the EventBus channels above, so scripts reach
/// it through `Events.once`. This is the bridge between the grid and the async
/// layer; the grid itself stays a plain data structure. Returns how many
/// events were published.
size_t PublishWorldDiffToEvents(const std::vector<WorldDiffEvent>& events);

/// Publishes one combat observation onto the `entityHurt` channel, so a script
/// reaches it through `Events.once` with a plain "<id>:<damage>" payload. This
/// is the bridge from the combat bus (whose payload is a struct pointer) to the
/// script-visible event layer. Returns 1.
size_t PublishCombatToEvents(const CombatEvent& evt);

}  // namespace Berry
