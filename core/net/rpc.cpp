// Local RPC & REPL. See include/berry/rpc.h for the model.
#include "berry/rpc.h"
#include "berry/net.h"

#include "../script/json_min.h"
#include "../script/json_write.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>

namespace Berry {

namespace jsonw = detail::jsonw;

namespace {

bool IsLoopbackHost(const std::string& host) {
  return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

/// Builds a JSON-RPC error frame. `idJson` is raw JSON; empty means "no id",
/// which is what a parse error carries.
std::string ErrorFrame(const std::string& idJson, int code,
                       const std::string& message) {
  std::string out;
  jsonw::Writer w(&out);
  w.BeginObject();
  w.Member("jsonrpc", std::string("2.0"));
  w.Key("id");
  if (idJson.empty()) w.Null(); else w.Raw(idJson);
  w.Key("error");
  w.BeginObject();
  w.Member("code", (i64)code);
  w.Member("message", message);
  w.EndObject();
  w.EndObject();
  return out;
}

/// Builds a JSON-RPC success frame. `idJson` and `resultJson` are emitted
/// verbatim.
std::string ResultFrame(const std::string& idJson, const std::string& resultJson) {
  std::string out;
  jsonw::Writer w(&out);
  w.BeginObject();
  w.Member("jsonrpc", std::string("2.0"));
  w.Key("id");
  w.Raw(idJson.empty() ? std::string("null") : idJson);
  w.Key("result");
  w.Raw(resultJson.empty() ? std::string("null") : resultJson);
  w.EndObject();
  return out;
}

/// Re-serialises an `id` verbatim, so a number stays a number and a string
/// stays a string, per the spec's "the id is echoed exactly".
std::string IdToJson(const detail::json::Value& v) {
  return jsonw::Dump(v);
}

}  // namespace

//----------------------------------------------------------------------------
// RpcServer
//----------------------------------------------------------------------------

RpcServer::RpcServer() = default;
RpcServer::~RpcServer() { Stop(); }

bool RpcServer::Register(const std::string& method, RpcHandler handler) {
  if (method.empty() || !handler) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  if (handlers_.find(method) == handlers_.end() && handlers_.size() >= kMaxMethods) {
    return false;
  }
  handlers_[method] = std::move(handler);
  return true;
}

bool RpcServer::Unregister(const std::string& method) {
  std::lock_guard<std::mutex> lk(mutex_);
  return handlers_.erase(method) > 0;
}

bool RpcServer::Has(const std::string& method) const {
  std::lock_guard<std::mutex> lk(mutex_);
  return handlers_.find(method) != handlers_.end();
}

std::vector<std::string> RpcServer::Methods() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<std::string> out;
  out.reserve(handlers_.size());
  for (const auto& kv : handlers_) out.push_back(kv.first);
  return out;
}

u64 RpcServer::framesHandled() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return framesHandled_;
}

bool RpcServer::HandleFrame(const std::string& frame, std::string* response) {
  {
    std::lock_guard<std::mutex> lk(mutex_);
    ++framesHandled_;
  }

  if (frame.size() > kMaxFrameBytes) {
    if (response) *response = ErrorFrame("", rpc_error::kInvalidRequest,
                                         "frame too large");
    return true;
  }

  detail::json::Value root;
  std::string jerr;
  if (!detail::json::Parse(frame, &root, &jerr) || !root.IsObject()) {
    if (response) *response = ErrorFrame("", rpc_error::kParseError,
                                         "parse error");
    return true;   // a parse error is answered
  }

  const detail::json::Value* method = root.Get("method");
  if (!method || !method->IsString() || method->str.empty()) {
    if (response) *response = ErrorFrame("", rpc_error::kInvalidRequest,
                                         "missing method");
    return true;
  }

  const detail::json::Value* idv = root.Get("id");
  const bool hasId = idv != nullptr && !idv->IsNull();
  const std::string id = hasId ? IdToJson(*idv) : std::string();

  // A notification (no id) gets no response, even on error, per the spec.
  auto reply = [&](const std::string& frameOut) {
    if (!hasId) return false;
    if (response) *response = frameOut;
    return true;
  };

  // `params` is passed to the handler verbatim; the handler owns its meaning.
  std::string params = "[]";
  if (const detail::json::Value* p = root.Get("params")) {
    // Re-serialise from the parsed tree so a handler always sees valid JSON.
    params = jsonw::Dump(*p);
  }

  RpcHandler handler;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = handlers_.find(method->str);
    if (it != handlers_.end()) handler = it->second;
  }
  if (!handler) {
    return reply(ErrorFrame(id, rpc_error::kMethodNotFound,
                            "method not found: " + method->str));
  }

  RpcResult result;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
  // A handler is embedder code; a throw must become an error reply, not a crash.
  try {
    result = handler(params);
  } catch (const std::exception& e) {
    result = RpcResult::Err(rpc_error::kInternalError, e.what());
  } catch (...) {
    result = RpcResult::Err(rpc_error::kInternalError, "handler threw");
  }
#else
  // The on-device build compiles with exceptions disabled, so a throw here
  // cannot be caught (it would call std::terminate either way). Call straight
  // through and document that a handler must not throw in that build.
  result = handler(params);
#endif

  if (!result.ok) {
    return reply(ErrorFrame(id, result.errorCode, result.errorMessage));
  }
  return reply(ResultFrame(id, result.result));
}

int RpcServer::Start(u16 port, const std::string& host) {
  if (!IsLoopbackHost(host)) return -1;   // loopback only, by design
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (running_) return -1;
  }

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(port);
  if (::bind(fd, (sockaddr*)&a, sizeof a) != 0) {
    ::close(fd);
    return -1;
  }
  if (::listen(fd, 4) != 0) {
    ::close(fd);
    return -1;
  }
  socklen_t len = sizeof a;
  ::getsockname(fd, (sockaddr*)&a, &len);
  {
    std::lock_guard<std::mutex> lk(mutex_);
    listenFd_ = fd;
    port_ = ntohs(a.sin_port);
    running_ = true;
  }
  thread_ = std::thread([this]() { ServeLoop(); });
  return (int)port_;
}

void RpcServer::Stop() {
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!running_) return;
    running_ = false;
  }
  // The listener thread wakes on its select timeout and sees running_ == false.
  if (thread_.joinable()) thread_.join();
  int fd = -1;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    fd = listenFd_;
    listenFd_ = -1;
    port_ = 0;
  }
  if (fd >= 0) ::close(fd);
}

u16 RpcServer::port() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return port_;
}

void RpcServer::ServeLoop() {
  while (true) {
    int fd = -1;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      if (!running_) break;
      fd = listenFd_;
    }
    if (fd < 0) break;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    timeval tv{};
    tv.tv_usec = 100 * 1000;   // 100ms wakeups so Stop() can join promptly
    int r = ::select(fd + 1, &fds, nullptr, nullptr, &tv);
    if (r <= 0) continue;
    int conn = ::accept(fd, nullptr, nullptr);
    if (conn < 0) continue;
    ServeConnection(conn);
    ::close(conn);
  }
}

void RpcServer::ServeConnection(int conn) {
  std::string buf;
  RateLimiter limiter;
  limiter.Reset((size_t)kRequestsPerSecond, kRequestsPerSecond);
  // A connection is served until the peer closes, an oversized frame arrives,
  // or the server stops.
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(mutex_);
      if (!running_) return;
    }
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(conn, &fds);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200 * 1000;
    int r = ::select(conn + 1, &fds, nullptr, nullptr, &tv);
    if (r < 0) return;
    if (r == 0) continue;   // timeout: re-check running_

    char chunk[4096];
    ssize_t n = ::recv(conn, chunk, sizeof chunk, 0);
    if (n <= 0) return;   // peer closed or error
    buf.append(chunk, (size_t)n);

    // A newline delimits a frame. Multiple frames in one read are each handled.
    for (;;) {
      size_t nl = buf.find('\n');
      if (nl == std::string::npos) break;
      std::string frame = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      if (!frame.empty() && frame.back() == '\r') frame.pop_back();
      if (frame.empty()) continue;

      std::string response;
      if (!limiter.Allow()) {
        // Over budget: answer with a rate-limit error and keep the connection,
        // so a tooling client sees a clear error rather than a silent drop.
        response = ErrorFrame("", rpc_error::kRateLimited, "rate limited");
      } else {
        HandleFrame(frame, &response);
      }
      if (!response.empty()) {
        response.push_back('\n');
        size_t off = 0;
        while (off < response.size()) {
          ssize_t sent = ::send(conn, response.data() + off, response.size() - off, 0);
          if (sent <= 0) return;
          off += (size_t)sent;
        }
        response.clear();
      }
    }
    // An unterminated frame that has grown past the cap is a client that will
    // never send a newline; drop it rather than buffer without bound.
    if (buf.size() > kMaxFrameBytes) return;
  }
}

//----------------------------------------------------------------------------
// RpcRepl
//----------------------------------------------------------------------------

bool RpcRepl::ParseLine(const std::string& line, std::string* method,
                        std::string* paramsJson) {
  size_t i = 0;
  while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
  if (i >= line.size()) return false;
  size_t start = i;
  while (i < line.size() && !std::isspace((unsigned char)line[i])) ++i;
  const std::string name = line.substr(start, i - start);
  if (name.empty()) return false;

  // The rest of the line is the argument list, split on whitespace and turned
  // into a JSON array of strings. A caller that needs a typed argument passes
  // JSON itself; this keeps the common case (`world.query 20`) typable.
  std::vector<std::string> args;
  while (i < line.size()) {
    while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
    if (i >= line.size()) break;
    size_t s = i;
    while (i < line.size() && !std::isspace((unsigned char)line[i])) ++i;
    args.push_back(line.substr(s, i - s));
  }

  std::string params = "[";
  for (size_t k = 0; k < args.size(); ++k) {
    if (k) params.push_back(',');
    const std::string& a = args[k];
    // A bare number or a JSON literal is passed through as-is so a handler can
    // receive a number; anything else becomes a quoted string.
    bool numeric = !a.empty();
    size_t dots = 0;
    for (size_t j = 0; j < a.size(); ++j) {
      const char c = a[j];
      if (c == '-' && j == 0) continue;
      if (c == '.') { ++dots; continue; }
      if (!std::isdigit((unsigned char)c)) { numeric = false; break; }
    }
    if (numeric && dots <= 1) {
      params += a;
    } else if (a == "true" || a == "false" || a == "null") {
      params += a;
    } else {
      jsonw::String(&params, a);
    }
  }
  params.push_back(']');

  if (method) *method = name;
  if (paramsJson) *paramsJson = params;
  return true;
}

std::vector<std::string> RpcRepl::Commands() const {
  std::vector<std::string> out;
  out.push_back("help");
  out.push_back("methods");
  if (server_) {
    for (const std::string& m : server_->Methods()) out.push_back(m);
  }
  return out;
}

std::string RpcRepl::Eval(const std::string& line) {
  std::string method;
  std::string params;
  if (!ParseLine(line, &method, &params)) return "";

  if (method == "methods") {
    if (!server_) return "(no server)";
    std::string out;
    for (const std::string& m : server_->Methods()) {
      if (!out.empty()) out += "\n";
      out += m;
    }
    return out.empty() ? "(no methods)" : out;
  }
  if (method == "help") {
    std::string out = "commands:";
    for (const std::string& m : Commands()) out += "\n  " + m;
    return out;
  }
  if (!server_) return "error: no server";

  std::string request;
  {
    jsonw::Writer w(&request);
    w.BeginObject();
    w.Member("jsonrpc", std::string("2.0"));
    w.Member("id", std::string("repl"));
    w.Member("method", method);
    w.Key("params");
    w.Raw(params);
    w.EndObject();
  }

  std::string response;
  if (!server_->HandleFrame(request, &response)) return "";
  return response;
}

}  // namespace Berry
