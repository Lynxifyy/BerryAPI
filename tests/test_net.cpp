#include "berry/net.h"
#include "shim_net.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

int main() {
  // --- Rate limiter ---
  RateLimiter rl;
  rl.Reset(5, 5.0f);   // burst 5, 5/sec
  int allowed = 0;
  for (int i = 0; i < 10; ++i) if (rl.Allow()) ++allowed;
  CHECK(allowed == 5);

  // --- HTTP loopback ---
  TinyHttpServer server;
  int port = server.Start(18081);
  CHECK(port > 0);
  if (port > 0) {
    std::string base = "http://127.0.0.1:" + std::to_string(port);
    HttpRequest req((base + "/ping").c_str());
    req.Method("GET");
    HttpResponse resp = req.Do();
    CHECK(resp.status == 200);
    CHECK(resp.body.find("pong") != std::string::npos);

    // POST with JSON
    HttpRequest req2((base + "/echo").c_str());
    req2.Method("POST").JsonBody("{\"n\":1}");
    HttpResponse resp2 = req2.Do();
    CHECK(resp2.status == 200);
    CHECK(resp2.body.find("n") != std::string::npos);
    server.Stop();
  }

  // --- WebSocket loopback (echo) ---
  TinyWsServer ws;
  int wsPort = ws.Start(18082);
  CHECK(wsPort > 0);
  if (wsPort > 0) {
    WebSocket client;
    bool opened = false, gotMsg = false;
    std::string echoed;
    client.OnOpen([&]() { opened = true; });
    client.OnMessage([&](const WsMessage& msg) {
      if (msg.opcode == WsOpcode::Text) {
        echoed = msg.text();
        gotMsg = true;
      }
    });

    bool conn = client.Connect((std::string("ws://127.0.0.1:") + std::to_string(wsPort) + "/ws").c_str());
    CHECK(conn);
    if (conn) {
      CHECK(client.SendText("hello-ws"));
      // Give the server a moment then pump.
      for (int i = 0; i < 5 && !gotMsg; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        client.Pump();
      }
      CHECK(opened);
      CHECK(gotMsg);
      CHECK(echoed == "hello-ws");
      client.Close();
    }
    ws.Stop();
  }

  // --- JSON-RPC ---
  JsonRpcRequest req;
  CHECK(JsonRpc::ParseRequest("{\"jsonrpc\":\"2.0\",\"method\":\"hit\",\"params\":{\"d\":5.0},\"id\":7}", &req));
  CHECK(req.method == "hit");
  CHECK(req.id == "7");
  CHECK(req.hasId);
  auto notif = JsonRpc::MakeNotification("tick", "{\"n\":1}");
  CHECK(notif.find("\"method\":\"tick\"") != std::string::npos);

  if (g_fail == 0) printf("test_net: ALL OK\n");
  return g_fail;
}