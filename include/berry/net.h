#pragma once
#include "berry/export.h"
#include "berry/types.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

namespace Berry {

//----------------------------------------------------------------------------
// Berry::Network module.
//
// Socket-based HTTP/1.1 client, RFC6455 WebSocket client, JSON-RPC 2.0 helper,
// and a small rate limiter. All blocking calls are intended for use from a
// worker thread (or the Scheduler); none touch the game render thread.
//----------------------------------------------------------------------------

class BERRY_API TcpSocket {
 public:
  TcpSocket();
  ~TcpSocket();
  void Close();

  bool Connect(const char* host, u16 port, int timeoutMs = 5000);
  ssize_t Send(const void* data, size_t len);
  ssize_t Recv(void* buf, size_t len, int timeoutMs = 5000);
  bool WaitReadable(int timeoutMs);
  int fd() const { return fd_; }
  const std::string& LastError() const { return err_; }

 private:
  int fd_ = -1;
  std::string err_;
};

//----------------------------------------------------------------------------
// HTTP client (HTTP/1.1, no external deps)
//----------------------------------------------------------------------------

struct BERRY_API HttpResponse {
  int status = 0;
  std::string reason;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;

  const char* Header(const char* name) const;
  bool ok() const { return status >= 200 && status < 300; }
};

class BERRY_API HttpRequest {
 public:
  HttpRequest(const char* url);
  HttpRequest& Method(const char* m) { method_ = m; return *this; }
  HttpRequest& Header(const char* name, const char* value);
  HttpRequest& Body(const std::string& data) { body_ = data; return *this; }
  HttpRequest& JsonBody(const char* json) { body_ = json; Header("Content-Type", "application/json"); return *this; }
  HttpRequest& Timeout(int ms) { timeoutMs_ = ms; return *this; }

  HttpResponse Do();

 private:
  std::string url_, method_ = "GET", body_, host_, path_;
  u16 port_ = 80;
  bool tls_ = false;
  int timeoutMs_ = 8000;
  std::vector<std::pair<std::string, std::string>> headers_;
};

//----------------------------------------------------------------------------
// WebSocket (RFC6455), client side
//----------------------------------------------------------------------------

enum class WsOpcode : u8 {
  Continuation = 0x0, Text = 0x1, Binary = 0x2,
  Close = 0x8, Ping = 0x9, Pong = 0xA
};

struct BERRY_API WsMessage {
  WsOpcode opcode = WsOpcode::Text;
  std::vector<u8> payload;
  bool fin = true;
  std::string text() const { return std::string(payload.begin(), payload.end()); }
};

using WsOpenCb = std::function<void()>;
using WsMessageCb = std::function<void(const WsMessage&)>;
using WsCloseCb = std::function<void(int code, const std::string& reason)>;

class BERRY_API WebSocket {
 public:
  WebSocket() = default;
  ~WebSocket();

  bool Connect(const char* url, int timeoutMs = 8000);
  void Close(int code = 1000, const char* reason = "bye");
  bool IsOpen() const;
  bool SendText(const char* text);
  bool SendBinary(const void* data, size_t len);
  bool SendPing();

  // Polls for incoming frames; call from the scheduler.
  void Pump();

  void OnOpen(WsOpenCb cb) { onOpen_ = std::move(cb); }
  void OnMessage(WsMessageCb cb) { onMessage_ = std::move(cb); }
  void OnClose(WsCloseCb cb) { onClose_ = std::move(cb); }

 private:
  bool SendFrame(WsOpcode op, const void* payload, size_t len);
  void HandleCloseFrame(const WsMessage& frame);

  std::unique_ptr<TcpSocket> sock_;
  std::mutex sendMutex_;
  std::string err_;
  bool open_ = false;
  u64 frameSeq_ = 0;

  WsOpenCb onOpen_;
  WsMessageCb onMessage_;
  WsCloseCb onClose_;
};

//----------------------------------------------------------------------------
// JSON-RPC 2.0 helper (minimal JSON parse/build — enough for method/params/id)
//----------------------------------------------------------------------------

struct BERRY_API JsonRpcRequest {
  std::string method;
  std::string params;   // raw JSON params
  std::string id;
  bool hasId = false;
};

class BERRY_API JsonRpc {
 public:
  // Parses a JSON-RPC 2.0 request frame. Returns false if not a valid request.
  static bool ParseRequest(const char* json, JsonRpcRequest* out);
  static std::string MakeResponse(const char* id, const char* resultJson);
  static std::string MakeError(const char* id, int code, const char* message);
  static std::string MakeNotification(const char* method, const char* paramsJson);
};

//----------------------------------------------------------------------------
// Rate limiter
//----------------------------------------------------------------------------

class BERRY_API RateLimiter {
 public:
  void Reset(size_t maxBurst, f32 perSecond);
  /// Returns true if a call would be allowed now.
  bool Allow();
  f32 Usage() const;
  void SetConstant(f32 perSecond) { perSecond_ = perSecond; }

 private:
  mutable std::mutex mutex_;
  f64 tokens_ = 0;
  f64 lastRefillUs_ = 0;
  f64 maxBurst_ = 0;
  f64 perSecond_ = 0;
};

}  // namespace Berry