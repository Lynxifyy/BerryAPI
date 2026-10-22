// Script-facing WebSocket client. See include/berry/ws_client.h.
//
// This wraps Berry::Network's WebSocket so a script can open a connection
// without ever seeing a socket. The security posture is the same as the HTTP
// path: the destination must pass the URL policy and be covered by the
// script's network grant, and `wss://` fails closed because TLS is not
// implemented.
//
// Threading: connections are normally driven from the script worker. A
// connection is held behind a shared_ptr so an unload (which may run on the
// detach thread) can close it while a blocking Pump/Connect on the script
// worker still holds a reference — the Conn outlives the map entry and the
// callbacks capture the shared_ptr, so nothing dangles.
#include "berry/ws_client.h"

#include "berry/logger.h"
#include "berry/net.h"
#include "berry/url.h"

#include <vector>

namespace Berry {

namespace {

const char* WsOpcodeName(u8 op) {
  switch ((WsOpcode)op) {
    case WsOpcode::Text: return "text";
    case WsOpcode::Binary: return "binary";
    case WsOpcode::Close: return "close";
    case WsOpcode::Ping: return "ping";
    case WsOpcode::Pong: return "pong";
    case WsOpcode::Continuation: return "continuation";
  }
  return "unknown";
}

/// The host of a `ws://` URL, lower-cased and bracket-stripped, so the grant
/// check sees the same spelling the HTTP path would. Empty when the URL is not
/// a `ws://` URL with a usable host.
std::string WsHost(const std::string& url) {
  const std::string kScheme = "ws://";
  if (url.rfind(kScheme, 0) != 0) return std::string();
  std::string rest = url.substr(kScheme.size());
  const size_t slash = rest.find_first_of("/?#");
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  if (authority.empty() || authority.find('@') != std::string::npos) {
    return std::string();
  }
  std::string host = authority;
  if (authority.front() == '[') {
    const size_t close = authority.find(']');
    if (close == std::string::npos) return std::string();
    host = authority.substr(1, close - 1);
  } else {
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) host = authority.substr(0, colon);
  }
  for (char& c : host) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return host;
}

/// The egress check for a WebSocket URL: a ws:// URL, a usable host, and the
/// same host grant the HTTP path enforces. The URL policy is written for
/// http/https, so the equivalent ws rule (no IP literal, no private/reserved
/// address, unless the harness opted into loopback) is applied here directly
/// rather than pretending the URL is http.
WsClientError CheckDestination(const std::string& url,
                               const CapabilitySet& granted,
                               const UrlPolicy& policy, std::string* host) {
  if (url.rfind("wss://", 0) == 0) return WsClientError::BadUrl;  // TLS absent
  const std::string h = WsHost(url);
  if (h.empty()) return WsClientError::BadUrl;
  const bool loopbackHttp = policy.allowLoopbackHttp && IsLoopbackHost(h);
  if (!loopbackHttp) {
    if (IsIpLiteral(h) && !policy.allowIpLiterals) return WsClientError::BadUrl;
    if (IsPrivateOrReservedHost(h) && !policy.allowPrivateRanges) {
      return WsClientError::BadUrl;
    }
  }
  if (!HostCoveredByGrant(granted, h)) return WsClientError::NotGranted;
  if (host) *host = h;
  return WsClientError::Ok;
}

}  // namespace

const char* WsClientErrorName(WsClientError e) {
  switch (e) {
    case WsClientError::Ok: return "ok";
    case WsClientError::BadHandle: return "bad-handle";
    case WsClientError::BadUrl: return "bad-url";
    case WsClientError::NotGranted: return "not-granted";
    case WsClientError::ConnectFailed: return "connect-failed";
    case WsClientError::SendFailed: return "send-failed";
    case WsClientError::Closed: return "closed";
    case WsClientError::TooManySockets: return "too-many-sockets";
    case WsClientError::NotOpen: return "not-open";
  }
  return "?";
}

WsClientPool::WsClientPool() = default;
WsClientPool::~WsClientPool() { CloseAll(); }

std::shared_ptr<WsClientPool::Conn> WsClientPool::Find(u64 handle) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = conns_.find(handle);
  return it == conns_.end() ? nullptr : it->second;
}

u64 WsClientPool::Connect(const std::string& url, const CapabilitySet& granted,
                          const UrlPolicy& policy, int timeoutMs,
                          WsClientError* err) {
  auto fail = [err](WsClientError e) {
    if (err) *err = e;
    return (u64)0;
  };
  std::string host;
  const WsClientError dest = CheckDestination(url, granted, policy, &host);
  if (dest != WsClientError::Ok) return fail(dest);

  auto conn = std::make_shared<Conn>();
  conn->url = url;
  conn->host = host;
  conn->sock = std::unique_ptr<WebSocket>(new WebSocket());
  {
    // Reserve the slot and the handle under one lock so two concurrent
    // connects cannot both pass the cap check and overshoot kMaxSockets.
    std::lock_guard<std::mutex> lk(mutex_);
    if (conns_.size() >= kMaxSockets) return fail(WsClientError::TooManySockets);
    conn->handle = nextHandle_++;
    conns_[conn->handle] = conn;
  }
  // The callbacks fire on the thread calling Pump (the script worker) and only
  // touch this connection's inbox. They capture the shared_ptr, so a concurrent
  // CloseAll cannot leave them pointing at freed memory.
  std::shared_ptr<Conn> self = conn;
  conn->sock->OnMessage([self](const WsMessage& msg) {
    WsClientMessage m;
    m.opcode = WsOpcodeName((u8)msg.opcode);
    m.binary = msg.opcode == WsOpcode::Binary;
    m.data.assign(msg.payload.begin(), msg.payload.end());
    self->inbox.push_back(std::move(m));
    while (self->inbox.size() > kMaxInbox) self->inbox.pop_front();
  });
  conn->sock->OnClose([self](int, const std::string& reason) {
    self->open = false;
    self->error = reason;
  });

  if (!conn->sock->Connect(url.c_str(), timeoutMs)) {
    std::lock_guard<std::mutex> lk(mutex_);
    conns_.erase(conn->handle);
    return fail(WsClientError::ConnectFailed);
  }
  conn->open = true;
  if (err) *err = WsClientError::Ok;
  return conn->handle;
}

bool WsClientPool::SendText(u64 handle, const std::string& text,
                            WsClientError* err) {
  std::shared_ptr<Conn> c = Find(handle);
  if (!c || !c->sock) {
    if (err) *err = WsClientError::BadHandle;
    return false;
  }
  if (!c->open.load()) {
    if (err) *err = WsClientError::NotOpen;
    return false;
  }
  if (!c->sock->SendText(text.c_str())) {
    if (err) *err = WsClientError::SendFailed;
    return false;
  }
  if (err) *err = WsClientError::Ok;
  return true;
}

bool WsClientPool::SendBinary(u64 handle, const void* data, size_t len,
                              WsClientError* err) {
  std::shared_ptr<Conn> c = Find(handle);
  if (!c || !c->sock) {
    if (err) *err = WsClientError::BadHandle;
    return false;
  }
  if (!c->open.load()) {
    if (err) *err = WsClientError::NotOpen;
    return false;
  }
  if (!c->sock->SendBinary(data, len)) {
    if (err) *err = WsClientError::SendFailed;
    return false;
  }
  if (err) *err = WsClientError::Ok;
  return true;
}

size_t WsClientPool::Pump() {
  // Snapshot the connections so the blocking reads run outside the lock and a
  // concurrent CloseAll cannot invalidate the iteration.
  std::vector<std::shared_ptr<Conn>> conns;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    conns.reserve(conns_.size());
    for (auto& kv : conns_) conns.push_back(kv.second);
  }
  size_t read = 0;
  for (auto& c : conns) {
    if (!c || !c->sock || !c->open.load()) continue;
    const size_t before = c->inbox.size();
    c->sock->Pump();
    if (!c->sock->IsOpen()) c->open = false;
    read += c->inbox.size() - before;
  }
  return read;
}

bool WsClientPool::Poll(u64 handle, WsClientMessage* out) {
  std::shared_ptr<Conn> c = Find(handle);
  if (!c || c->inbox.empty()) return false;
  if (out) *out = c->inbox.front();
  c->inbox.pop_front();
  return true;
}

size_t WsClientPool::Pending(u64 handle) const {
  std::shared_ptr<Conn> c = Find(handle);
  return c ? c->inbox.size() : 0;
}

bool WsClientPool::Close(u64 handle) {
  std::shared_ptr<Conn> c;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = conns_.find(handle);
    if (it == conns_.end()) return false;
    c = it->second;
    conns_.erase(it);
  }
  if (c && c->sock) c->sock->Close();
  return true;
}

void WsClientPool::CloseAll() {
  std::vector<std::shared_ptr<Conn>> conns;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    conns.reserve(conns_.size());
    for (auto& kv : conns_) conns.push_back(kv.second);
    conns_.clear();
  }
  for (auto& c : conns) {
    if (c && c->sock) c->sock->Close();
  }
}

bool WsClientPool::IsOpen(u64 handle) const {
  std::shared_ptr<Conn> c = Find(handle);
  return c && c->open.load();
}

size_t WsClientPool::Count() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return conns_.size();
}

std::string WsClientPool::Url(u64 handle) const {
  std::shared_ptr<Conn> c = Find(handle);
  return c ? c->url : std::string();
}

std::string WsClientPool::LastError(u64 handle) const {
  std::shared_ptr<Conn> c = Find(handle);
  return c ? c->error : std::string();
}

}  // namespace Berry
