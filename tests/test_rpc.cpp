// Host tests for the local RPC server and REPL.
//
// Covers:
//   * The dispatcher: a successful call, a notification (no reply), method not
//     found, a handler error, a handler that throws, a malformed frame, a
//     missing method, and the frame-size cap.
//   * Registration: replace, unregister, the method cap.
//   * The loopback listener: a real client round-trips a call over TCP, and a
//     non-loopback bind is refused.
//   * The REPL: line parsing (numbers vs strings), help/methods, and a call.
#include "berry/rpc.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

/// A tiny loopback client: connects, sends one newline-delimited frame, reads
/// one newline-delimited reply.
std::string CallOnce(u16 port, const std::string& frame) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return "";
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(port);
  if (::connect(fd, (sockaddr*)&a, sizeof a) != 0) { ::close(fd); return ""; }
  std::string out = frame + "\n";
  ::send(fd, out.data(), out.size(), 0);

  std::string reply;
  char buf[4096];
  for (int i = 0; i < 50; ++i) {
    ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) break;
    reply.append(buf, (size_t)n);
    if (reply.find('\n') != std::string::npos) break;
  }
  ::close(fd);
  const size_t nl = reply.find('\n');
  return nl == std::string::npos ? reply : reply.substr(0, nl);
}

}  // namespace

//----------------------------------------------------------------------------
// Dispatcher
//----------------------------------------------------------------------------

static void TestDispatcher() {
  RpcServer srv;
  CHECK(srv.Register("echo", [](const std::string& params) {
    return RpcResult::Ok(params);
  }));
  CHECK(srv.Register("boom", [](const std::string&) -> RpcResult {
    throw std::runtime_error("kaboom");
  }));
  CHECK(srv.Register("deny", [](const std::string&) {
    return RpcResult::Err(-32001, "nope");
  }));
  CHECK(srv.Has("echo"));
  CHECK(srv.Methods().size() == 3);
  CHECK(!srv.Register("", [](const std::string&) { return RpcResult::Ok("null"); }));

  std::string out;

  // A successful call.
  CHECK(srv.HandleFrame("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"echo\",\"params\":[1,2]}", &out));
  CHECK(out.find("\"id\":1") != std::string::npos);
  CHECK(out.find("\"result\":[1,2]") != std::string::npos);

  // A string id round-trips.
  CHECK(srv.HandleFrame("{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":\"echo\",\"params\":[]}", &out));
  CHECK(out.find("\"id\":\"abc\"") != std::string::npos);

  // A notification (no id) gets no reply.
  out.clear();
  CHECK(!srv.HandleFrame("{\"jsonrpc\":\"2.0\",\"method\":\"echo\",\"params\":[]}", &out));
  CHECK(out.empty());

  // A notification to a missing method also gets no reply.
  CHECK(!srv.HandleFrame("{\"jsonrpc\":\"2.0\",\"method\":\"ghost\"}", &out));

  // Method not found.
  CHECK(srv.HandleFrame("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ghost\"}", &out));
  CHECK(out.find("-32601") != std::string::npos);

  // A handler error.
  CHECK(srv.HandleFrame("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"deny\"}", &out));
  CHECK(out.find("-32001") != std::string::npos);
  CHECK(out.find("nope") != std::string::npos);

  // A handler that throws becomes an internal error, not a crash.
  CHECK(srv.HandleFrame("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"boom\"}", &out));
  CHECK(out.find("-32603") != std::string::npos);
  CHECK(out.find("kaboom") != std::string::npos);

  // Malformed frames.
  CHECK(srv.HandleFrame("not json", &out));
  CHECK(out.find("-32700") != std::string::npos);
  CHECK(srv.HandleFrame("{}", &out));
  CHECK(out.find("-32600") != std::string::npos);
  CHECK(srv.HandleFrame("{\"id\":1,\"method\":\"\"}", &out));
  CHECK(out.find("-32600") != std::string::npos);

  // The frame-size cap.
  std::string huge(RpcServer::kMaxFrameBytes + 1, 'x');
  CHECK(srv.HandleFrame(huge, &out));
  CHECK(out.find("-32600") != std::string::npos);

  // Unregister.
  CHECK(srv.Unregister("echo"));
  CHECK(!srv.Has("echo"));
  CHECK(!srv.Unregister("echo"));

  // A params field that is absent defaults to an empty array.
  CHECK(srv.HandleFrame("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"deny\"}", &out));
  CHECK(out.find("-32001") != std::string::npos);

  CHECK(srv.framesHandled() > 0);
}

static void TestMethodCap() {
  RpcServer srv;
  for (size_t i = 0; i < RpcServer::kMaxMethods; ++i) {
    CHECK(srv.Register("m" + std::to_string(i),
                       [](const std::string&) { return RpcResult::Ok("null"); }));
  }
  // At the cap, a new name is refused...
  CHECK(!srv.Register("overflow", [](const std::string&) { return RpcResult::Ok("null"); }));
  // ...but replacing an existing one still works.
  CHECK(srv.Register("m0", [](const std::string&) { return RpcResult::Ok("null"); }));
  CHECK(srv.Methods().size() == RpcServer::kMaxMethods);
}

//----------------------------------------------------------------------------
// Loopback listener
//----------------------------------------------------------------------------

static void TestLoopbackListener() {
  RpcServer srv;
  CHECK(srv.Register("ping", [](const std::string&) {
    return RpcResult::Ok("\"pong\"");
  }));

  // A non-loopback bind is refused.
  CHECK(srv.Start(0, "0.0.0.0") == -1);
  CHECK(!srv.running());

  const int port = srv.Start(0);
  CHECK(port > 0);
  CHECK(srv.running());
  CHECK(srv.port() == (u16)port);

  const std::string reply = CallOnce((u16)port,
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}");
  CHECK(reply.find("\"result\":\"pong\"") != std::string::npos);

  // A second call on a fresh connection still works (the server loops).
  const std::string reply2 = CallOnce((u16)port,
      "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ghost\"}");
  CHECK(reply2.find("-32601") != std::string::npos);

  srv.Stop();
  CHECK(!srv.running());
  CHECK(srv.port() == 0);
}

//----------------------------------------------------------------------------
// REPL
//----------------------------------------------------------------------------

static void TestRepl() {
  std::string method, params;
  CHECK(RpcRepl::ParseLine("world.query 20", &method, &params));
  CHECK(method == "world.query");
  CHECK(params == "[20]");

  CHECK(RpcRepl::ParseLine("combat.damage 5 -2.5 true name", &method, &params));
  CHECK(method == "combat.damage");
  CHECK(params == "[5,-2.5,true,\"name\"]");

  CHECK(RpcRepl::ParseLine("noargs", &method, &params));
  CHECK(method == "noargs");
  CHECK(params == "[]");

  CHECK(!RpcRepl::ParseLine("   ", &method, &params));

  RpcServer srv;
  srv.Register("add", [](const std::string& params) {
    // A handler that echoes its params so the test can see them.
    return RpcResult::Ok(params);
  });
  RpcRepl repl(&srv);
  CHECK(repl.Commands().size() == 3);   // help, methods, add

  const std::string r = repl.Eval("add 1 2");
  CHECK(r.find("\"result\":[1,2]") != std::string::npos);

  const std::string m = repl.Eval("methods");
  CHECK(m.find("add") != std::string::npos);

  const std::string h = repl.Eval("help");
  CHECK(h.find("add") != std::string::npos);

  const std::string missing = repl.Eval("ghost");
  CHECK(missing.find("-32601") != std::string::npos);

  CHECK(repl.Eval("").empty());

  RpcRepl none(nullptr);
  CHECK(none.Eval("add 1").find("no server") != std::string::npos);
}

int main() {
  TestDispatcher();
  TestMethodCap();
  TestLoopbackListener();
  TestRepl();
  if (g_fail == 0) printf("test_rpc: ALL OK\n");
  return g_fail;
}
