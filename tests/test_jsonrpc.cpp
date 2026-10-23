// Host tests for the script-facing JSON-RPC 2.0 client.
//
// Covers:
//   * JsonDocumentToScriptValue: the JSON -> ScriptValue mapping (objects,
//     arrays, primitives, null).
//   * JsonRpcCallBlocking over a loopback server: a successful call decodes its
//     result into a table, a JSON-RPC error object is reported as an error, a
//     mismatched id is rejected, a non-JSON body is rejected, and a refused
//     destination (host not granted) is reported rather than sent.
//   * The egress rules are the HTTP ones: an ungranted host never reaches the
//     socket.
#include "berry/jsonrpc_client.h"
#include "berry/script.h"
#include "berry/url.h"
#include "shim_net.h"

#include <cstdio>
#include <string>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

CapabilitySet Grant(const std::string& cap) {
  CapabilitySet s;
  s.Add(cap);
  return s;
}

UrlPolicy LoopbackPolicy() {
  UrlPolicy p;
  p.allowIpLiterals = true;
  p.allowLoopbackHttp = true;
  return p;
}

}  // namespace

int main() {
  // --- JSON -> ScriptValue mapping ---
  {
    const std::string doc =
        "{\"s\":\"hi\",\"n\":3,\"b\":true,\"z\":null,"
        "\"arr\":[10,20],\"obj\":{\"k\":\"v\"}}";
    std::string err = "sentinel";
    const ScriptValue v = JsonTextToScriptValue(doc, &err);
    CHECK(err.empty());
    const ScriptTable* t = v.AsTable();
    CHECK(t != nullptr);
    if (t) {
      CHECK((*t).at("s").AsString() == "hi");
      CHECK((*t).at("n").AsNumber() == 3.0);
      CHECK((*t).at("b").Truthy());
      CHECK((*t).at("z").IsNil());
      const ScriptTable* arr = (*t).at("arr").AsTable();
      CHECK(arr != nullptr);
      if (arr) {
        CHECK((*arr).size() == 2);
        CHECK((*arr).at("0").AsNumber() == 10.0);
        CHECK((*arr).at("1").AsNumber() == 20.0);
      }
      const ScriptTable* obj = (*t).at("obj").AsTable();
      CHECK(obj != nullptr);
      if (obj) CHECK((*obj).at("k").AsString() == "v");
    }

    // Malformed JSON yields Nil and reports why, so a caller can fail closed.
    err.clear();
    const ScriptValue bad = JsonTextToScriptValue("{not json", &err);
    CHECK(bad.IsNil());
    CHECK(!err.empty());

    CHECK(JsonDocumentToScriptValue(nullptr).IsNil());
  }

  TinyJsonRpcServer server;
  int port = server.Start(18085);
  CHECK(port > 0);
  if (port > 0) {
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    // A granted host reaches the server; the result is a structured table.
    {
      ScriptContext ctx("t", Grant("network:127.0.0.1"));
      const JsonRpcOutcome out = JsonRpcCallBlocking(
          ctx, base + "/rpc", "add", "{\"a\":1,\"b\":2}", 4000, LoopbackPolicy());
      CHECK(out.ok);
      CHECK(out.http_status == 200);
      CHECK(!out.result_json.empty());
      CHECK(out.result_json.find("\"method\":\"add\"") != std::string::npos);
      CHECK(out.result_json.find("\"a\":1") != std::string::npos);
    }

    // Omitting params is allowed and the server sees a null echo.
    {
      ScriptContext ctx("t", Grant("network:127.0.0.1"));
      const JsonRpcOutcome out = JsonRpcCallBlocking(
          ctx, base + "/rpc", "ping", "", 4000, LoopbackPolicy());
      CHECK(out.ok);
      CHECK(out.result_json.find("\"echo\":null") != std::string::npos);
    }

    // A JSON-RPC error object is surfaced as an error, not a result.
    {
      ScriptContext ctx("t", Grant("network:127.0.0.1"));
      const JsonRpcOutcome out = JsonRpcCallBlocking(
          ctx, base + "/rpcerr", "ghost", "{}", 4000, LoopbackPolicy());
      CHECK(!out.ok);
      CHECK(out.error_code == -32601);
      CHECK(out.error_message == "method not found");
      CHECK(out.result_json.empty());
    }

    // A mismatched response id is rejected (spoofing/desync guard).
    {
      ScriptContext ctx("t", Grant("network:127.0.0.1"));
      const JsonRpcOutcome out = JsonRpcCallBlocking(
          ctx, base + "/rpcbadid", "add", "{}", 4000, LoopbackPolicy());
      CHECK(!out.ok);
      CHECK(out.transport_error.find("id mismatch") != std::string::npos);
    }

    // A non-JSON body is rejected rather than handed back as a value.
    {
      ScriptContext ctx("t", Grant("network:127.0.0.1"));
      const JsonRpcOutcome out = JsonRpcCallBlocking(
          ctx, base + "/rpcbadjson", "add", "{}", 4000, LoopbackPolicy());
      CHECK(!out.ok);
      CHECK(out.transport_error.find("bad response json") != std::string::npos);
    }

    // A host outside the grant never reaches the socket: the HTTP path refuses
    // it and the outcome reports the refusal.
    {
      ScriptContext ctx("t", Grant("network:example.com"));
      const JsonRpcOutcome out = JsonRpcCallBlocking(
          ctx, base + "/rpc", "add", "{}", 4000, LoopbackPolicy());
      CHECK(!out.ok);
      CHECK(out.transport_error.find("not granted") != std::string::npos);
    }

    // Invalid params JSON is refused before any request is sent.
    {
      ScriptContext ctx("t", Grant("network:127.0.0.1"));
      const JsonRpcOutcome out = JsonRpcCallBlocking(
          ctx, base + "/rpc", "add", "{not json", 4000, LoopbackPolicy());
      CHECK(!out.ok);
      CHECK(out.transport_error.find("bad params json") != std::string::npos);
    }

    // An empty method is refused up front.
    {
      ScriptContext ctx("t", Grant("network:127.0.0.1"));
      const JsonRpcOutcome out = JsonRpcCallBlocking(
          ctx, base + "/rpc", "", "{}", 4000, LoopbackPolicy());
      CHECK(!out.ok);
      CHECK(out.transport_error.find("method is empty") != std::string::npos);
    }

    // The default (strict) policy refuses a plaintext loopback destination, so
    // the call fails closed instead of silently downgrading.
    {
      ScriptContext ctx("t", Grant("network:127.0.0.1"));
      const JsonRpcOutcome out = JsonRpcCallBlocking(
          ctx, base + "/rpc", "add", "{}", 4000, UrlPolicy());
      CHECK(!out.ok);
      CHECK(!out.transport_error.empty());
    }

    server.Stop();
  }

  if (g_fail == 0) printf("test_jsonrpc: ALL OK\n");
  return g_fail;
}
