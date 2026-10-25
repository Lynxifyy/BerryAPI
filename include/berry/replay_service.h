#pragma once
// Replay mod service: one object the engine owns, bundling the replay pieces.
//
// The parts are separately testable and separately useful — `ReplayCapture`
// fills a container, `ReplayWorld` reconstructs a tick, `ReplayReview` draws
// one, `ReplayServer` feeds a tick back to the game's own client. This is the
// object the running client actually holds, so a script (through the
// `ReplayMod` bindings) and the engine's tick/frame drivers reach all of them
// through one owned instance rather than each caller owning its own.
//
// It owns the loaded replay too: `Load` decodes bytes into an owned
// `ReplayFile` and builds the `ReplayWorld` over it, so the world's borrowed
// pointer never outlives the file (the reason the world takes a pointer at all
// is so the host tests can build one over a stack `ReplayFile`).
//
// It is the production caller of every replay piece: the engine drives `Tick`
// from the World module's tick and `Frame` from the frame hook, and the
// `ReplayMod` bindings drive the rest. Thread-safe.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/game.h"
#include "berry/render.h"
#include "berry/replay_capture.h"
#include "berry/replay_format.h"
#include "berry/replay_review.h"
#include "berry/replay_server.h"
#include "berry/replay_world.h"
#include "berry/world_query.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Berry {

/// The replay mod, owned by the engine. Thread-safe.
class BERRY_API ReplayService {
 public:
  ReplayService();
  ~ReplayService();

  ReplayService(const ReplayService&) = delete;
  ReplayService& operator=(const ReplayService&) = delete;

  //--- capture --------------------------------------------------------------

  ReplayCapture& capture() { return capture_; }
  const ReplayCapture& capture() const { return capture_; }

  //--- loaded replay --------------------------------------------------------

  /// Decodes `bytes` into an owned replay and points the review at it. On a
  /// refusal the current replay is left untouched and `*err` (optional) says
  /// why. Returns false on a malformed replay.
  bool Load(const std::string& bytes, ReplayError* err = nullptr);
  /// Encodes the loaded replay back to bytes. Appends to `*out`. Returns
  /// `BadMagic`-free success even for an empty replay.
  ReplayError Save(std::string* out) const;
  /// True when a replay is loaded.
  bool loaded() const;
  /// The loaded replay, or null.
  const ReplayFile* file() const;
  /// The reconstruction over the loaded replay, or null.
  ReplayWorld* world();
  /// Drops the loaded replay and stops any playback.
  void Unload();

  //--- local review ---------------------------------------------------------

  ReplayReview& review() { return review_; }

  //--- loopback playback ----------------------------------------------------

  ReplayServer& server() { return server_; }

  //--- drivers (called by the engine) ---------------------------------------

  /// The local player's id, so a capture records the local player's own motion
  /// between keyframes. 0 means unknown (no per-tick local record).
  void SetLocalPlayer(EntityId id);
  EntityId localPlayer() const;

  /// Installs the server's live-session probe: `probe` answers whether a LIVE
  /// multiplayer session is active. While it does, the loopback server neither
  /// starts nor streams, so a recorded packet can never reach a live session.
  /// Empty (the default) means "never live", which is right for a host test.
  void SetLiveSessionProbe(std::function<bool()> probe);

  /// The network protocol version this build speaks. `Attach` refuses a loaded
  /// replay whose fingerprint carries a different one, so a wrong-version file
  /// never reaches the client's parsers. 0 disables the check (no build).
  void SetExpectedProtocolVersion(u32 version);
  u32 expectedProtocolVersion() const;

  /// Fills `cfg`'s guard fields from this service's state — the expected
  /// protocol version and the live-session probe — so a caller that starts the
  /// server (a script binding, the engine) gets the guards without having to
  /// remember them. Every production `Start` goes through this.
  void ApplyServerGuards(ReplayServerConfig* cfg) const;

  /// Captures one tick from the seam while a capture session is recording.
  /// Driven by the World module's tick, next to the session recorder.
  void Tick(u64 tick, LevelSeam& seam, const std::vector<WorldDiffEvent>& diff);

  /// Pumps the loopback playback server once. Driven by the Engine's frame so
  /// playback works whether or not a script is loaded. Never blocks.
  size_t PumpServer();

  /// Advances the local review by `dtSeconds`. Driven by the frame clock; the
  /// overlay owns no clock of its own, so a paused client does not advance.
  void AdvanceReview(f32 dtSeconds);

  /// Draws the review through `r` when it is active. Returns how many entities
  /// were drawn. Called on the render thread, after the other overlays.
  size_t PresentReview(const CameraState& cam, Renderer& r, const Vec2& screen);

  //--- introspection --------------------------------------------------------

  /// A one-line summary for a status line: capture state, held sections, bytes,
  /// and whether a replay is loaded and a client is connected.
  std::string status() const;

 private:
  ReplayCapture capture_;
  mutable std::mutex mutex_;
  std::unique_ptr<ReplayFile> file_;
  std::unique_ptr<ReplayWorld> world_;
  ReplayReview review_;
  ReplayServer server_;
  std::atomic<u64> localPlayerRaw_{0};
  /// The protocol version `Attach` enforces on a loaded replay. 0 = no check.
  std::atomic<u32> expectedProtocolVersion_{0};
  /// The live-session probe, guarded by `probeMutex_` because it is installed
  /// from the attach path and read from the frame thread.
  mutable std::mutex probeMutex_;
  std::function<bool()> liveProbe_;
};

}  // namespace Berry
