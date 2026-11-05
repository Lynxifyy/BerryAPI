#include "berry/net.h"
#include "berry/tls.h"
#include "berry/logger.h"
#include "berry/event.h"
#include "berry/version.h"
#include "berry/voice_crypto.h"   // VoiceRandomBytes: the CSPRNG behind WS keys/masks
#include "berry/inflate.h"        // Content-Encoding decode (gzip/deflate)

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
#include <cctype>
#include <algorithm>
#include <sstream>

namespace Berry {

//----------------------------------------------------------------------------
// Deadline
//----------------------------------------------------------------------------

Deadline Deadline::After(int ms) {
  Deadline d;
  const u64 now = NowMs();
  if (ms <= 0) {
    // Already expired: `atMs` must be non-zero so `hasDeadline()` is true and
    // the first wait returns immediately.
    d.atMs = now == 0 ? 1 : now;
  } else {
    d.atMs = now + (u64)ms;
  }
  return d;
}

bool Deadline::expired() const {
  if (atMs == 0) return false;
  return NowMs() >= atMs;
}

int Deadline::remainingMs() const {
  if (atMs == 0) return -1;
  const u64 now = NowMs();
  if (now >= atMs) return 0;
  const u64 left = atMs - now;
  return left > (u64)INT32_MAX ? INT32_MAX : (int)left;
}

//----------------------------------------------------------------------------
// TcpSocket
//----------------------------------------------------------------------------

TcpSocket::TcpSocket() = default;
TcpSocket::~TcpSocket() { Close(); }

void TcpSocket::Close() {
  if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

bool TcpSocket::Connect(const char* host, u16 port, int timeoutMs) {
  return Connect(host, port, Deadline::After(timeoutMs));
}

bool TcpSocket::Connect(const char* host, u16 port, const Deadline& deadline) {
  Close();

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
    // The deadline bounds the WHOLE connect, across every candidate address, so
    // a host with many A/AAAA records cannot multiply the wait.
    if (deadline.expired()) { lastErr = ETIMEDOUT; break; }
    int remaining = deadline.remainingMs();
    timeval tv{};
    if (remaining >= 0) {
      tv.tv_sec = remaining / 1000;
      tv.tv_usec = (remaining % 1000) * 1000;
    } else {
      tv.tv_sec = 5;
    }

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

ssize_t TcpSocket::SendAll(const void* data, size_t len, const Deadline& deadline) {
  if (fd_ < 0) return -1;
  const char* p = (const char*)data;
  size_t sent = 0;
  while (sent < len) {
    const int remaining = deadline.remainingMs();
    if (remaining == 0) return sent > 0 ? (ssize_t)sent : -2;
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd_, &wset);
    timeval tv{};
    const int waitMs = remaining < 0 ? 5000 : remaining;
    tv.tv_sec = waitMs / 1000;
    tv.tv_usec = (waitMs % 1000) * 1000;
    const int rc = ::select(fd_ + 1, nullptr, &wset, nullptr, &tv);
    if (rc == 0) return sent > 0 ? (ssize_t)sent : -2;   // deadline / idle
    if (rc < 0) {
      if (errno == EINTR) continue;
      return sent > 0 ? (ssize_t)sent : -1;
    }
    const ssize_t n = ::send(fd_, p + sent, len - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      return sent > 0 ? (ssize_t)sent : -1;
    }
    sent += (size_t)n;
  }
  return (ssize_t)sent;
}

ssize_t TcpSocket::Recv(void* buf, size_t len, int timeoutMs) {
  if (fd_ < 0) return -1;
  if (!WaitReadable(timeoutMs)) return -2;   // timeout
  return ::recv(fd_, buf, len, 0);
}

ssize_t TcpSocket::Recv(void* buf, size_t len, const Deadline& deadline) {
  if (fd_ < 0) return -1;
  if (!WaitReadable(deadline)) return -2;   // timeout or deadline passed
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

bool TcpSocket::WaitReadable(const Deadline& deadline) {
  if (fd_ < 0) return false;
  const int remaining = deadline.remainingMs();
  if (remaining == 0) return false;   // deadline already passed
  // With no deadline (-1) fall back to a bounded poll, so a caller can never
  // block forever through this path either.
  const int waitMs = remaining < 0 ? 5000 : remaining;
  return WaitReadable(waitMs);
}

//----------------------------------------------------------------------------
// UdpSocket
//----------------------------------------------------------------------------

UdpSocket::UdpSocket() = default;
UdpSocket::~UdpSocket() { Close(); }

void UdpSocket::Close() {
  if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

bool UdpSocket::Bind(u16 port, const char* bindAddr, bool broadcast) {
  Close();
  int s = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) {
    err_ = std::string("socket: ") + strerror(errno);
    return false;
  }

  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (::inet_pton(AF_INET, bindAddr ? bindAddr : "0.0.0.0", &a.sin_addr) != 1) {
    err_ = "bad bind address";
    ::close(s);
    return false;
  }
  // Every client on a host binds the same fixed port, so the group join can be
  // shared. SO_REUSEADDR lets the second bind succeed instead of failing with
  // EADDRINUSE.
  int reuse = 1;
  ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
  if (::bind(s, (sockaddr*)&a, sizeof a) != 0) {
    err_ = std::string("bind: ") + strerror(errno);
    ::close(s);
    return false;
  }
  if (broadcast) {
    int on = 1;
    ::setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
  }
  fd_ = s;
  return true;
}

bool UdpSocket::JoinMulticast(const char* group, const char* ifaceAddr) {
  if (fd_ < 0) { err_ = "not bound"; return false; }
  ip_mreq req{};
  if (::inet_pton(AF_INET, group, &req.imr_multiaddr) != 1 ||
      (req.imr_multiaddr.s_addr & htonl(0xf0000000u)) != htonl(0xe0000000u)) {
    err_ = "not a multicast group";
    return false;
  }
  if (::inet_pton(AF_INET, ifaceAddr ? ifaceAddr : "0.0.0.0",
                  &req.imr_interface) != 1) {
    err_ = "bad interface address";
    return false;
  }
  if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &req, sizeof req) != 0) {
    err_ = std::string("join: ") + strerror(errno);
    return false;
  }
  // Without this a sender on the same host does not see its own datagrams, so a
  // host test (two sockets on loopback) would hear nothing.
  int loop = 1;
  ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
  return true;
}

bool UdpSocket::SetMulticastInterface(const char* ifaceAddr) {
  if (fd_ < 0) { err_ = "not bound"; return false; }
  in_addr iface{};
  if (::inet_pton(AF_INET, ifaceAddr ? ifaceAddr : "0.0.0.0", &iface) != 1) {
    err_ = "bad interface address";
    return false;
  }
  if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof iface) != 0) {
    err_ = std::string("multicast if: ") + strerror(errno);
    return false;
  }
  return true;
}

bool UdpSocket::SendTo(const void* data, size_t len, const char* host, u16 port) {
  if (fd_ < 0) { err_ = "not bound"; return false; }
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (::inet_pton(AF_INET, host, &a.sin_addr) != 1) {
    err_ = "bad destination";
    return false;
  }
  ssize_t n = ::sendto(fd_, data, len, MSG_NOSIGNAL, (sockaddr*)&a, sizeof a);
  if (n < 0 || (size_t)n != len) {
    err_ = std::string("sendto: ") + strerror(errno);
    return false;
  }
  return true;
}

ssize_t UdpSocket::RecvFrom(void* buf, size_t len, std::string* fromHost,
                            u16* fromPort, int timeoutMs) {
  if (fd_ < 0) { err_ = "not bound"; return -1; }
  fd_set rset;
  FD_ZERO(&rset);
  FD_SET(fd_, &rset);
  timeval tv{};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  int rc = ::select(fd_ + 1, &rset, nullptr, nullptr, &tv);
  if (rc == 0) return 0;               // timeout
  if (rc < 0) { err_ = std::string("select: ") + strerror(errno); return -1; }

  sockaddr_in from{};
  socklen_t flen = sizeof from;
  ssize_t n = ::recvfrom(fd_, buf, len, 0, (sockaddr*)&from, &flen);
  if (n < 0) { err_ = std::string("recvfrom: ") + strerror(errno); return -1; }
  if (fromHost) {
    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
    *fromHost = ip;
  }
  if (fromPort) *fromPort = ntohs(from.sin_port);
  return n;
}

u16 UdpSocket::localPort() const {
  if (fd_ < 0) return 0;
  sockaddr_in a{};
  socklen_t len = sizeof a;
  if (::getsockname(fd_, (sockaddr*)&a, &len) != 0) return 0;
  return ntohs(a.sin_port);
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

bool HttpRequest::HasControl(const std::string& s) {
  for (char c : s) {
    if (c == '\r' || c == '\n' || c == '\0') return true;
  }
  return false;
}

bool HttpRequest::ParseAuthority(const std::string& authority, u16 defaultPort) {
  port_ = defaultPort;
  explicitPort_ = false;
  std::string hostport = authority;

  // An IPv6 literal is bracketed: [::1]:8080. The colon inside the brackets is
  // part of the address, never a port separator (bug 33).
  size_t at = hostport.find('@');
  if (at != std::string::npos) hostport = hostport.substr(at + 1);   // drop userinfo
  if (hostport.empty()) return false;

  if (hostport[0] == '[') {
    size_t close = hostport.find(']');
    if (close == std::string::npos) return false;
    host_ = hostport.substr(1, close - 1);
    std::string after = hostport.substr(close + 1);
    if (after.empty()) return true;
    if (after[0] != ':') return false;
    std::string portStr = after.substr(1);
    if (portStr.empty()) return false;
    for (char c : portStr) if (!isdigit((unsigned char)c)) return false;
    long p = strtol(portStr.c_str(), nullptr, 10);
    if (p < 1 || p > 65535) return false;   // reject 0/out-of-range (bug 34)
    port_ = (u16)p;
    explicitPort_ = true;
    return true;
  }

  size_t colon = hostport.rfind(':');
  if (colon == std::string::npos) {
    host_ = hostport;
    return !host_.empty();
  }
  host_ = hostport.substr(0, colon);
  std::string portStr = hostport.substr(colon + 1);
  if (host_.empty() || portStr.empty()) return false;
  for (char c : portStr) if (!isdigit((unsigned char)c)) return false;
  long p = strtol(portStr.c_str(), nullptr, 10);
  if (p < 1 || p > 65535) return false;     // a junk or 70000 port is refused
  port_ = (u16)p;
  explicitPort_ = true;
  return true;
}

HttpRequest::HttpRequest(const char* url) : url_(url ? url : "") {
  // parse "http://host:port/path" or "https://host:port/path"
  std::string u = url_;
  // A control character anywhere in the URL (host, path or port) is refused so
  // it can never be spliced into the request line or a header (bug 32).
  if (HasControl(u)) {
    malformed_ = true;
    return;
  }
  std::string rest = u;
  if (rest.rfind("http://", 0) == 0) {
    rest = rest.substr(7);
  } else if (rest.rfind("https://", 0) == 0) {
    rest = rest.substr(8);
    if (TlsSocket::Available()) {
      tls_ = true;
      port_ = 443;   // the default for https unless the authority overrides it
    } else {
      // A build without TLS must FAIL CLOSED on https rather than open a
      // plaintext socket to port 443: silently downgrading would send the
      // caller's traffic unencrypted while they asked for TLS.
      badScheme_ = true;
    }
  } else {
    // An unknown/malformed scheme: refuse, and do not try to parse an authority
    // out of something that is not a URL.
    badScheme_ = true;
    return;
  }
  size_t slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  path_ = slash == std::string::npos ? "/" : rest.substr(slash);
  if (!ParseAuthority(authority, port_)) {
    malformed_ = true;
  }
}

HttpRequest& HttpRequest::Header(const char* name, const char* value) {
  // A header name or value carrying CR/LF/NUL would split the request; refuse
  // to store it so it can never reach the wire (bug 32).
  if (name && value && !HasControl(name) && !HasControl(value)) {
    headers_.emplace_back(name, value);
  }
  return *this;
}

HttpResponse HttpRequest::Do() {
  HttpResponse resp;
  if (malformed_) {
    // A control character in the URL, or a bad authority/port: refuse without
    // opening a socket so a request-splitting URL can never reach the wire.
    resp.status = 0;
    resp.reason = "malformed request URL";
    return resp;
  }
  if (badScheme_) {
    // https without TLS support, or a malformed scheme: refuse without opening
    // a socket. A silent downgrade would send plaintext where TLS was asked.
    resp.status = 0;
    resp.reason = "unsupported scheme (TLS not available)";
    return resp;
  }
  // One absolute deadline for connect + handshake + send + every read. A
  // per-read timeout is not a deadline: a server that dribbles one byte just
  // before each timeout would otherwise keep this loop alive indefinitely.
  const Deadline deadline = Deadline::After(timeoutMs_);

  TcpSocket sock;
  TlsSocket tls;
  if (tls_) {
    if (!tls.Connect(host_.c_str(), port_, deadline)) {
      resp.status = 0;
      resp.reason = "tls connect failed: " + tls.LastError();
      return resp;
    }
  } else if (!sock.Connect(host_.c_str(), port_, deadline)) {
    resp.status = 0;
    resp.reason = "connect error";
    return resp;
  }

  // The Host header carries the port whenever it is not the scheme default (or
  // whenever the caller wrote one explicitly) so a virtual host on a
  // non-default port is addressed correctly (bug 35). An IPv6 literal is
  // re-bracketed, which the Host grammar requires.
  const bool ipv6 = host_.find(':') != std::string::npos;
  const std::string hostHdr = (ipv6 ? "[" + host_ + "]" : host_) +
      ((explicitPort_ || port_ != (tls_ ? 443 : 80))
           ? ":" + std::to_string((unsigned)port_)
           : "");

  std::string req = method_ + " " + path_ + " HTTP/1.1\r\n"
      "Host: " + hostHdr + "\r\n"
      "User-Agent: BerryAPI/" BERRY_VERSION_STRING "\r\n"
      "Accept-Encoding: gzip, deflate\r\n"
      "Connection: close\r\n";
  // A body-bearing method always sends Content-Length, even for an empty body,
  // so a server that requires it does not answer 411 (bug 36).
  if (!body_.empty() || method_ == "POST" || method_ == "PUT" ||
      method_ == "PATCH") {
    char lenBuf[32];
    snprintf(lenBuf, sizeof lenBuf, "%zu", body_.size());
    req += std::string("Content-Length: ") + lenBuf + "\r\n";
  }
  for (auto& h : headers_) {
    std::string lower = h.first;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    if (lower == "host" || lower == "content-length" ||
        lower == "connection" || lower == "transfer-encoding") {
      continue;   // client-owned; a duplicate is a smuggling vector
    }
    req += h.first + ": " + h.second + "\r\n";
  }
  req += "\r\n";
  req += body_;

  const ssize_t sent = tls_ ? tls.Send(req.data(), req.size(), deadline)
                            : sock.SendAll(req.data(), req.size(), deadline);
  if (sent != (ssize_t)req.size()) {
    resp.status = 0;
    resp.reason = "send failed";
    return resp;
  }

  char buf[8192];
  std::string raw;
  // Cap the total buffered bytes (headers + body). A server that streams
  // forever must not be able to make the worker allocate without bound; the
  // read stops at the cap and the response is flagged truncated.
  const size_t cap = maxBodyBytes_ + 64 * 1024;   // headroom for the headers
  bool timedOut = false;
  for (;;) {
    // The wait is clamped to what is left of the deadline, so the total time
    // is bounded no matter how the peer paces its bytes.
    ssize_t n = tls_ ? tls.Recv(buf, sizeof buf, deadline)
                     : sock.Recv(buf, sizeof buf, deadline);
    if (n < 0) {
      if (n == -2) {
        timedOut = true;
        if (raw.empty()) {
          resp.status = 0;
          resp.reason = "timeout";
          return resp;
        }
      }
      break;
    }
    if (n == 0) break;
    if (raw.size() + (size_t)n > cap) {
      raw.append(buf, cap - raw.size());
      resp.truncated = true;
      break;
    }
    raw.append(buf, (size_t)n);
  }

  size_t hEnd = raw.find("\r\n\r\n");
  if (hEnd == std::string::npos) {
    resp.status = 0;
    resp.reason = "no HTTP headers";
    return resp;
  }

  auto trim = [](std::string s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
  };
  auto ieq = [](const std::string& a, const char* b) {
    size_t n = strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i)
      if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
  };

  std::istringstream hstream(raw.substr(0, hEnd));
  std::string line;
  bool statusOk = false;
  if (std::getline(hstream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    // Strict: "HTTP/1.0" or "HTTP/1.1", a space, then exactly three digits.
    if (line.rfind("HTTP/1.", 0) == 0 && line.size() >= 12 &&
        isdigit((unsigned char)line[7]) &&
        isdigit((unsigned char)line[9]) &&
        isdigit((unsigned char)line[10]) &&
        isdigit((unsigned char)line[11]) && line[8] == ' ') {
      resp.status = (line[9] - '0') * 100 + (line[10] - '0') * 10 +
                    (line[11] - '0');
      statusOk = true;
      size_t sp = line.find(' ', 12);
      resp.reason = sp == std::string::npos ? std::string() : line.substr(sp + 1);
    }
  }
  if (!statusOk) {
    resp.status = 0;
    resp.reason = "bad HTTP status line";
    return resp;
  }

  // Header fields. An obsolete line-fold (a continuation line starting with
  // space/tab) appends to the previous value; a duplicate field is combined
  // with ", " per RFC 7230 (bug 37).
  while (std::getline(hstream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    if (line[0] == ' ' || line[0] == '\t') {
      if (!resp.headers.empty()) {
        resp.headers.back().second += " " + trim(line);
      }
      continue;
    }
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = line.substr(0, colon);
    std::string value = trim(line.substr(colon + 1));
    bool merged = false;
    for (auto& h : resp.headers) {
      if (ieq(h.first, name.c_str())) {
        h.second += ", " + value;
        merged = true;
        break;
      }
    }
    if (!merged) resp.headers.emplace_back(name, value);
  }

  // Body framing. Chunked wins over Content-Length (RFC 7230 §3.3.3), so a
  // server that chunks a response is decoded rather than having its size lines
  // left in the body (bug 30).
  std::string bodyRaw = raw.substr(hEnd + 4);
  bool chunked = false;
  const char* clHeader = resp.Header("Content-Length");
  for (auto& h : resp.headers) {
    if (ieq(h.first, "Transfer-Encoding") &&
        h.second.find("chunked") != std::string::npos) {
      chunked = true;
    }
  }

  if (chunked) {
    std::string decoded;
    size_t pos = 0;
    for (;;) {
      size_t eol = bodyRaw.find("\r\n", pos);
      if (eol == std::string::npos) { resp.truncated = true; break; }
      std::string sizeLine = bodyRaw.substr(pos, eol - pos);
      size_t semi = sizeLine.find(';');
      if (semi != std::string::npos) sizeLine = sizeLine.substr(0, semi);
      sizeLine = trim(sizeLine);
      if (sizeLine.empty()) { resp.truncated = true; break; }
      char* endp = nullptr;
      unsigned long long chunk = strtoull(sizeLine.c_str(), &endp, 16);
      if (endp == sizeLine.c_str()) { resp.truncated = true; break; }
      if (chunk == 0) break;   // last chunk
      pos = eol + 2;
      if (pos + chunk > bodyRaw.size()) { resp.truncated = true; break; }
      if (decoded.size() + chunk > maxBodyBytes_) {
        decoded.append(bodyRaw, pos, maxBodyBytes_ - decoded.size());
        resp.truncated = true;
        break;
      }
      decoded.append(bodyRaw, pos, (size_t)chunk);
      pos += (size_t)chunk + 2;   // skip the chunk's trailing CRLF
    }
    resp.body = std::move(decoded);
  } else if (clHeader && strchr(clHeader, ',') != nullptr) {
    // Two different Content-Length values (merged above) are a smuggling
    // vector; refuse rather than pick one.
    resp.truncated = true;
    resp.body.clear();
  } else if (clHeader) {
    long long cl = strtoll(clHeader, nullptr, 10);
    if (cl < 0) {
      resp.truncated = true;
      resp.body.clear();
    } else if ((size_t)cl > bodyRaw.size()) {
      // A short body is a truncated response, never a complete one (bug 31).
      resp.body = bodyRaw;
      resp.truncated = true;
    } else {
      resp.body = bodyRaw.substr(0, (size_t)cl);
    }
  } else {
    // No framing header: the body runs to the end of the connection (close).
    resp.body = std::move(bodyRaw);
  }

  // Content-Encoding (improvement 34): decode gzip/deflate so a caller sees the
  // entity body, not the transfer encoding. A decode failure leaves the body as
  // received (a caller that asked for identity still gets bytes) rather than
  // silently corrupting it; `encoding_failed` records the failure.
  {
    const char* enc = resp.Header("Content-Encoding");
    if (enc && *enc) {
      std::string lower;
      for (const char* q = enc; *q; ++q) lower.push_back((char)tolower((unsigned char)*q));
      const bool gz = lower.find("gzip") != std::string::npos;
      const bool df = lower.find("deflate") != std::string::npos;
      if (gz || df) {
        std::string decoded;
        if (InflateAuto(reinterpret_cast<const u8*>(resp.body.data()),
                        resp.body.size(), &decoded, maxBodyBytes_)) {
          resp.body = std::move(decoded);
        } else {
          resp.encodingFailed = true;
        }
      }
    }
  }

  // Trim the body to the caller's cap even when the raw buffer was within the
  // headroom above, so maxBodyBytes is a hard bound on what a caller receives.
  if (resp.body.size() > maxBodyBytes_) {
    resp.body.resize(maxBodyBytes_);
    resp.truncated = true;
  }
  // A deadline that fired mid-body is a truncated response, not a complete one;
  // flag it so a caller does not parse half a document as if it were whole.
  if (timedOut) resp.truncated = true;
  return resp;
}

//----------------------------------------------------------------------------
// WebSocket (RFC6455)
//----------------------------------------------------------------------------

WebSocket::WebSocket() = default;
WebSocket::~WebSocket() { Close(); }

static std::string WsBase64(const unsigned char* data, size_t len);

// A compact SHA-1, used only for the RFC6455 handshake accept value. SHA-1 is
// not a security primitive here: the accept value proves the peer read our key,
// nothing more.
namespace {

struct Sha1 {
  u32 h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  u64 len = 0;
  u8 buf[64];
  size_t bufLen = 0;

  static u32 Rol(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

  void Block(const u8* p) {
    u32 w[80];
    for (int i = 0; i < 16; ++i)
      w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) |
             ((u32)p[i * 4 + 2] << 8) | (u32)p[i * 4 + 3];
    for (int i = 16; i < 80; ++i)
      w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      u32 f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
      u32 tmp = Rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = Rol(b, 30); b = a; a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }

  void Update(const u8* p, size_t n) {
    len += n;
    while (n) {
      size_t take = 64 - bufLen;
      if (take > n) take = n;
      memcpy(buf + bufLen, p, take);
      bufLen += take; p += take; n -= take;
      if (bufLen == 64) { Block(buf); bufLen = 0; }
    }
  }

  void Final(u8 out[20]) {
    u64 bits = len * 8;
    u8 pad = 0x80;
    Update(&pad, 1);
    u8 zero = 0;
    while (bufLen != 56) Update(&zero, 1);
    u8 lenb[8];
    for (int i = 0; i < 8; ++i) lenb[i] = (u8)(bits >> (56 - 8 * i));
    Update(lenb, 8);
    for (int i = 0; i < 5; ++i) {
      out[i * 4] = (u8)(h[i] >> 24);
      out[i * 4 + 1] = (u8)(h[i] >> 16);
      out[i * 4 + 2] = (u8)(h[i] >> 8);
      out[i * 4 + 3] = (u8)h[i];
    }
  }
};

}  // namespace

std::string WsAcceptFor(const std::string& key) {
  static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  Sha1 s;
  s.Update(reinterpret_cast<const u8*>(key.data()), key.size());
  s.Update(reinterpret_cast<const u8*>(kGuid), sizeof(kGuid) - 1);
  u8 digest[20];
  s.Final(digest);
  return WsBase64(digest, 20);
}

namespace {

/// A cryptographically secure 4-byte masking key. Returns false when no
/// CSPRNG is available, so a caller refuses rather than sending a predictable
/// mask (RFC6455 §10.3 forbids a predictable mask).
bool WsSecureMask(u8 out[4]) {
  return VoiceRandomBytes(out, 4);
}

/// Fills `out` with `n` cryptographically secure bytes.
bool WsSecureRandom(u8* out, size_t n) { return VoiceRandomBytes(out, n); }

/// A close code that may appear on the wire. 1005 and 1006 are reserved for
/// local use and must never be sent; 1004 is undefined; a code below 1000 or an
/// unassigned 1016-2999 code is invalid (bug 45).
bool ValidCloseCode(int code) {
  if (code < 1000 || code >= 5000) return false;
  switch (code) {
    case 1004: case 1005: case 1006: case 1015:
      return false;
    default:
      return true;
  }
}

}  // namespace

bool WebSocket::Connect(const char* url, int timeoutMs) {
  const Deadline deadline = Deadline::After(timeoutMs);
  std::string u = url;
  std::string rest = u;
  bool wantTls = false;
  if (rest.rfind("ws://", 0) == 0) {
    rest = rest.substr(5);
  } else if (rest.rfind("wss://", 0) == 0) {
    rest = rest.substr(6);
    wantTls = true;
    if (!TlsSocket::Available()) {
      // FAIL CLOSED. Downgrading to a plaintext socket on port 443 would
      // silently send the caller's traffic unencrypted while they asked for
      // TLS — the worst possible failure mode.
      err_ = "wss not supported (TLS not available)";
      return false;
    }
  } else {
    err_ = "bad ws url";
    return false;
  }
  size_t slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  std::string path = slash == std::string::npos ? "/" : rest.substr(slash);

  // A control character in the URL would split the request line; refuse it.
  for (char c : u) {
    if (c == '\r' || c == '\n' || c == '\0') {
      err_ = "bad ws url";
      return false;
    }
  }

  std::string host = authority;
  u16 port = wantTls ? 443 : 80;
  bool explicitPort = false;
  // A bracketed IPv6 literal keeps its colons; only a colon after the closing
  // bracket separates the port.
  if (!authority.empty() && authority[0] == '[') {
    size_t close = authority.find(']');
    if (close == std::string::npos) {
      err_ = "bad ws url";
      return false;
    }
    host = authority.substr(1, close - 1);
    std::string after = authority.substr(close + 1);
    if (!after.empty()) {
      if (after[0] != ':') { err_ = "bad ws url"; return false; }
      std::string ps = after.substr(1);
      for (char c : ps) if (!isdigit((unsigned char)c)) { err_ = "bad ws url"; return false; }
      long p = strtol(ps.c_str(), nullptr, 10);
      if (p < 1 || p > 65535) { err_ = "bad ws port"; return false; }
      port = (u16)p;
      explicitPort = true;
    }
  } else {
    size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
      host = authority.substr(0, colon);
      std::string ps = authority.substr(colon + 1);
      for (char c : ps) if (!isdigit((unsigned char)c)) { err_ = "bad ws url"; return false; }
      long p = strtol(ps.c_str(), nullptr, 10);
      if (p < 1 || p > 65535) { err_ = "bad ws port"; return false; }
      port = (u16)p;
      explicitPort = true;
    }
  }
  if (host.empty()) { err_ = "bad ws url"; return false; }

  StreamClose();
  rxBuf_.clear();
  closeSent_ = false;
  tls_ = wantTls;
  if (wantTls) {
    tlsSock_.reset(new TlsSocket());
    if (!tlsSock_->Connect(host.c_str(), port, deadline)) {
      err_ = tlsSock_->LastError();
      tlsSock_.reset();
      tls_ = false;
      return false;
    }
  } else {
    sock_.reset(new TcpSocket());
    if (!sock_->Connect(host.c_str(), port, deadline)) {
      err_ = sock_->LastError();
      sock_.reset();
      return false;
    }
  }

  // A random 16-byte nonce, base64-encoded, as RFC6455 §4.1 requires. A
  // predictable key would let a cache-poisoning proxy forge the handshake.
  unsigned char keyBytes[16];
  if (!WsSecureRandom(keyBytes, sizeof keyBytes)) {
    err_ = "no csprng";
    StreamClose();
    return false;
  }
  std::string key = WsBase64(keyBytes, 16);

  const bool ipv6 = host.find(':') != std::string::npos;
  const std::string hostHdr = (ipv6 ? "[" + host + "]" : host) +
      ((explicitPort || port != (wantTls ? 443 : 80))
           ? ":" + std::to_string((unsigned)port)
           : "");
  std::string req =
      "GET " + path + " HTTP/1.1\r\n"
      "Host: " + hostHdr + "\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: " + key + "\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "User-Agent: BerryAPI\r\n\r\n";
  if (StreamSend(req.data(), req.size(), deadline) != (ssize_t)req.size()) {
    err_ = "handshake send failed";
    StreamClose();
    return false;
  }

  // Read handshake response, bounded.
  char buf[4096];
  std::string raw;
  while (raw.find("\r\n\r\n") == std::string::npos) {
    ssize_t n = StreamRecv(buf, sizeof buf, deadline);
    if (n <= 0) {
      err_ = "handshake timeout";
      StreamClose();
      return false;
    }
    raw.append(buf, (size_t)n);
    if (raw.size() > 65536) {
      err_ = "handshake too large";
      StreamClose();
      return false;
    }
  }
  size_t hEnd = raw.find("\r\n\r\n");

  // Strict upgrade check: the status line must be exactly "HTTP/1.1 101 ...",
  // and the Upgrade and Connection headers must both be present (bug 39).
  bool ok101 = raw.rfind("HTTP/1.1 101", 0) == 0;
  std::string head = raw.substr(0, hEnd);
  bool hasUpgrade = head.find("Upgrade: websocket") != std::string::npos ||
                    head.find("upgrade: websocket") != std::string::npos;
  bool hasConnection = head.find("Connection: Upgrade") != std::string::npos ||
                       head.find("connection: Upgrade") != std::string::npos ||
                       head.find("connection: upgrade") != std::string::npos;
  // Sec-WebSocket-Accept must equal base64(SHA1(key + GUID)) (bug 38).
  std::string wantAccept = WsAcceptFor(key);
  bool acceptOk = false;
  {
    const std::string hdrName = "Sec-WebSocket-Accept:";
    size_t p = head.find(hdrName);
    if (p == std::string::npos) p = head.find("sec-websocket-accept:");
    if (p != std::string::npos) {
      size_t v = p + hdrName.size();
      size_t e = head.find("\r\n", v);
      std::string val = head.substr(v, e == std::string::npos ? std::string::npos : e - v);
      size_t b = val.find_first_not_of(" \t");
      size_t en = val.find_last_not_of(" \t\r");
      if (b != std::string::npos) val = val.substr(b, en - b + 1);
      acceptOk = (val == wantAccept);
    }
  }
  if (!ok101 || !hasUpgrade || !hasConnection || !acceptOk) {
    err_ = "not upgraded";
    StreamClose();
    return false;
  }

  // Any bytes after the handshake terminator are the start of the frame
  // stream; keep them so the first frame is not lost (bug 43).
  std::string leftover = raw.substr(hEnd + 4);
  rxBuf_.assign(leftover.begin(), leftover.end());

  open_ = true;
  if (onOpen_) onOpen_();
  return true;
}

ssize_t WebSocket::StreamSend(const void* data, size_t len, const Deadline& dl) {
  if (tlsSock_) return tlsSock_->Send(data, len, dl);
  if (sock_) return sock_->SendAll(data, len, dl);
  return -1;
}

ssize_t WebSocket::StreamRecv(void* buf, size_t len, const Deadline& dl) {
  if (tlsSock_) return tlsSock_->Recv(buf, len, dl);
  if (sock_) return sock_->Recv(buf, len, dl);
  return -1;
}

bool WebSocket::StreamWaitReadable(const Deadline& dl) {
  if (tlsSock_) {
    // A TLS record may already be buffered inside mbedTLS; `Readable` checks
    // that first, then the socket, so a Pump never misses a frame.
    return tlsSock_->Readable(dl.remainingMs());
  }
  if (sock_) return sock_->WaitReadable(dl);
  return false;
}

void WebSocket::StreamClose() {
  if (tlsSock_) { tlsSock_->Close(); tlsSock_.reset(); }
  if (sock_) { sock_->Close(); sock_.reset(); }
  tls_ = false;
}

void WebSocket::Close(int code, const char* reason) {
  if (!open_) return;
  open_ = false;
  const char* why = reason ? reason : "";

  // Send a properly masked close frame carrying a validated code and the
  // reason text, unless the code may not appear on the wire (bug 45).
  if ((sock_ || tlsSock_) && !closeSent_) {
    closeSent_ = true;
    if (ValidCloseCode(code)) {
      std::string payload;
      payload.push_back((char)((code >> 8) & 0xff));
      payload.push_back((char)(code & 0xff));
      payload.append(why);
      if (payload.size() > 125) payload.resize(125);   // control frame cap
      // Reuse SendFrame's masking, but it checks open_; build inline.
      u8 mask[4];
      if (WsSecureMask(mask)) {
        std::vector<u8> frame;
        frame.push_back(0x88);
        frame.push_back((u8)(0x80 | payload.size()));
        frame.push_back(mask[0]); frame.push_back(mask[1]);
        frame.push_back(mask[2]); frame.push_back(mask[3]);
        for (size_t i = 0; i < payload.size(); ++i)
          frame.push_back((u8)payload[i] ^ mask[i % 4]);
        std::lock_guard<std::mutex> lk(sendMutex_);
        StreamSend(frame.data(), frame.size(), Deadline::After(1000));
      }
    }
  }

  // Wait briefly for the peer's close echo so the server sees a clean close.
  const Deadline dl = Deadline::After(500);
  u8 tmp[64];
  while (StreamWaitReadable(dl)) {
    ssize_t n = StreamRecv(tmp, sizeof tmp, dl);
    if (n <= 0) break;
    if ((tmp[0] & 0x0f) == 0x08) break;   // a close frame arrived
  }

  if (onClose_) onClose_(code, why);
  StreamClose();
  rxBuf_.clear();
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
  if (!open_ || (!sock_ && !tlsSock_)) return false;

  // A fresh, unpredictable masking key per frame, from the CSPRNG (bug 41).
  u8 mask[4];
  if (!WsSecureMask(mask)) return false;

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
  frame.push_back(mask[0]); frame.push_back(mask[1]);
  frame.push_back(mask[2]); frame.push_back(mask[3]);
  const u8* p = (const u8*)payload;
  for (size_t i = 0; i < n; ++i) frame.push_back((u8)(p[i] ^ mask[i % 4]));

  // The whole send (including the mask draw) is serialized, so two concurrent
  // SendFrame calls cannot interleave on the wire.
  std::lock_guard<std::mutex> lk(sendMutex_);
  return StreamSend(frame.data(), frame.size(), Deadline::After(5000)) ==
         (ssize_t)frame.size();
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

  // A server-to-client frame MUST NOT be masked (RFC6455 §5.1). A masked frame
  // is a protocol error, not something to accept and unmask (bug 46).
  if (masked) return WsFrameStatus::Invalid;

  size_t off = 2;
  if (payloadLen == 126) {
    if (len < off + 2) return WsFrameStatus::NeedMore;
    payloadLen = ((u64)data[off] << 8) | data[off + 1];
    // Non-minimal encoding: a value < 126 must have used the 7-bit form.
    if (payloadLen < 126) return WsFrameStatus::Invalid;
    off += 2;
  } else if (payloadLen == 127) {
    if (len < off + 8) return WsFrameStatus::NeedMore;
    payloadLen = 0;
    for (int i = 0; i < 8; ++i) payloadLen = (payloadLen << 8) | data[off + i];
    // The most significant bit must be 0, and the value must need 8 bytes.
    if (payloadLen & 0x8000000000000000ULL) return WsFrameStatus::Invalid;
    if (payloadLen < 65536) return WsFrameStatus::Invalid;
    off += 8;
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

bool WebSocket::RecvInto(std::string* out, size_t n, const Deadline& dl) {
  while (out->size() < n) {
    char tmp[4096];
    size_t want = n - out->size();
    if (want > sizeof tmp) want = sizeof tmp;
    ssize_t r = StreamRecv(tmp, want, dl);
    if (r <= 0) return false;
    out->append(tmp, (size_t)r);
  }
  return true;
}

void WebSocket::Pump() {
  if (!open_ || (!sock_ && !tlsSock_)) return;
  // A Pump is a bounded read attempt. The carried buffer (rxBuf_) is the single
  // source of frame bytes, so bytes that arrived with the handshake, or a
  // partial frame from an earlier Pump, are never lost (bugs 43, 47).
  const Deadline poll = Deadline::After(200);

  // Read whatever is available into rxBuf_.
  if (StreamWaitReadable(poll)) {
    char tmp[8192];
    ssize_t n = StreamRecv(tmp, sizeof tmp, poll);
    if (n > 0) {
      rxBuf_.insert(rxBuf_.end(), (const u8*)tmp, (const u8*)tmp + n);
    } else if (n == 0) {
      open_ = false;
      if (onClose_) onClose_(1006, "eof");
      return;
    }
  }
  if (rxBuf_.empty()) return;

  WsFrameView view;
  WsFrameStatus st = ParseWsFrame(rxBuf_.data(), rxBuf_.size(), &view,
                                  16u * 1024 * 1024);
  if (st == WsFrameStatus::NeedMore) return;   // keep buffering
  if (st == WsFrameStatus::Invalid) {
    // A protocol error from the peer: close with 1002 and stop.
    err_ = "protocol error";
    Close(1002, "protocol error");
    return;
  }
  if (st == WsFrameStatus::Oversized) {
    // A frame bomb: close 1009 and drop the buffer so we never desync (bug 47).
    err_ = "oversized frame";
    Close(1009, "message too big");
    return;
  }

  // A complete frame: consume exactly its bytes.
  std::vector<u8> payload(view.payload, view.payload + view.payloadLen);
  rxBuf_.erase(rxBuf_.begin(), rxBuf_.begin() + view.frameLen);

  WsMessage msg{(WsOpcode)view.opcode, std::move(payload), view.fin};
  if (view.opcode == (u8)WsOpcode::Close) {
    int code = 1000;
    if (msg.payload.size() >= 2) code = (msg.payload[0] << 8) | msg.payload[1];
    std::string reason = msg.payload.size() > 2
        ? std::string((const char*)msg.payload.data() + 2, msg.payload.size() - 2) : "";
    // Echo the close (masked) and report it once.
    if (!closeSent_ && (sock_ || tlsSock_)) {
      closeSent_ = true;
      u8 mask[4];
      if (WsSecureMask(mask)) {
        u8 reply[6] = {0x88, 0x82, mask[0], mask[1], mask[2], mask[3]};
        u8 p0 = (u8)((code >> 8) & 0xff) ^ mask[0];
        u8 p1 = (u8)(code & 0xff) ^ mask[1];
        reply[4] = p0; reply[5] = p1;
        StreamSend(reply, sizeof reply, Deadline::After(1000));
      }
    }
    open_ = false;
    if (onClose_) onClose_(code, reason);
    StreamClose();
    rxBuf_.clear();
    return;
  }
  if (view.opcode == (u8)WsOpcode::Ping) {
    SendFrame(WsOpcode::Pong, msg.payload.data(), msg.payload.size());
    return;
  }
  if (onMessage_) onMessage_(msg);
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