// Loopback metrics debug endpoint. See include/berry/debug_endpoint.h.
#include "berry/debug_endpoint.h"
#include "berry/logger.h"
#include "berry/url.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <sstream>

namespace Berry {

const char* DebugEndpointErrorName(DebugEndpointError e) {
  switch (e) {
    case DebugEndpointError::Ok: return "ok";
    case DebugEndpointError::NotLoopback: return "not-loopback";
    case DebugEndpointError::NoToken: return "no-token";
    case DebugEndpointError::BindFailed: return "bind-failed";
    case DebugEndpointError::AlreadyRunning: return "already-running";
  }
  return "?";
}

bool DebugEndpoint::ConstantTimeEquals(const std::string& a,
                                       const std::string& b) {
  // Compare the lengths without branching on content, then fold the byte
  // differences into one accumulator so the compare is data-independent.
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    diff |= (unsigned char)(a[i] ^ b[i]);
  }
  return diff == 0;
}

std::string DebugEndpoint::HandleRequest(const std::string& request,
                                         const std::string& token,
                                         MetricsRegistry* reg) {
  // The request is the first line, e.g. "GET /metrics?token=abc HTTP/1.1",
  // followed by headers. Extract the token from the query or the header.
  std::string gotToken;
  {
    size_t sp1 = request.find(' ');
    size_t sp2 = sp1 == std::string::npos ? std::string::npos
                                          : request.find(' ', sp1 + 1);
    if (sp1 != std::string::npos && sp2 != std::string::npos) {
      const std::string target = request.substr(sp1 + 1, sp2 - sp1 - 1);
      const std::string q = "token=";
      size_t p = target.find(q);
      if (p != std::string::npos) {
        size_t end = target.find('&', p);
        gotToken = target.substr(p + q.size(),
                                 end == std::string::npos ? std::string::npos
                                                          : end - (p + q.size()));
      }
    }
  }
  if (gotToken.empty()) {
    const std::string h = "X-Berry-Token:";
    size_t p = request.find(h);
    if (p != std::string::npos) {
      size_t v = p + h.size();
      size_t e = request.find("\r\n", v);
      std::string t = request.substr(v, e == std::string::npos ? std::string::npos
                                                              : e - v);
      size_t b = t.find_first_not_of(" \t");
      size_t en = t.find_last_not_of(" \t\r");
      if (b != std::string::npos) gotToken = t.substr(b, en - b + 1);
    }
  }
  if (!ConstantTimeEquals(gotToken, token)) {
    return "{\"error\":\"unauthorized\"}";
  }
  if (!reg) return "{\"error\":\"no metrics\"}";

  // A bounded JSON snapshot.
  std::string out = "{\"counters\":{";
  bool first = true;
  for (const auto& kv : reg->Snapshot()) {
    if (!first) out += ",";
    first = false;
    out += "\"" + kv.first + "\":" + std::to_string((long long)kv.second);
  }
  out += "},\"histograms\":[";
  first = true;
  for (const auto& h : reg->HistogramSnapshot()) {
    if (!first) out += ",";
    first = false;
    out += "{\"name\":\"" + h.name + "\",\"count\":" + std::to_string((unsigned long long)h.count) +
           ",\"mean\":" + std::to_string(h.mean) +
           ",\"min\":" + std::to_string(h.min) +
           ",\"max\":" + std::to_string(h.max) + "}";
  }
  out += "]}";
  return out;
}

DebugEndpoint::DebugEndpoint() = default;
DebugEndpoint::~DebugEndpoint() { Stop(); }

DebugEndpointError DebugEndpoint::Start(const DebugEndpointConfig& cfg) {
  if (running_.load()) return DebugEndpointError::AlreadyRunning;
  if (cfg.token.empty()) return DebugEndpointError::NoToken;
  // Refuse anything that is not loopback, so the endpoint is never reachable
  // off the device.
  if (!IsLoopbackHost(cfg.bindAddr)) {
    return DebugEndpointError::NotLoopback;
  }
  token_ = cfg.token;

  listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd_ < 0) return DebugEndpointError::BindFailed;
  int reuse = 1;
  ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(cfg.port);
  if (::bind(listenFd_, (sockaddr*)&a, sizeof a) != 0) {
    ::close(listenFd_);
    listenFd_ = -1;
    return DebugEndpointError::BindFailed;
  }
  if (::listen(listenFd_, 4) != 0) {
    ::close(listenFd_);
    listenFd_ = -1;
    return DebugEndpointError::BindFailed;
  }
  socklen_t len = sizeof a;
  ::getsockname(listenFd_, (sockaddr*)&a, &len);
  port_ = ntohs(a.sin_port);
  running_ = true;
  thread_ = std::thread([this]() { Run(); });
  return DebugEndpointError::Ok;
}

void DebugEndpoint::Run() {
  while (running_.load()) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(listenFd_, &fds);
    timeval tv{};
    tv.tv_usec = 100 * 1000;
    int r = ::select(listenFd_ + 1, &fds, nullptr, nullptr, &tv);
    if (r <= 0) continue;
    int c = ::accept(listenFd_, nullptr, nullptr);
    if (c < 0) continue;
    char buf[2048] = {0};
    ssize_t n = ::recv(c, buf, sizeof buf - 1, 0);
    if (n > 0) {
      std::string body = HandleRequest(std::string(buf, (size_t)n), token_,
                                       &MetricsRegistry::Instance());
      std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                         "Content-Length: " + std::to_string(body.size()) +
                         "\r\nConnection: close\r\n\r\n" + body;
      ssize_t wr = ::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
      (void)wr;
    }
    ::close(c);
  }
}

void DebugEndpoint::Stop() {
  if (!running_.exchange(false)) return;
  if (listenFd_ >= 0) {
    ::close(listenFd_);   // unblocks the pending accept
    listenFd_ = -1;
  }
  if (thread_.joinable()) thread_.join();
  port_ = 0;
}

}  // namespace Berry
