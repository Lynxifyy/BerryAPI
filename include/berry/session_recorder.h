#pragma once
// Session recorder: drives record.h's Recorder/ReplaySeam from the module tick
// and hands the tape to a script.
//
// `record.h` gives the pieces — a Recorder that appends a frame from a seam, a
// Recording that serialises to one JSON document, and a ReplaySeam that plays a
// tape back as a LevelSeam. This is the piece that connects them to the running
// client: it captures one frame per tick from the same seam the world grid
// reads, and it can swap that seam for the replay so every read-only query a
// script makes answers from the tape instead of the game.
//
// The recorder never touches game memory: it snapshots what the seam already
// reported, exactly like the grid. It is client-side only — a tape is what this
// client saw, a debugging aid and a test fixture, not a network protocol.
//
// Bounded by record.h's caps (`kMaxFrames`, `kMaxEntitiesPerFrame`, …), so a
// runaway session stops recording rather than exhausting memory; `overflowed()`
// reports that it did.

#include "berry/export.h"
#include "berry/game.h"
#include "berry/record.h"
#include "berry/types.h"
#include "berry/world_query.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Berry {

/// Owns a tape and the recording/replay state over it. Thread-safe: the tick
/// captures on the sim thread while a script may read the tape or step a replay
/// from elsewhere.
class BERRY_API SessionRecorder {
 public:
  SessionRecorder();
  ~SessionRecorder();

  SessionRecorder(const SessionRecorder&) = delete;
  SessionRecorder& operator=(const SessionRecorder&) = delete;

  //--- capture -------------------------------------------------------------

  /// Begins a fresh recording, clearing any previous tape so a session always
  /// starts from a known state. Returns false when already recording.
  bool Start();

  /// Stops recording. Returns false when not recording. The tape is kept.
  bool Stop();

  /// True between Start and Stop.
  bool recording() const;

  /// Captures one frame while recording: the level time and every entity the
  /// seam reports, plus the tick's world-diff events (so a replay carries the
  /// entered/left/updated transitions the grid produced, not just snapshots).
  /// A no-op when not recording. `diff` may be empty.
  void CaptureTick(u64 tick, LevelSeam& seam,
                   const std::vector<WorldDiffEvent>& diff = {});

  /// True when a capture hit a cap and dropped data.
  bool overflowed() const;

  //--- tape ----------------------------------------------------------------

  size_t frames() const;
  bool empty() const;
  void Clear();

  /// The tape as one JSON document (deterministic: same tape, same bytes).
  std::string Save() const;

  /// Parses a tape, replacing the current one. Rejects the whole document on
  /// any error (a partially-loaded tape never plays). Does not affect whether
  /// recording is active.
  bool Load(const std::string& json, TapeError* outError = nullptr,
            std::string* detail = nullptr);

  //--- replay --------------------------------------------------------------

  /// A LevelSeam over the current tape. Hand it to the World module to route
  /// every read-only query through the recording. The returned seam borrows the
  /// tape, so it is only valid while this recorder lives and the tape is not
  /// replaced (Load) or cleared.
  std::shared_ptr<LevelSeam> ReplaySeamHandle();

  /// Advances the replay one frame. False at the end of the tape.
  bool ReplayStep();
  /// Seeks to a frame index (0-based). False when out of range.
  bool ReplaySeek(size_t index);
  /// Rewinds to just before the first frame (an empty world).
  void ReplayRewind();
  /// True when no frame is current (before the first step, or past the last).
  bool ReplayAtEnd() const;
  /// The current frame's tick, or 0 when there is none.
  u64 ReplayTick() const;
  /// The current frame index, or -1 before the first step.
  i64 ReplayIndex() const;

 private:
  mutable std::mutex mutex_;
  Recording tape_;
  std::unique_ptr<Recorder> recorder_;
  std::shared_ptr<ReplaySeam> replay_;
  bool recording_ = false;
  bool overflowed_ = false;
};

}  // namespace Berry
