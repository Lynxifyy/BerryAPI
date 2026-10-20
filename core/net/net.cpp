#include "berry/net.h"
#include "berry/logger.h"
#include "berry/event.h"
#include "berry/version.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <sstream>

namespace Berry {

//----------------------------------------------------------------------------
// TcpSocket
//----------------------------------------------------------------------------

TcpSocket::TcpSocket() = default;
TcpSocket::~TcpSocket() { Close(); }

void TcpSocket::Close() {
  if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

bool TcpSocket::Connect(const char* host, u16 port, int timeoutMs) {
  Close();
  timeval tv{};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  char portStr[16];
  snprintf(portStr, sizeof portStr, "%u", port);
  int rc = getaddrinfo(host, portStr, &hints, &res);
  if (rc != 0) {
    err_ = "getaddrinfo: " + std::string(gai_strerror(rc));
    return false;
  }

  int lastErr = 0;
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    int s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s < 0) { lastErr = errno; continue; }
    int flags = ::fcntl(s, F_GETFL, 0);
    ::fcntl(s, F_SETFL, flags | O_NONBLOCK);

    int c = ::connect(s, ai->ai_addr, ai->ai_addrlen);
    if (c < 0 && errno == EINPROGRESS) {
      fd_set wset;
      FD_ZERO(&wset);
      FD_SET(s, &wset);
      int sel = ::select(s + 1, nullptr, &wset, nullptr, &tv);
      if (sel > 0) {
        int soerr = 0;
        socklen_t slen = sizeof soerr;
        getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &slen);
        if (soerr == 0) c = 0;
        else lastErr = soerr;
      } else if (sel == 0) {
        lastErr = ETIMEDOUT;
      } else {
        lastErr = errno;
      }
    } else if (c < 0) {
      lastErr = errno;
    }

    ::fcntl(s, F_SETFL, flags);   // restore blocking
    if (c == 0) {
      fd_ = s;
      freeaddrinfo(res);
      return true;
    }
    ::close(s);
  }
  freeaddrinfo(res);
  err_ = "connect failed: " + std::string(strerror(lastErr));
  return false;
}

ssize_t TcpSocket::Send(const void* data, size_t len) {
  if (fd_ < 0) return -1;
  return ::send(fd_, data, len, MSG_NOSIGNAL);
}

ssize_t TcpSocket::Recv(void* buf, size_t len, int timeoutMs) {
  if (fd_ < 0) return -1;
  if (!WaitReadable(timeoutMs)) return -2;   // timeout
  return ::recv(fd_, buf, len, 0);
}

bool TcpSocket::WaitReadable(int timeoutMs) {
  if (fd_ < 0) return false;
  fd_set rset;
  FD_ZERO(&rset);
  FD_SET(fd_, &rset);
  timeval tv{};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  int rc = ::select(fd_ + 1, &rset, nullptr, nullptr, &tv);
  return rc > 0;
}

//----------------------------------------------------------------------------
// HTTP
//----------------------------------------------------------------------------

const char* HttpResponse::Header(const char* name) const {
  for (auto& h : headers) {
    if (strcasecmp(h.first.c_str(), name) == 0) return h.second.c_str();
  }
  return nullptr;
}

HttpRequest::HttpRequest(const char* url) : url_(url) {
  // parse "http://host:port/path"
  std::string u = url;
  std::string rest = u;
  if (rest.rfind("http://", 0) == 0) {
    rest = rest.substr(7);
  } else if (rest.rfind("https://", 0) == 0) {
    tls_ = true;
    port_ = 443;
    rest = rest.substr(8);
  }
  size_t slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  path_ = slash == std::string::npos ? "/" : rest.substr(slash);
  size_t colon = authority.rfind(':');
  if (colon != std::string::npos && authority.find(']') == std::string::npos) {
    host_ = authority.substr(0, colon);
    port_ = (u16)atoi(authority.substr(colon + 1).c_str());
  } else {
    host_ = authority;
  }
}

HttpRequest& HttpRequest::Header(const char* name, const char* value) {
  headers_.emplace_back(name, value);
  return *this;
}

HttpResponse HttpRequest::Do() {
  HttpResponse resp;
  TcpSocket sock;
  if (!sock.Connect(host_.c_str(), port_, timeoutMs_)) {
    resp.status = 0;
    resp.reason = "connect error";
    return resp;
  }

  std::string req = method_ + " " + path_ + " HTTP/1.1\r\n"
      "Host: " + host_ + "\r\n"
      "User-Agent: BerryAPI/" BERRY_VERSION_STRING "\r\n"
      "Connection: close\r\n";
  if (!body_.empty()) {
    char lenBuf[32];
    snprintf(lenBuf, sizeof lenBuf, "%zu", body_.size());
    req += std::string("Content-Length: ") + lenBuf + "\r\n";
  }
  for (auto& h : headers_) req += h.first + ": " + h.second + "\r\n";
  req += "\r\n";
  req += body_;

  if (sock.Send(req.data(), req.size()) != (ssize_t)req.size()) {
    resp.status = 0;
    resp.reason = "send failed";
    return resp;
  }

  char buf[8192];
  std::string raw;
  for (;;) {
    ssize_t n = sock.Recv(buf, sizeof buf, timeoutMs_);
    if (n < 0) break;
    raw.append(buf, (size_t)n);
    if (n == 0) break;
  }

  size_t hEnd = raw.find("\r\n\r\n");
  if (hEnd == std::string::npos) {
    resp.status = 0;
    resp.reason = "no HTTP headers";
    return resp;
  }

  std::istringstream hstream(raw.substr(0, hEnd));
  std::string line;
  if (std::getline(hstream, line)) {
    // "HTTP/1.1 200 OK"
    if (line.size() >= 12) {
      resp.status = atoi(line.c_str() + 9);
      size_t sp = line.find(' ', 9);
      if (sp != std::string::npos) resp.reason = line.substr(sp + 1);
    }
  }
  while (std::getline(hstream, line)) {
    if (line.empty()) continue;
    if (line.back() == '\r') line.pop_back();
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string v = line.substr(colon + 1);
      while (!v.empty() && v.front() == ' ') v.erase(v.begin());
      resp.headers.emplace_back(line.substr(0, colon), v);
    }
  }
  resp.body = raw.substr(hEnd + 4);
  return resp;
}

//----------------------------------------------------------------------------
// WebSocket (RFC6455)
//----------------------------------------------------------------------------

WebSocket::~WebSocket() { Close(); }

static std::string WsBase64(const unsigned char* data, size_t len);

bool WebSocket::Connect(const char* url, int timeoutMs) {
  std::string u = url;
  std::string rest = u;
  bool tls = false;
  if (rest.rfind("ws://", 0) == 0) {
    rest = rest.substr(5);
  } else if (rest.rfind("wss://", 0) == 0) {
    tls = true;   // NOTE: TLS not implemented; plaintext only for now
    rest = rest.substr(6);
  } else {
    err_ = "bad ws url";
    return false;
  }
  size_t slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  std::string path = slash == std::string::npos ? "/" : rest.substr(slash);

  std::string host = authority;
  u16 port = tls ? 443 : 80;
  size_t colon = authority.rfind(':');
  if (colon != std::string::npos) {
    host = authority.substr(0, colon);
    port = (u16)atoi(authority.substr(colon + 1).c_str());
  }

  sock_.reset(new TcpSocket());
  if (!sock_->Connect(host.c_str(), port, timeoutMs)) {
    err_ = sock_->LastError();
    return false;
  }

  // Random-ish key (16 bytes base64). Deterministic is fine for private use,
  // but we derive from a counter for a fresh handshake each time.
  unsigned char keyBytes[16];
  for (int i = 0; i < 16; ++i) keyBytes[i] = (unsigned char)((frameSeq_ * 31 + i * 7) & 0xff);
  std::string key = WsBase64(keyBytes, 16);

  std::string req =
      "GET " + path + " HTTP/1.1\r\n"
      "Host: " + host + "\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: " + key + "\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "User-Agent: BerryAPI\r\n\r\n";
  if (sock_->Send(req.data(), req.size()) != (ssize_t)req.size()) return false;

  // Read handshake response.
  char buf[4096];
  std::string raw;
  while (raw.find("\r\n\r\n") == std::string::npos) {
    ssize_t n = sock_->Recv(buf, sizeof buf, timeoutMs);
    if (n <= 0) {
      err_ = "handshake timeout";
      return false;
    }
    raw.append(buf, (size_t)n);
    if (raw.size() > 65536) return false;
  }
  // Verify the 101 Switching Protocols upgrade response.
  bool upgraded = raw.rfind("HTTP/1.1 101", 0) == 0 ||
                  raw.find(" 101 ") != std::string::npos;
  if (!upgraded) {
    err_ = "not upgraded";
    return false;
  }
  open_ = true;
  if (onOpen_) onOpen_();
  return true;
}

void WebSocket::Close(int code, const char* reason) {
  if (!open_) return;
  open_ = false;
  unsigned char frame[4];
  frame[0] = 0x88;
  frame[1] = 0x02;
  frame[2] = (unsigned char)(code >> 8);
  frame[3] = (unsigned char)(code & 0xff);
  if (sock_) sock_->Send(frame, sizeof frame);
  if (onClose_) onClose_(code, reason ? reason : "");
  if (sock_) { sock_->Close(); sock_.reset(); }
}

bool WebSocket::IsOpen() const { return open_; }

bool WebSocket::SendText(const char* text) {
  return SendFrame(WsOpcode::Text, text, strlen(text));
}
bool WebSocket::SendBinary(const void* data, size_t len) {
  return SendFrame(WsOpcode::Binary, data, len);
}
bool WebSocket::SendPing() {
  return SendFrame(WsOpcode::Ping, nullptr, 0);
}

bool WebSocket::SendFrame(WsOpcode op, const void* payload, size_t len) {
  if (!open_ || !sock_) return false;
  std::vector<u8> frame;
  frame.push_back(0x80 | (u8)op);   // FIN + opcode
  size_t n = len;
  if (n < 126) {
    frame.push_back((u8)(0x80 | n));  // masked
  } else if (n < 65536) {
    frame.push_back((u8)(0x80 | 126)); frame.push_back((u8)(n >> 8)); frame.push_back((u8)(n & 0xff));
  } else {
    frame.push_back((u8)(0x80 | 127));
    for (int i = 7; i >= 0; --i) frame.push_back((u8)((n >> (8 * i)) & 0xff));
  }
  // Masking key (client frames must be masked).
  u32 mask = (u32)(frameSeq_ * 2654435761u) + 0x9e3779b9u;
  frameSeq_++;
  frame.push_back((u8)(mask >> 24));
  frame.push_back((u8)(mask >> 16));
  frame.push_back((u8)(mask >> 8));
  frame.push_back((u8)(mask & 0xff));
  const u8* p = (const u8*)payload;
  // Mask key is transmitted big-endian (mask>>24 first) per RFC6455; apply it
  // in the same byte order so the receiver's key[i % 4] decrypts correctly.
  const u8 key[4] = {(u8)(mask >> 24), (u8)(mask >> 16), (u8)(mask >> 8), (u8)mask};
  for (size_t i = 0; i < n; ++i) frame.push_back((u8)(p[i] ^ key[i % 4]));

  std::lock_guard<std::mutex> lk(sendMutex_);
  return sock_ && sock_->Send(frame.data(), frame.size()) == (ssize_t)frame.size();
}

//----------------------------------------------------------------------------
// Pure RFC6455 frame parser. This is the part we can fuzz headlessly: no
// sockets, just a buffer + fail-closed statuses.
//----------------------------------------------------------------------------

WsFrameStatus ParseWsFrame(const u8* data, size_t len, WsFrameView* out,
                           u64 maxPayload) {
  if (!out) return WsFrameStatus::Invalid;
  *out = WsFrameView{};
  if (!data || len < 2) return WsFrameStatus::NeedMore;

  const u8 b0 = data[0];
  const u8 b1 = data[1];
  if (b0 & 0x70) return WsFrameStatus::Invalid;   // RSV bits not negotiated
  bool fin = (b0 & 0x80) != 0;
  u8 opcode = b0 & 0x0F;
  bool masked = (b1 & 0x80) != 0;
  u64 payloadLen = b1 & 0x7F;

  // Reserved opcodes and fragmentation control-frame misuse.
  if (opcode >= 0x3 && opcode <= 0x7) return WsFrameStatus::Invalid;
  if (opcode >= 0xB && opcode <= 0xF) return WsFrameStatus::Invalid;
  bool control = opcode >= 0x8;

  // RFC6455 §5.5: control frames MUST have a payload <=125 bytes, so the
  // 126/127 length-marker forms are invalid for them outright (fail closed,
  // before deciding we need more header bytes).
  if (control && (payloadLen == 126 || payloadLen == 127)) return WsFrameStatus::Invalid;

  size_t off = 2;
  if (payloadLen == 126) {
    if (len < off + 2) return WsFrameStatus::NeedMore;
    payloadLen = ((u64)data[off] << 8) | data[off + 1];
    off += 2;
  } else if (payloadLen == 127) {
    if (len < off + 8) return WsFrameStatus::NeedMore;
    payloadLen = 0;
    for (int i = 0; i < 8; ++i) payloadLen = (payloadLen << 8) | data[off + i];
    off += 8;
  }

  if (masked) {
    if (len < off + 4) return WsFrameStatus::NeedMore;
    off += 4;   // masking key; payload that follows is masked on the wire
  }

  // RFC6455: control frames are <=125 bytes and not fragmented.
  if (control && (payloadLen > 125 || !fin)) return WsFrameStatus::Invalid;
  if (payloadLen > maxPayload) return WsFrameStatus::Oversized;
  if (payloadLen > len - off) return WsFrameStatus::NeedMore;

  // The view is read-only: `payload` points into the caller's buffer (the
  // on-the-wire bytes, still masked if `masked`). Callers that need the
  // decoded content unmask themselves using the key at data+off-4 (when
  // masked). Parsing never writes into the buffer, which keeps this function
  // pure and fuzz-safe.
  out->fin = fin;
  out->opcode = opcode;
  out->masked = masked;
  out->payloadLen = payloadLen;
  out->payload = data + off;
  out->frameLen = off + (size_t)payloadLen;
  return WsFrameStatus::Ok;
}

void WebSocket::Pump() {
  if (!open_ || !sock_) return;
  if (!sock_->WaitReadable(0)) return;
  unsigned char hdr[2];
  ssize_t hr = sock_->Recv(hdr, 2, 100);
  if (hr < 2) {
    if (hr == 0) { open_ = false; if (onClose_) onClose_(1006, "eof"); }
    return;
  }
  bool fin = (hdr[0] & 0x80) != 0;
  u8 opcode = hdr[0] & 0x0f;
  bool masked = (hdr[1] & 0x80) != 0;
  u64 len = hdr[1] & 0x7f;
  if (len == 126) {
    unsigned char ext[2];
    if (sock_->Recv(ext, 2, 500) != 2) return;
    len = ((u64)ext[0] << 8) | ext[1];
  } else if (len == 127) {
    unsigned char ext[8];
    if (sock_->Recv(ext, 8, 500) != 8) return;
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
  }
  unsigned char maskKey[4] = {0, 0, 0, 0};
  if (masked && sock_->Recv(maskKey, 4, 500) != 4) return;
  if (len > 16u * 1024 * 1024) return;   // sanity cap
  std::vector<u8> payload((size_t)len);
  size_t got = 0;
  while (got < len) {
    ssize_t n = sock_->Recv(payload.data() + got, (size_t)(len - got), 1000);
    if (n <= 0) return;
    got += (size_t)n;
  }
  for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= maskKey[i % 4];

  WsMessage msg{(WsOpcode)opcode, std::move(payload), fin};
  if (opcode == (u8)WsOpcode::Close) {
    int code = 1000;
    if (msg.payload.size() >= 2) code = (msg.payload[0] << 8) | msg.payload[1];
    std::string reason = msg.payload.size() > 2
        ? std::string((const char*)msg.payload.data() + 2, msg.payload.size() - 2) : "";
    HandleCloseFrame(msg);
    open_ = false;
    if (onClose_) onClose_(code, reason);
    return;
  }
  if (opcode == (u8)WsOpcode::Ping) {
    SendFrame(WsOpcode::Pong, msg.payload.data(), msg.payload.size());
    return;
  }
  if (onMessage_) onMessage_(msg);
}

void WebSocket::HandleCloseFrame(const WsMessage& frame) {
  // Respond with a close echo so the server sees a clean close.
  unsigned char reply[4] = {0x88, 0x02, 0, 0};
  if (frame.payload.size() >= 2) {
    reply[2] = frame.payload[0];
    reply[3] = frame.payload[1];
  }
  if (sock_) sock_->Send(reply, 4);
}

//----------------------------------------------------------------------------
// base64 + SHA1 (RFC6455 handshake needs SHA1("key + GUID"))
//----------------------------------------------------------------------------

static const char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string WsBase64(const unsigned char* d, size_t n) {
  std::string out;
  int i = 0;
  while (i + 3 <= (int)n) {
    u32 v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
    out += kBase64[(v >> 18) & 63];
    out += kBase64[(v >> 12) & 63];
    out += kBase64[(v >> 6) & 63];
    out += kBase64[v & 63];
    i += 3;
  }
  int rem = (int)n - i;
  if (rem == 1) {
    u32 v = d[i] << 16;
    out += kBase64[(v >> 18) & 63];
    out += kBase64[(v >> 12) & 63];
    out += "==";
  } else if (rem == 2) {
    u32 v = (d[i] << 16) | (d[i + 1] << 8);
    out += kBase64[(v >> 18) & 63];
    out += kBase64[(v >> 12) & 63];
    out += kBase64[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

//----------------------------------------------------------------------------
// JSON-RPC 2.0 (tiny)
//----------------------------------------------------------------------------

bool JsonRpc::ParseRequest(const char* json, JsonRpcRequest* out) {
  if (!json || !out) return false;
  const char* p = json;
  auto skipWS = [&]() { for (;; ++p) { char c = *p; if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue; else break; } };
  skipWS();
  if (*p != '{') return false;
  ++p;
  // Find "method"
  // Simple scan: locate "method" then a string value.
  bool hasMethod = false;
  out->id.clear();
  out->hasId = false;
  while (*p) {
    skipWS();
    if (*p == '}') break;
    if (*p == ',') ++p;
    skipWS();
    if (*p != '"') { ++p; continue; }
    ++p;
    std::string key;
    while (*p && *p != '"') { key += *p++; }
    if (*p == '"') ++p;
    skipWS();
    if (*p == ':') ++p;   // consume key/value separator
    skipWS();
    if (key == "method") {
      if (*p == '"') {
        ++p;
        while (*p && *p != '"') { out->method += *p++; }
        if (*p) ++p;
        hasMethod = true;
      }
    } else if (key == "params") {
      int depth = 0;
      const char* start = p;
      while (*p) {
        if (*p == '{' || *p == '[') ++depth;
        else if (*p == '}' || *p == ']') {
          if (--depth <= 0) { ++p; break; }
        } else if (*p == '"' && depth == 0) { break; }
        if (depth == 0) break;
        ++p;
      }
      out->params = std::string(start, p);
    } else if (key == "id") {
      out->hasId = true;
      size_t n = 0;
      while (p[n] && p[n] != ',' && p[n] != '}') ++n;
      out->id = std::string(p, n);
      p += n;
    } else {
      // skip value
      if (*p == '"') { ++p; while (*p && *p != '"') ++p; if (*p) ++p; }
      else { while (*p && *p != ',' && *p != '}') ++p; }
    }
  }
  return hasMethod;
}

std::string JsonRpc::MakeResponse(const char* id, const char* resultJson) {
  return std::string("{\"jsonrpc\":\"2.0\",\"result\":") + resultJson +
         ",\"id\":" + (id && *id ? id : "null") + "}";
}

std::string JsonRpc::MakeError(const char* id, int code, const char* message) {
  char buf[64];
  snprintf(buf, sizeof buf, "%d", code);
  return std::string("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":") + buf +
         ",\"message\":\"" + message + "\"},\"id\":" +
         (id && *id ? id : "null") + "}";
}

std::string JsonRpc::MakeNotification(const char* method, const char* paramsJson) {
  return std::string("{\"jsonrpc\":\"2.0\",\"method\":\"") + method + "\",\"params\":" +
         (paramsJson ? paramsJson : "{}") + "}";
}

//----------------------------------------------------------------------------
// RateLimiter (token bucket)
//----------------------------------------------------------------------------

void RateLimiter::Reset(size_t maxBurst, f32 perSecond) {
  std::lock_guard<std::mutex> lk(mutex_);
  maxBurst_ = (f64)maxBurst;
  perSecond_ = perSecond;
  tokens_ = maxBurst_;
  lastRefillUs_ = (f64)NowNs() / 1000.0;
}

bool RateLimiter::Allow() {
  std::lock_guard<std::mutex> lk(mutex_);
  f64 nowUs = (f64)NowNs() / 1000.0;
  f64 elapsed = nowUs - lastRefillUs_;
  lastRefillUs_ = nowUs;
  tokens_ += elapsed / 1e6 * perSecond_;
  if (tokens_ > maxBurst_) tokens_ = maxBurst_;
  if (tokens_ >= 1.0) {
    tokens_ -= 1.0;
    return true;
  }
  return false;
}

f32 RateLimiter::Usage() const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (maxBurst_ <= 0) return 0;
  return (f32)(tokens_ / maxBurst_);
}

}  // namespace Berry