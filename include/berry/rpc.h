#pragma once
// Local RPC & REPL: a loopback JSON-RPC 2.0 endpoint so tooling (a VS Code
// extension, a dashboard, an OBS overlay) can talk to a running client.
//
// The design splits cleanly in two, and that split is the point:
//
//   * `RpcServer` is the dispatcher: it maps a method name to a handler and
//     turns a request frame into a response frame. It has no sockets in it, so
//     the whole protocol is testable by calling `HandleFrame` directly.
//   * `RpcServer::Start` wraps that dispatcher in a loopback TCP listener. The
//     listener is the only part that touches the network, and it only ever
//     accepts connections from 127.0.0.1.
//
// This is a local debugging surface, so the security posture is deliberately
// narrow:
//
//   * it binds loopback only; a request to bind any other address is refused
//   * it is opt-in: nothing starts listening until `Start` is called
//   * frames are length-capped, methods are count-capped, and a per-connection
//     rate limit drops a flood rather than serving it
//   * a malformed frame is answered with a JSON-RPC parse error, never treated
//     as a partial request
//   * an unknown method is a method-not-found error, never a silent success
//
// The transport is a newline-delimited JSON-RPC stream over TCP. A WebSocket
// server can wrap the same `HandleFrame`, so the protocol logic stays in one
// place; the dispatcher does not know or care which transport carries it.

#include "berry/export.h"
#include "berry/types.h"

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Berry {

/// A handler's outcome. `result` is raw JSON (so a handler can return an object
/// or an array, not just a string). `ok == false` becomes a JSON-RPC error.
struct BERRY_API RpcResult {
  bool ok = true;
  std::string result = "null";   // raw JSON
  int errorCode = -32000;        // JSON-RPC server error
  std::string errorMessage;

  static RpcResult Ok(std::string resultJson) {
    RpcResult r;
    r.result = std::move(resultJson);
    return r;
  }
  static RpcResult Err(int code, std::string message) {
    RpcResult r;
    r.ok = false;
    r.errorCode = code;
    r.errorMessage = std::move(message);
    return r;
  }
};

/// A method handler: given the request's raw params JSON, produce a result.
using RpcHandler = std::function<RpcResult(const std::string& paramsJson)>;

//----------------------------------------------------------------------------
// JSON-RPC error codes (the standard set plus this server's own).
//----------------------------------------------------------------------------

namespace rpc_error {
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;
constexpr int kRateLimited = -32000;
}  // namespace rpc_error

/// The dispatcher. Thread-safe: handlers may be registered while a connection
/// is being served.
class BERRY_API RpcServer {
 public:
  /// The most bytes in one request frame. A longer frame is rejected and the
  /// connection is dropped, rather than buffering without bound.
  static constexpr size_t kMaxFrameBytes = 64 * 1024;
  /// The most methods that may be registered.
  static constexpr size_t kMaxMethods = 256;
  /// Requests per second a single connection may make before it is throttled.
  static constexpr f32 kRequestsPerSecond = 200.0f;

  RpcServer();
  ~RpcServer();

  /// Registers a method. Replaces an existing handler of the same name.
  /// Refused (returns false) when the registry is full or the name is empty.
  bool Register(const std::string& method, RpcHandler handler);
  bool Unregister(const std::string& method);
  bool Has(const std::string& method) const;
  std::vector<std::string> Methods() const;

  /// Handles one request frame. Writes the response frame into `*response` and
  /// returns true when a response should be sent; returns false for a
  /// notification (a request with no `id`), which gets no reply per the spec.
  ///
  /// This is the whole protocol, with no I/O. It never throws.
  bool HandleFrame(const std::string& frame, std::string* response);

  /// Starts a loopback listener. `host` must be a loopback address
  /// ("127.0.0.1" or "localhost"); anything else is refused. `port` of 0 asks
  /// the OS for a free port. Returns the bound port, or -1 on failure.
  int Start(u16 port = 0, const std::string& host = "127.0.0.1");
  void Stop();
  bool running() const { return running_; }
  /// The bound port, or 0 when not listening.
  u16 port() const;

  /// How many frames have been handled, for a status method.
  u64 framesHandled() const;

 private:
  void ServeLoop();
  void ServeConnection(int fd);

  mutable std::mutex mutex_;
  std::map<std::string, RpcHandler> handlers_;
  std::thread thread_;
  int listenFd_ = -1;
  u16 port_ = 0;
  bool running_ = false;
  u64 framesHandled_ = 0;
};

//----------------------------------------------------------------------------
// REPL — a one-line front end over the dispatcher.
//----------------------------------------------------------------------------

/// Turns a line like `world.query 20` into a JSON-RPC call and returns the
/// result as a short human-readable string. This is what a debug console or a
/// terminal session drives. It is a thin wrapper: every call goes through
/// `RpcServer::HandleFrame`, so the REPL cannot reach anything the RPC surface
/// does not already expose.
class BERRY_API RpcRepl {
 public:
  explicit RpcRepl(RpcServer* server) : server_(server) {}

  /// Evaluates one line. Returns a printable result or an error string. A blank
  /// line returns an empty string.
  std::string Eval(const std::string& line);

  /// The commands the REPL understands: every registered RPC method, plus the
  /// REPL's own `help` and `methods`.
  std::vector<std::string> Commands() const;

  /// Splits a REPL line into a method and its raw-JSON params array. Exposed so
  /// a caller can show the parsed form; returns false for an empty line.
  static bool ParseLine(const std::string& line, std::string* method,
                        std::string* paramsJson);

 private:
  RpcServer* server_ = nullptr;
};

}  // namespace Berry
