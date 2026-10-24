#pragma once
// Replay capture: turn this client's own live stream into a replay.
//
// `replay_format.h` is the container; this is what fills it. It sits on the
// same seam the world grid already reads and, once per tick, appends a section
// to a replay:
//
//   * a **keyframe** every `keyframeTicks` ticks: the full world state the seam
//     reports (level time plus every entity), the anchor a seek starts from;
//   * a **delta** on every other tick: only what changed since the previous
//     tick — entities that appeared, moved, changed metadata or left, a time or
//     weather change, and the local player's own position — so a replay costs a
//     fraction of a full snapshot per tick.
//
// It also takes the raw packet stream (`CapturePacket`): the receive path hands
// it the bytes and the tick they arrived on, and it stores them verbatim. That
// is the material a loopback replay server needs to drive the game's own
// parsers, and the one thing the metadata-only `PacketLog` deliberately cannot
// hold.
//
// Bounds and privacy, because this is the one place that keeps other players'
// data:
//
//   * a **size budget** (`maxBytes`) and a **ring** (`ringSeconds`): the
//     recorder evicts its oldest sections once either is exceeded, so "save the
//     last 60 seconds" and "never grow past N megabytes on a phone" are the
//     same mechanism. Eviction drops whole sections from the front, so the
//     oldest kept keyframe is still a valid anchor for everything after it.
//   * a **thermal guard**: a caller that reads the device's thermal level calls
//     `SetThermalLevel`; at the hot levels capture stops adding deltas rather
//     than competing with the game for memory and cycles.
//   * **nothing leaves the device.** The recorder writes to memory and, on
//     request, to a local file through the loader's file seam. There is no
//     upload path, and `Anonymize()` rewrites the names in place before an
//     export, so a replay can be shared without carrying who was in it.
//
// It is client-side and read-only with respect to the world: it snapshots what
// the seam already reported and copies the packets the client already received.
// It never writes to game memory and never sends anything.

#include "berry/export.h"
#include "berry/game.h"
#include "berry/packet.h"
#include "berry/replay_format.h"
#include "berry/types.h"
#include "berry/world_query.h"

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace Berry {

/// The payload codec for capture records. Every `Encode` produces the bytes a
/// record's `payload` holds; every `Decode` fails closed on a short or
/// malformed payload, so a corrupt replay cannot make a reader read past a
/// buffer.
namespace ReplayCodec {
BERRY_API std::string EncodeEntity(const EntitySnapshot& snap, bool full);
BERRY_API bool DecodeEntity(const std::string& payload, EntitySnapshot* out,
                            bool* outFull);
BERRY_API std::string EncodeTime(const LevelTime& time);
BERRY_API bool DecodeTime(const std::string& payload, LevelTime* out);
BERRY_API std::string EncodeEvent(const std::string& channel,
                                  const std::string& payload);
BERRY_API bool DecodeEvent(const std::string& payload, std::string* outChannel,
                           std::string* outPayload);
}  // namespace ReplayCodec

/// Why a capture was refused or throttled.
enum class CaptureState : u32 {
  Idle = 0,      // not recording
  Recording,     // capturing keyframes and deltas
  Throttled,     // recording, but the thermal guard is dropping deltas
  Stopped,       // hit a hard cap; nothing more is captured until Start
};

const char* CaptureStateName(CaptureState s);

/// Configuration for a capture session.
struct BERRY_API CaptureConfig {
  /// Ticks between keyframes. 20 ticks = one second at the sim rate. A shorter
  /// interval makes seeking cheaper and the file larger.
  u64 keyframeTicks = 20 * 5;
  /// Keep only this many seconds of the most recent capture (the ring). 0 keeps
  /// everything up to `maxBytes`.
  f32 ringSeconds = 0.0f;
  /// Uncompressed payload budget. The recorder evicts the oldest sections once
  /// it is exceeded, so a phone never grows a replay without bound.
  size_t maxBytes = 64u * 1024 * 1024;
  /// Capture the raw packet bytes (needed for a loopback replay server). Off
  /// makes a lighter, world-only replay that carries no packet data.
  bool capturePackets = true;
  /// Largest single packet payload accepted. A larger one is refused, so a
  /// hostile length cannot grow the replay.
  size_t maxPacketBytes = 1u * 1024 * 1024;
  /// Anonymize names on save: every recorded display name is replaced with a
  /// stable pseudonym, so a shared replay does not carry who was in it.
  bool anonymizeOnSave = false;
};

/// Captures the live stream into a replay. Thread-safe: the tick and the packet
/// hook may run on different threads (the packet feed is on the network
/// worker), so every entry point takes the lock.
class BERRY_API ReplayCapture {
 public:
  ReplayCapture();
  ~ReplayCapture();

  ReplayCapture(const ReplayCapture&) = delete;
  ReplayCapture& operator=(const ReplayCapture&) = delete;

  //--- configuration --------------------------------------------------------

  void SetConfig(const CaptureConfig& cfg);
  CaptureConfig config() const;

  /// The build fingerprint written into every replay this capture produces.
  void SetFingerprint(const ReplayFingerprint& fp);
  ReplayFingerprint fingerprint() const;

  //--- session --------------------------------------------------------------

  /// Begins a fresh capture, clearing any previous sections so a session always
  /// starts from a known state. False when already recording.
  bool Start();
  /// Ends the session. The captured replay is kept. False when not recording.
  bool Stop();
  CaptureState state() const;
  bool recording() const;

  /// The device thermal level, as the platform reports it (0 = nominal, higher
  /// = hotter). At `kThermalThrottleLevel` and above, capture keeps its
  /// keyframes but stops adding deltas, so a hot phone is not asked to compress
  /// every tick. Negative resets to nominal.
  static constexpr i32 kThermalThrottleLevel = 2;
  void SetThermalLevel(i32 level);
  i32 thermalLevel() const;

  //--- capture --------------------------------------------------------------

  /// Captures one tick: a keyframe on the keyframe interval, a delta otherwise.
  /// A no-op unless recording. `localPlayer` (when its id is nonzero) gets a
  /// per-tick position record, so the local player's own motion is in the
  /// replay even between keyframes. `diff` are the grid's world transitions,
  /// recorded as timeline markers.
  void CaptureTick(u64 tick, LevelSeam& seam,
                   const std::vector<WorldDiffEvent>& diff = {},
                   EntityId localPlayer = EntityId{});

  /// Captures one raw packet from the receive (or send) path. `payload` is the
  /// packet body verbatim. Returns false when not recording, the packet is over
  /// the size cap, or packets are disabled in the config.
  bool CapturePacket(PacketDir dir, u32 id, const std::string& payload,
                     u64 tick);

  /// Captures a timeline marker (a combat event, a script event).
  bool CaptureEvent(const std::string& channel, const std::string& payload,
                    u64 tick);

  //--- the replay -----------------------------------------------------------

  size_t sectionCount() const;
  /// The uncompressed payload bytes currently held.
  size_t bytes() const;
  /// How many sections the size or ring bound has evicted.
  u64 evicted() const;
  /// True when a section was refused for exceeding a cap (not an eviction).
  bool overflowed() const;
  /// The first and last tick held, or 0 when empty.
  u64 firstTick() const;
  u64 lastTick() const;
  bool empty() const;
  void Clear();

  /// A copy of the captured replay. Anonymized in place when
  /// `config().anonymizeOnSave` is set.
  ReplayFile Snapshot() const;

  /// Encodes the captured replay into the container format. `out` is appended
  /// to. Returns the error; an empty capture is still a valid (empty) file.
  ReplayError Save(std::string* out) const;

  /// Encodes only the trailing `ringSeconds` worth of sections (the "save the
  /// last 60 seconds" export). 0 writes everything held.
  ReplayError SaveRing(f32 ringSeconds, std::string* out) const;

  /// Replaces every recorded display name with a stable pseudonym ("Player1",
  /// "Player2", …), so an exported replay does not carry who was in it. Chat is
  /// not recorded by this layer, so a name is the only identity it holds.
  void Anonymize();

 private:
  /// Appends a section, evicting from the front while over a bound. Caller
  /// holds `mutex_`.
  bool PushLocked(const ReplaySection& s);
  /// Evicts oldest sections while over the byte budget or the ring window.
  /// Caller holds `mutex_`.
  void EvictLocked();
  /// The captured replay as a `ReplayFile`. Caller holds `mutex_`.
  ReplayFile SnapshotLocked() const;
  /// Rewrites every recorded display name in `f` to a pseudonym, numbering from
  /// `*counter`. The save path uses this on an export copy only.
  void AnonymizeFile(ReplayFile* f, size_t* counter) const;

  mutable std::mutex mutex_;
  CaptureConfig cfg_;
  ReplayFingerprint fp_;
  CaptureState state_ = CaptureState::Idle;
  i32 thermal_ = 0;
  u64 tick_ = 0;
  u64 lastKeyframeTick_ = 0;
  bool haveKeyframe_ = false;

  std::deque<ReplaySection> sections_;
  ReplaySection pending_;          // the delta being built for this tick
  bool building_ = false;
  u64 pendingStart_ = 0;
  size_t bytes_ = 0;
  u64 evicted_ = 0;
  bool overflowed_ = false;

  // Previous tick's entity state, for the delta diff. Bounded by the entity cap.
  std::vector<EntitySnapshot> lastEntities_;
  LevelTime lastTime_;
  bool haveTime_ = false;
  size_t anonCounter_ = 0;
};

}  // namespace Berry
