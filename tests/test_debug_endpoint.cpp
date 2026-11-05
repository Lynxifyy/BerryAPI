// Host tests for the loopback metrics debug endpoint (improvement 91).
#include "berry/debug_endpoint.h"
#include <cstdio>
#include <string>
using namespace Berry;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
int main() {
  // Constant-time compare is correct.
  CHECK(DebugEndpoint::ConstantTimeEquals("abc", "abc"));
  CHECK(!DebugEndpoint::ConstantTimeEquals("abc", "abd"));
  CHECK(!DebugEndpoint::ConstantTimeEquals("abc", "abcd"));

  MetricsRegistry& reg = MetricsRegistry::Instance();
  reg.Clear();
  reg.CounterFor("combat.hits")->Add(7);
  reg.GaugeFor("world.entities")->Set(3);
  reg.HistogramFor("frame.time_ms")->Observe(4.0);

  // A request without the token is refused.
  CHECK(DebugEndpoint::HandleRequest("GET /metrics HTTP/1.1\r\n\r\n", "s3cr3t", &reg)
            .find("unauthorized") != std::string::npos);
  // A wrong token is refused.
  CHECK(DebugEndpoint::HandleRequest("GET /metrics?token=wrong HTTP/1.1\r\n\r\n", "s3cr3t", &reg)
            .find("unauthorized") != std::string::npos);
  // The right token (query form) returns the snapshot.
  {
    std::string body = DebugEndpoint::HandleRequest(
        "GET /metrics?token=s3cr3t HTTP/1.1\r\n\r\n", "s3cr3t", &reg);
    CHECK(body.find("combat.hits") != std::string::npos);
    CHECK(body.find("world.entities") != std::string::npos);
    CHECK(body.find("frame.time_ms") != std::string::npos);
  }
  // The right token (header form).
  {
    std::string body = DebugEndpoint::HandleRequest(
        "GET /metrics HTTP/1.1\r\nX-Berry-Token: s3cr3t\r\n\r\n", "s3cr3t", &reg);
    CHECK(body.find("combat.hits") != std::string::npos);
  }

  // A non-loopback bind is refused; an empty token is refused.
  {
    DebugEndpoint ep;
    DebugEndpointConfig bad;
    bad.bindAddr = "0.0.0.0";
    bad.token = "x";
    CHECK(ep.Start(bad) == DebugEndpointError::NotLoopback);
    DebugEndpointConfig noTok;
    noTok.bindAddr = "127.0.0.1";
    CHECK(ep.Start(noTok) == DebugEndpointError::NoToken);
    // A loopback start works and stops cleanly.
    DebugEndpointConfig ok;
    ok.bindAddr = "127.0.0.1";
    ok.token = "t";
    CHECK(ep.Start(ok) == DebugEndpointError::Ok);
    CHECK(ep.running());
    CHECK(ep.port() != 0);
    ep.Stop();
    CHECK(!ep.running());
  }

  if (g_fail == 0) printf("test_debug_endpoint: ALL OK\n");
  return g_fail;
}
