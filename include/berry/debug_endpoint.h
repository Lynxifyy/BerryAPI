#pragma once
// A loopback-only, token-protected metrics debug endpoint (improvement 91).
//
// Exposes the metrics registry over HTTP for a developer tool (a browser tab,
// curl), so a device build can be inspected without a debugger. It is
// deliberately hard to misuse:
//
//   * It binds 127.0.0.1 only, never a routable address, so nothing off the
//     device can reach it.
//   * A request must carry the token in an `X-Berry-Token` header (or `?token=`),
//     compared in constant time, so a local app that scans ports cannot read it
//     without the token.
//   * The response is a bounded JSON snapshot, so a hostile caller cannot make
//     it allocate without limit.

#include "berry/export.h"
#include "berry/metrics.h"
#include "berry/types.h"

#include <atomic>
#include <string>
#include <thread>

namespace Berry {

/// The endpoint configuration.
struct BERRY_API DebugEndpointConfig {
  std::string bindAddr = "127.0.0.1";   // must be a loopback address
  u16 port = 0;                          // 0 lets the kernel choose
  std::string token;                     // required; empty refuses to start
};

enum class DebugEndpointError : u32 {
  Ok = 0,
  NotLoopback,   // bindAddr is not a loopback address
  NoToken,       // token is empty
  BindFailed,
  AlreadyRunning,
};

const char* DebugEndpointErrorName(DebugEndpointError e);

/// Serves a metrics snapshot on loopback. Thread-safe start/stop.
class BERRY_API DebugEndpoint {
 public:
  DebugEndpoint();
  ~DebugEndpoint();

  DebugEndpoint(const DebugEndpoint&) = delete;
  DebugEndpoint& operator=(const DebugEndpoint&) = delete;

  /// Starts the server thread. Refuses a non-loopback bind or an empty token.
  DebugEndpointError Start(const DebugEndpointConfig& cfg);
  /// Stops the server and joins its thread. Idempotent.
  void Stop();
  bool running() const { return running_.load(); }
  /// The bound port (the kernel's choice when the config asked for 0).
  u16 port() const { return port_; }
  const std::string& lastError() const { return err_; }

  /// Handles one request line + token and returns the JSON body (or an error
  /// string). Pure, so the auth and routing are testable without a socket.
  static std::string HandleRequest(const std::string& request,
                                   const std::string& token,
                                   MetricsRegistry* reg);

  /// Constant-time string compare, so the token check does not leak its length
  /// or content through timing. Exposed for the test.
  static bool ConstantTimeEquals(const std::string& a, const std::string& b);

 private:
  void Run();

  std::atomic<bool> running_{false};
  int listenFd_ = -1;
  u16 port_ = 0;
  std::string token_;
  std::string err_;
  std::thread thread_;
};

}  // namespace Berry
