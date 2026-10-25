#pragma once
// Loopback replay server: play a replay back into the game's own client.
//
// This is playback option (A). The client already knows how to draw chunks,
// entities, skins and particles — it does that every frame from the packets it
// receives. So instead of writing a second renderer (option B: re-create
// entities and chunks by poking engine memory, which needs a signature per
// structure and breaks on every update), a replay is played back by putting the
// recorded packets back on the wire to the game's own network stack. The game
// draws everything itself, through its own code paths, and this library never
// writes a byte of game memory.
//
// The shape:
//
//   * the server binds a UDP socket on **loopback only** — `Start` refuses any
//     address that is not `127.0.0.1`/`::1`/`localhost`. That refusal is the
//     whole safety story: a replay can never be served to a remote peer, so the
//     recorded session can never leak off the device.
//   * it accepts one client (the game, joining `127.0.0.1:<port>`) with a small
//     framed handshake, then streams the recorded packets **in tick order**,
//     paced by the replay clock, starting from a keyframe so the client's first
//     world state is complete.
//   * it only ever sends to the address it received a join from. There is no
//     path that addresses any other host, so the server cannot be turned into a
//     relay even by a bug.
//   * seeking restarts the stream from the nearest keyframe at or before the
//     target tick, which is what a replay scrubber needs.
//
// Framing is deliberately small and is NOT RakNet: the game's own transport
// (RakNet, or the platform's) is a seam, and the device adapter hands the
// recorded packets to it. This class is the local feed and the pacing, which is
// the part that is testable on a host with no device.
//
// Wire frames (all big-endian lengths, fixed 4-byte magic):
//
//   client -> server  "BRJO"                       join, asking for the stream
//   server -> client  "BRAC" <u32 startTick>       accepted; the stream begins
//   server -> client  "BRPK" <u32 id> <u32 len> <len bytes>   one packet
//   server -> client  "BREN"                       the replay reached its end
//   client -> server  "BRBY"                       leaving
//   client -> server  "BRGO" <u32 tick>            seek to a tick (restart)
//
// An unknown or short frame is dropped and the client is left connected; a
// frame over the cap drops the client, so a peer cannot grow the buffer.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/net.h"
#include "berry/replay_format.h"
#include "berry/replay_world.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace Berry {

/// What the loopback server is allowed to accept. `host` must be a loopback
/// address; anything else is refused at `Start`.
struct BERRY_API ReplayServerConfig {
  std::string host = "127.0.0.1";
  /// The port to listen on. 0 asks the OS for a free one, which `port()`
  /// reports, so a test never collides with a real service.
  u16 port = 0;
  /// The most bytes in one frame from a client. A longer frame drops the
  /// client, so a peer cannot grow the buffer without bound.
  size_t maxFrameBytes = 64 * 1024;
  /// How many recorded packets are sent per replay tick. The game's own
  /// transport is the real bottleneck; this bounds one pump's work.
  size_t packetsPerTick = 256;
  /// The most datagrams one `Pump` will process before returning, so a busy
  /// client cannot pin the caller.
  size_t maxDatagramsPerPump = 64;
  /// The network protocol version this build speaks. A replay whose fingerprint
  /// carries a different version is refused by `Attach` (the recorded packet
  /// ids and layouts are a property of the protocol, so feeding another
  /// version's bytes to this client's parsers would be undefined). 0 disables
  /// the check — a host test that builds a synthetic replay has no build.
  u32 expectedProtocolVersion = 0;
  /// A probe the server calls to learn whether a LIVE multiplayer session is
  /// active. When it returns true, `Start` refuses and a running server stops
  /// streaming, so a recorded session's packets can never be fed into a live
  /// one. Empty means "never live" (a host test has no session), which is why
  /// the production wiring always installs a real probe.
  std::function<bool()> liveSessionActive;
};

/// Why the server refused to start (or to attach a replay).
enum class ReplayServerError : u32 {
  Ok = 0,
  NotLoopback,        // the configured host is not a loopback address
  BindFailed,         // the socket could not bind
  LiveSessionActive,  // a live multiplayer session is active; playback refused
  BadProtocolVersion, // the replay's protocol version is not the build's
  AlreadyRunning,
};

const char* ReplayServerErrorName(ReplayServerError e);

/// Plays a replay back to a loopback client. Thread-safe.
class BERRY_API ReplayServer {
 public:
  ReplayServer();
  ~ReplayServer();

  ReplayServer(const ReplayServer&) = delete;
  ReplayServer& operator=(const ReplayServer&) = delete;

  /// Binds the loopback socket. Refuses any non-loopback host (fail closed,
  /// leaving the server stopped). `port` of 0 picks a free port.
  ReplayServerError Start(const ReplayServerConfig& cfg);
  void Stop();
  bool running() const;
  /// The bound port, or 0 when stopped.
  u16 port() const;
  ReplayServerConfig config() const;

  /// Sets the replay the server streams. The file must outlive the server or
  /// the next `Attach`. A null or empty file leaves the server with nothing to
  /// send, and a joining client is told the stream ended.
  ///
  /// Refuses (returning `BadProtocolVersion`) a file whose fingerprint carries a
  /// protocol version other than `config().expectedProtocolVersion`, unless that
  /// expected version is 0. On a refusal the server keeps its previous replay, so
  /// a mismatched file never displaces a good one.
  ReplayServerError Attach(const ReplayFile* file);

  /// Processes pending client datagrams and sends the packets that are due.
  /// Returns the number of datagrams processed. Call it from the tick or a
  /// worker; it never blocks longer than `timeoutMs`.
  size_t Pump(int timeoutMs = 0);

  /// True when a client has joined and not left.
  bool clientConnected() const;
  /// How many packets have been sent since the client joined.
  u64 packetsSent() const;
  /// The tick the stream is currently at.
  u64 streamTick() const;
  /// True when the live-session guard is currently holding playback back: a
  /// live session is active, so the server is neither started nor streaming.
  /// A caller uses this to explain why nothing plays.
  bool liveBlocked() const;
  /// Restarts the stream for the connected client at the nearest keyframe at or
  /// before `tick`. False when no client is connected.
  bool Seek(u64 tick);
  /// Pauses the stream (the client stays connected). A paused server sends
  /// nothing until `Resume`.
  void Pause();
  void Resume();
  bool paused() const;

  /// The socket, so a test can send it a client datagram directly.
  UdpSocket& socket() { return sock_; }

 private:
  struct Client {
    bool connected = false;
    std::string host;
    u16 port = 0;
    u64 streamTick = 0;
    size_t sectionIdx = 0;
    size_t recordIdx = 0;
    bool sentEnd = false;
  };

  /// Sends one frame to the connected client. Caller holds `mutex_`.
  bool SendFrameLocked(const std::string& frame);
  /// Sends the packets due at the current stream tick. Caller holds `mutex_`.
  void StreamLocked();
  /// Moves the stream cursor to the section at or before `tick`. Caller holds
  /// `mutex_`.
  void RewindToLocked(u64 tick);
  /// Handles one received datagram. Caller holds `mutex_`.
  void HandleDatagramLocked(const std::string& data, const std::string& fromHost,
                            u16 fromPort);

  mutable std::mutex mutex_;
  ReplayServerConfig cfg_;
  UdpSocket sock_;
  bool running_ = false;
  bool paused_ = false;
  const ReplayFile* file_ = nullptr;
  /// The live-session probe, stored separately from `cfg_` so `liveBlocked()`
  /// answers correctly even when `Start` refused (which leaves `cfg_` alone).
  std::function<bool()> liveProbe_;
  Client client_;
  u64 packetsSent_ = 0;
};

}  // namespace Berry
