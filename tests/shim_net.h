#pragma once
#include "berry/types.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

using Berry::u8;
using Berry::u16;
using Berry::u64;

//----------------------------------------------------------------------------
// Tiny single-thread test servers (loopback HTTP + minimal WebSocket echo)
// for exercising Berry::Network without external services.
//----------------------------------------------------------------------------

class TinyTcpAcceptor {
 public:
  TinyTcpAcceptor() = default;
  ~TinyTcpAcceptor() { Stop(); }

  int Start(u16 port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return -1;
    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::bind(fd_, (sockaddr*)&a, sizeof a) != 0) return -1;
    if (::listen(fd_, 8) != 0) return -1;
    socklen_t len = sizeof a;
    getsockname(fd_, (sockaddr*)&a, &len);
    running_ = true;
    thread_ = std::thread([this]() {
      while (running_) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);
        timeval tv{};
        tv.tv_usec = 100 * 1000;   // 100ms wakeups so Stop() can join
        int r = ::select(fd_ + 1, &fds, nullptr, nullptr, &tv);
        if (r <= 0) continue;      // timeout or error -> re-check running_
        int c = ::accept(fd_, nullptr, nullptr);
        if (c < 0) continue;
        Handle(c);
        ::close(c);
      }
    });
    return (int)ntohs(a.sin_port);
  }

  void Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
  }

 protected:
  virtual void Handle(int conn) = 0;

  std::string RecvAll(int conn, int timeoutMs, size_t cap = 1 << 20) {
    std::string data;
    fd_set fds;
    timeval tv{};
    while (data.size() < cap) {
      FD_ZERO(&fds); FD_SET(conn, &fds);
      tv.tv_sec = timeoutMs / 1000;
      tv.tv_usec = (timeoutMs % 1000) * 1000;
      int r = ::select(conn + 1, &fds, nullptr, nullptr, &tv);
      if (r <= 0) break;
      char buf[4096];
      ssize_t n = ::recv(conn, buf, sizeof buf, 0);
      if (n <= 0) break;
      data.append(buf, (size_t)n);
    }
    return data;
  }

  // Reads until `token` is present or wraparound deadline expires.
  static std::string RecvUntil(int conn, const std::string& token, int totalMs) {
    std::string out;
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(totalMs);
    while (out.size() < (1u << 20)) {
      fd_set fds;
      FD_ZERO(&fds); FD_SET(conn, &fds);
      timeval tv{};
      tv.tv_sec = 0; tv.tv_usec = 200 * 1000;
      int r = ::select(conn + 1, &fds, nullptr, nullptr, &tv);
      if (r <= 0) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        continue;
      }
      ssize_t n = ::recv(conn, buf, sizeof buf, 0);
      if (n <= 0) break;
      out.append(buf, (size_t)n);
      if (out.find(token) != std::string::npos) return out;
    }
    return out;
  }

  // Reads exactly `want` bytes with an overall deadline.
  static bool RecvExactly(int conn, size_t want, std::string& out, int totalMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(totalMs);
    while (out.size() < want) {
      fd_set fds;
      FD_ZERO(&fds); FD_SET(conn, &fds);
      timeval tv{};
      tv.tv_sec = 0; tv.tv_usec = 200 * 1000;
      int r = ::select(conn + 1, &fds, nullptr, nullptr, &tv);
      if (r <= 0) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        continue;
      }
      char buf[4096];
      ssize_t n = ::recv(conn, buf, sizeof buf, 0);
      if (n <= 0) return false;
      out.append(buf, (size_t)n);
    }
    return true;
  }

  int Port() const { return port_; }
  std::atomic<bool> running_{false};
  int fd_ = -1;
  int port_ = 0;
  std::thread thread_;
};

class TinyHttpServer final : public TinyTcpAcceptor {
 protected:
  void Handle(int conn) override {
    std::string req = RecvAll(conn, 2000);
    if (req.empty()) return;
    bool post = req.rfind("POST", 0) == 0;
    std::string body = "pong";
    std::string ct = "text/plain";
    if (post) {
      body = req;   // echo request
      ct = "application/json";
    }
    std::string resp = "HTTP/1.1 200 OK\r\n"
        "Content-Type: " + ct + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;
    ::send(conn, resp.data(), resp.size(), MSG_NOSIGNAL);
  }
};

class TinyWsServer final : public TinyTcpAcceptor {
 protected:
  void Handle(int conn) override {
    // Handshake ends at the first blank line; don't slurp post-handshake frames.
    std::string handshake = RecvUntil(conn, "\r\n\r\n", 4000);
    if (handshake.find("Upgrade: websocket") == std::string::npos &&
        handshake.find("upgrade: websocket") == std::string::npos) return;
    std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    ::send(conn, resp.data(), resp.size(), MSG_NOSIGNAL);

    // Read exactly one frame (header + optional ext len + mask + payload).
    // Header: first 2 bytes always.
    std::string raw;
    if (!RecvExactly(conn, 2, raw, 4000)) return;
    unsigned char b1 = (unsigned char)raw[1];
    u64 len = b1 & 0x7f;
    bool masked = (b1 & 0x80) != 0;
    size_t header = 2;
    if (len == 126) {
      if (!RecvExactly(conn, 2, raw, 4000)) return;
      len = ((u64)(unsigned char)raw[2] << 8) | (unsigned char)raw[3];
      header = 4;
    } else if (len == 127) {
      if (!RecvExactly(conn, 8, raw, 4000)) return;
      len = 0;
      for (size_t i = 0; i < 8; ++i) len = (len << 8) | (unsigned char)raw[2 + i];
      header = 10;
    }
    unsigned char maskKey[4] = {0, 0, 0, 0};
    if (masked) {
      if (!RecvExactly(conn, 4, raw, 4000)) return;
      for (size_t i = 0; i < 4; ++i) maskKey[i] = (unsigned char)raw[header + i];
      header += 4;
    }
    if (len > 16u * 1024) return;
    // Any bytes in `raw` beyond the header are already payload.
    size_t need = (size_t)len;
    if (raw.size() > header) need = need > (raw.size() - header) ? 0 : need - (raw.size() - header);
    std::string payloadBody;
    if (raw.size() > header) payloadBody = raw.substr(header);
    std::string body;
    if (need && !RecvExactly(conn, need, body, 4000)) return;
    payloadBody += body;
    std::vector<u8> outP;
    for (size_t i = 0; i < (size_t)len; ++i)
      outP.push_back(masked ? (u8)(payloadBody[i] ^ maskKey[i % 4]) : (u8)payloadBody[i]);
    std::string payload((const char*)outP.data(), outP.size());

    // Echo back as a single unmasked TEXT frame.
    std::vector<u8> out;
    out.push_back(0x81);
    if (len < 126) out.push_back((u8)len);
    else if (len < 65536) { out.push_back(126); out.push_back((u8)(len >> 8)); out.push_back((u8)(len & 0xff)); }
    else { out.push_back(127); for (int i = 7; i >= 0; --i) out.push_back((u8)((len >> (8 * i)) & 0xff)); }
    if (!payload.empty()) out.insert(out.end(), payload.begin(), payload.end());
    ::send(conn, out.data(), out.size(), MSG_NOSIGNAL);
  }
};