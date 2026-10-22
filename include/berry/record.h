#pragma once
// Record & replay: capture a tick's worth of world state and script events, and
// play it back through the seam so a script can be regression-tested with no
// game running.
//
// The recorder sits between the seam and the world grid. On each tick it takes
// the same `EntitySnapshot`s the grid would see, plus any script events a module
// emits, and appends them to an in-memory tape. The tape serialises to a single
// text document (one JSON object) so a failing session can be saved, attached to
// a bug report, and replayed on a developer machine.
//
// Replay is a `LevelSeam` implementation (`ReplaySeam`): feed it a tape and it
// answers `AllEntities`/`GetEntity`/`GetTime`/`GetBlock` from the recorded
// frames, advancing one frame per `Step()`. Because it implements the same seam
// every module already talks to, a script under test cannot tell it apart from
// the live game — which is the whole point.
//
// The tape is deliberately dumb and total:
//
//   * it stores snapshots, not pointers, so it is portable across builds and
//     carries nothing that could be dereferenced on playback
//   * a malformed tape is rejected wholesale, like every other data document in
//     BerryAPI; a partially-loaded tape never plays
//   * it is bounded (`kMaxFrames`, `kMaxEntitiesPerFrame`, `kMaxEventsPerFrame`)
//     so a runaway recorder cannot exhaust memory
//   * playback is read-only: `ReplaySeam` answers queries, it never writes to
//     game memory and never mutates the tape
//
// Client-side only: a tape records what this client saw. It is a debugging aid
// and a test fixture, not a network protocol.

#include "berry/export.h"
#include "berry/game.h"
#include "berry/types.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Berry {

/// A script event captured on the tape: a named channel plus an opaque payload
/// string (the same short string an EventBus listener would receive).
struct BERRY_API RecordedEvent {
  std::string channel;
  std::string payload;
};

/// A block value recorded for one coordinate.
struct BERRY_API RecordedBlock {
  i32 x = 0, y = 0, z = 0;
  u32 block = 0;
};

/// One tick's worth of recorded state.
struct BERRY_API RecordedFrame {
  u64 tick = 0;
  std::vector<EntitySnapshot> entities;
  std::vector<RecordedEvent> events;
  LevelTime time;
  // Blocks are sparse: only the coordinates a script actually read, so a tape
  // stays small and does not try to snapshot the world.
  std::vector<RecordedBlock> blocks;
};

/// Errors a tape can fail with. A tape with any error does not play.
enum class TapeError : u32 {
  Ok = 0,
  BadJson,
  MissingVersion,
  BadVersion,
  NoFrames,
  TooManyFrames,
  TooManyEntities,
  TooManyEvents,
  BadFrame,
  BadEntity,
  BadEvent,
  BadBlock,
  BadTick,        // ticks are not strictly increasing
};

const char* TapeErrorName(TapeError e);

/// A recorded session: an ordered list of frames. The tape's own format version
/// is checked on load so a future tape is refused rather than misread.
class BERRY_API Recording {
 public:
  /// The tape format version written into the document.
  static constexpr u32 kFormatVersion = 1;
  static constexpr size_t kMaxFrames = 20000;
  static constexpr size_t kMaxEntitiesPerFrame = 4096;
  static constexpr size_t kMaxEventsPerFrame = 4096;
  static constexpr size_t kMaxBlocksPerFrame = 4096;

  /// Appends a frame. Ticks must strictly increase; a repeat or a step back is
  /// refused so a tape always replays in order.
  bool Push(const RecordedFrame& frame);

  const std::vector<RecordedFrame>& frames() const { return frames_; }
  size_t frameCount() const { return frames_.size(); }
  bool empty() const { return frames_.empty(); }
  void Clear() { frames_.clear(); }

  /// Serialises to one JSON document. Deterministic: same tape, same bytes.
  std::string ToJson() const;

  /// Parses a tape. Rejects the whole document on any error. On success `out`
  /// holds exactly the recorded frames.
  static bool FromJson(const std::string& json, Recording* out,
                       TapeError* outError = nullptr, std::string* error = nullptr);

 private:
  std::vector<RecordedFrame> frames_;
};

//----------------------------------------------------------------------------
// Recorder — appends live state to a tape.
//----------------------------------------------------------------------------

/// Collects a frame from a seam. Call `BeginFrame`, `AddEntity`/`AddEvent`/
/// `AddBlock`, then `CommitFrame`. A recorder that hits a cap stops recording
/// and reports it rather than dropping silently.
class BERRY_API Recorder {
 public:
  explicit Recorder(Recording* tape) : tape_(tape) {}

  void BeginFrame(u64 tick, const LevelTime& time);
  bool AddEntity(const EntitySnapshot& snap);
  bool AddEvent(const std::string& channel, const std::string& payload);
  bool AddBlock(i32 x, i32 y, i32 z, u32 block);
  bool CommitFrame();

  /// Snapshots every entity and the level time from `seam` into the current
  /// frame. The one call a module makes each tick to record.
  void CaptureFrom(LevelSeam& seam);

  bool overflowed() const { return overflowed_; }
  bool active() const { return tape_ != nullptr; }

 private:
  Recording* tape_ = nullptr;
  RecordedFrame pending_;
  bool building_ = false;
  bool overflowed_ = false;
};

//----------------------------------------------------------------------------
// ReplaySeam — plays a tape back as a LevelSeam.
//----------------------------------------------------------------------------

/// A `LevelSeam` backed by a tape. `Step()` advances one frame; queries then
/// answer from that frame. Before the first `Step` (and after the last frame)
/// the seam reports an empty world, so a consumer that runs past the tape ends
/// up with nothing rather than a stale frame.
class BERRY_API ReplaySeam final : public LevelSeam {
 public:
  /// Borrows the tape; the tape must outlive the seam.
  explicit ReplaySeam(const Recording* tape);

  const char* Name() const override { return "replay"; }

  bool GetEntity(EntityId id, EntitySnapshot* out) const override;
  std::vector<EntityId> AllEntities() const override;
  std::vector<EntityId> EntitiesInAabb(const Aabb& box,
                                       EntityType filter = EntityType::Unknown) const override;
  std::vector<EntityId> EntitiesInRadius(const Vec3& center, f32 radius) const override;
  bool RaycastEntity(const Vec3& origin, const Vec3& dir, f32 maxDist,
                     EntityId* outId, f32* outDist) const override;
  bool RaycastBlock(const Vec3& origin, const Vec3& dir, f32 maxDist,
                    Vec3* outPos, u32* outBlockType) const override;
  u32 GetBlock(const Vec3& worldPos) const override;
  LevelTime GetTime() const override;

  /// Advances to the next frame. Returns false at the end of the tape.
  bool Step();
  /// Seeks to a frame index (0-based). Returns false when out of range.
  bool SeekTo(size_t index);
  /// Rewinds to just before the first frame (an empty world).
  void Rewind();

  /// The current frame index, or -1 before the first step.
  i64 frameIndex() const { return index_; }
  size_t frameCount() const { return tape_ ? tape_->frameCount() : 0; }
  /// True when there is no current frame: before the first step, or past the
  /// last one.
  bool atEnd() const { return current() == nullptr; }
  /// The tick of the current frame, or 0 when there is none.
  u64 currentTick() const;

  /// The events recorded in the current frame, for a caller that wants to feed
  /// them back into the EventBus on playback.
  const std::vector<RecordedEvent>* currentEvents() const;

 private:
  const RecordedFrame* current() const;

  const Recording* tape_ = nullptr;
  // -1 means "before the first frame": an empty world.
  i64 index_ = -1;
};

/// Replays a whole tape through a callback, one frame at a time. Handy in a
/// test: the callback runs with the seam positioned on each frame.
void ReplayAll(const Recording& tape,
               const std::function<void(ReplaySeam&, const RecordedFrame&)>& fn);

}  // namespace Berry
