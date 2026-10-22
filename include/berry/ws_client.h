#pragma once
// BerryAPI script-facing WebSocket client.
//
// `Berry::Network` already implements an RFC6455 client (`WebSocket`). This
// service is the script-facing wrapper: it owns a small pool of connections,
// each addressed by an integer handle, and turns the frame pump into a
// script-pollable inbox. A script calls `ws.connect`, `ws.send`, `ws.pump` and
// `ws.poll`; it never sees a socket.
//
// Two deliberate properties:
//
//   * The egress gate is the same one `network.get` uses. A connect is refused
//     unless the URL passes the URL policy AND the host is covered by the
//     script's network grant, so a WebSocket cannot become a way around the
//     SSRF rules the HTTP path enforces. `wss://` fails closed (TLS is not
//     implemented) rather than downgrading to plaintext.
//   * Nothing here runs on the render thread. `Pump` performs blocking reads
//     with a short timeout and is meant for the script worker, exactly like
//     the blocking HTTP call.
//
// Messages are drained in order, so a script that polls sees exactly the
// frames the peer sent, in order, and never a partial one.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/url.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {

class WebSocket;

/// One received frame, as the script sees it. `opcode` is the RFC6455 opcode
/// name ("text", "binary", "close", "ping", "pong").
struct BERRY_API WsClientMessage {
  std::string opcode;
  std::string data;
  bool binary = false;
};

/// Why a WebSocket call was refused. Every value except Ok leaves state
/// unchanged.
enum class WsClientError : u32 {
  Ok = 0,
  BadHandle,      // unknown or closed handle
  BadUrl,         // not a ws:// URL the policy accepts
  NotGranted,     // host outside the network grant
  ConnectFailed,  // the peer refused, or the handshake failed
  SendFailed,
  Closed,
  TooManySockets, // the pool is full
  NotOpen,
};

const char* WsClientErrorName(WsClientError e);

/// Owns the script-visible WebSocket handles. Thread-confined to the script
/// worker: every method is called from the thread that owns the connections,
/// so the sockets are never touched concurrently. The internal mutex only
/// guards the pool table against the close-from-anywhere path.
class BERRY_API WsClientPool {
 public:
  /// The most sockets one pool will hold, so a script cannot exhaust file
  /// descriptors by opening connections in a loop.
  static constexpr size_t kMaxSockets = 16;
  /// The most frames one handle buffers before the oldest is dropped, so a peer
  /// that floods cannot grow the inbox without bound.
  static constexpr size_t kMaxInbox = 256;

  WsClientPool();
  ~WsClientPool();

  WsClientPool(const WsClientPool&) = delete;
  WsClientPool& operator=(const WsClientPool&) = delete;

  /// Opens `url` and returns a positive handle, or 0 with `err` set. The URL
  /// must pass `policy` and its host must be covered by `granted`.
  u64 Connect(const std::string& url, const CapabilitySet& granted,
              const UrlPolicy& policy, int timeoutMs, WsClientError* err);

  /// Sends a text frame. False (and `err`) when the handle is unknown/closed.
  bool SendText(u64 handle, const std::string& text, WsClientError* err);
  /// Sends a binary frame.
  bool SendBinary(u64 handle, const void* data, size_t len, WsClientError* err);

  /// Reads any pending frames for every open handle and appends them to the
  /// inboxes. Called by a script (on the script worker). Returns how many
  /// frames were read across all handles.
  size_t Pump();

  /// Pops the oldest buffered frame for `handle`. False when the inbox is
  /// empty (or the handle is unknown).
  bool Poll(u64 handle, WsClientMessage* out);

  /// The number of frames waiting for `handle`.
  size_t Pending(u64 handle) const;

  /// Closes and forgets a handle. Idempotent.
  bool Close(u64 handle);

  /// Closes every handle (used on script unload).
  void CloseAll();

  bool IsOpen(u64 handle) const;
  size_t Count() const;

  /// The URL a handle was opened with, for diagnostics. Empty when unknown.
  std::string Url(u64 handle) const;

  /// A short, last-error string for a handle (transport detail).
  std::string LastError(u64 handle) const;

 private:
  struct Conn {
    u64 handle = 0;
    std::string url;
    std::string host;
    std::unique_ptr<WebSocket> sock;
    std::deque<WsClientMessage> inbox;
    /// Written by the script worker and by the close callback (which may fire
    /// on the detach thread), so it is atomic rather than a plain bool.
    std::atomic<bool> open{false};
    std::string error;
  };

  /// Looks a handle up, returning a shared reference so a concurrent CloseAll
  /// cannot free the connection under a caller that is still using it.
  std::shared_ptr<Conn> Find(u64 handle) const;

  mutable std::mutex mutex_;
  std::unordered_map<u64, std::shared_ptr<Conn>> conns_;
  u64 nextHandle_ = 1;
};

}  // namespace Berry