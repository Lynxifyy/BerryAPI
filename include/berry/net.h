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
// UDP socket (IPv4)
//
// The datagram transport under the LAN cosmetic sync channel. A bound socket
// sends to and receives from peers; datagrams are best-effort and unordered, so
// the layer above frames and reassembles. This class only moves bytes.
//----------------------------------------------------------------------------

class BERRY_API UdpSocket {
 public:
  UdpSocket();
  ~UdpSocket();
  void Close();

  /// Binds to `port` on `bindAddr` (default: any address). A port of 0 lets the
  /// kernel choose one, which `localPort()` then reports. `broadcast` enables
  /// SO_BROADCAST for sending to a subnet broadcast address.
  bool Bind(u16 port = 0, const char* bindAddr = "0.0.0.0", bool broadcast = false);

  /// Joins an IPv4 multicast group on `ifaceAddr`. Enables multicast loopback
  /// so two sockets on one host see each other (and a host test runs on
  /// loopback). Returns false (and records the error) on a bad group or a
  /// failed join; the socket stays usable for unicast.
  bool JoinMulticast(const char* group, const char* ifaceAddr = "127.0.0.1");
  /// Sets the outgoing interface for multicast sends (default: any). Required on
  /// some hosts before a send to a group is delivered to a local member.
  bool SetMulticastInterface(const char* ifaceAddr);

  /// Sends one datagram. Non-blocking: returns false immediately on error.
  bool SendTo(const void* data, size_t len, const char* host, u16 port);

  /// Receives one datagram, waiting up to `timeoutMs`. `fromHost` (optional)
  /// receives the sender's numeric address, `fromPort` its port. Returns the
  /// byte count, 0 on timeout, or -1 on error. A datagram larger than `len` is
  /// truncated to `len` and reported at that length (the framing layer sizes its
  /// buffer to the path MTU, so a peer cannot overrun it).
  ssize_t RecvFrom(void* buf, size_t len, std::string* fromHost, u16* fromPort,
                   int timeoutMs = 100);

  /// The bound port, or 0 when unbound.
  u16 localPort() const;
  bool valid() const { return fd_ >= 0; }
  const std::string& LastError() const { return err_; }
  int fd() const { return fd_; }

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
  /// Set when the body hit the request's byte cap and the rest was discarded,
  /// so a caller can tell a complete response from a clipped one.
  bool truncated = false;

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
  /// Caps how many response-body bytes are buffered. A hostile or broken server
  /// that streams without end would otherwise make the caller buffer without
  /// limit; the read stops (and the response is marked truncated) at the cap.
  HttpRequest& MaxBodyBytes(size_t n) { maxBodyBytes_ = n; return *this; }
  /// True when the URL named a scheme this client cannot serve (https/wss,
  /// since TLS is not implemented). Do() then refuses instead of downgrading.
  bool badScheme() const { return badScheme_; }

  HttpResponse Do();

 private:
  std::string url_, method_ = "GET", body_, host_, path_;
  u16 port_ = 80;
  bool badScheme_ = false;
  /// Default 4 MiB: comfortably larger than any JSON/config response, small
  /// enough that a streaming server cannot exhaust the worker's memory.
  size_t maxBodyBytes_ = 4u * 1024 * 1024;
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

// Result of parsing an in-memory RFC6455 frame (see ParseWsFrame).
enum class WsFrameStatus : u8 {
  Ok = 0,       // header (and full payload) present; header/mask valid
  NeedMore,     // buffer too short: wait for more bytes
  Oversized,    // payload length exceeds the safety cap — fail closed
  Invalid       // reserved bits, bad opcode, oversized/rsv control frame
};

// Parsed frame header + (unmasked) payload from `data`. Server->client frames
// are unmasked per RFC6455; a present mask is still accepted and applied so a
// hostile/faulty peer cannot smuggle a corrupt stream.
struct BERRY_API WsFrameView {
  bool fin = true;
  u8 opcode = 0;
  bool masked = false;
  u64 payloadLen = 0;
  const u8* payload = nullptr;     // valid when status == Ok
  size_t frameLen = 0;             // total bytes consumed (header + payload)
};

// Parses a WebSocket frame from an in-memory buffer. Fail closed: returns
// Invalid for reserved opcodes, RSV bits, or malformed control frames
// (control frames must be <=125 bytes and FIN). `maxPayload` guards against
// oversized frame bombs.
BERRY_API WsFrameStatus ParseWsFrame(const u8* data, size_t len,
                                     WsFrameView* out,
                                     u64 maxPayload = 16u * 1024 * 1024);

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