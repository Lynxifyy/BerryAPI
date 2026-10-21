// Host tests for URL parsing, the egress policy and network scope matching.
//
// http_get is an SSRF sink, so these tests are written from the attacker's
// side: every case asks "can a script with a network grant reach something it
// was not granted?" and expects no.
//
// Covers:
//   * Url::Parse shape (scheme/host/port/path, IPv6 brackets, default ports).
//   * Parser rejections: control chars, userinfo, missing scheme, bad port.
//   * Host classification: IPv4/IPv6 literals, loopback, private, reserved,
//     internal names, v4-mapped v6.
//   * UrlPolicy: https-only, IP literals, private ranges, the loopback-http
//     carve-out.
//   * Network scope matching: scoped grants cover their host and nothing else,
//     an unscoped `network` grant covers nothing, wildcards cover all, and the
//     legacy `domain=` spelling is equivalent.
//   * End to end: the http_get binding refuses a destination outside the grant
//     without opening a socket.
#include "berry/async.h"
#include "berry/manifest.h"
#include "berry/url.h"

#include "shim_net.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static CapabilitySet Grant(const std::vector<std::string>& caps) {
  CapabilitySet s;
  for (const auto& c : caps) s.Add(c);
  return s;
}

static void TestParseShape() {
  Url u;
  CHECK(u.Parse("https://api.example.com/v1/state?x=1"));
  CHECK(u.scheme == "https");
  CHECK(u.host == "api.example.com");
  CHECK(u.port.empty());
  CHECK(u.path == "/v1/state?x=1");
  CHECK(u.EffectivePort() == 443);

  // Scheme and host are lower-cased; a bare host still gets "/".
  CHECK(u.Parse("HTTPS://API.Example.COM"));
  CHECK(u.scheme == "https");
  CHECK(u.host == "api.example.com");
  CHECK(u.path == "/");
  CHECK(u.EffectivePort() == 443);

  // Explicit port and http default.
  CHECK(u.Parse("http://example.com:8080/x"));
  CHECK(u.host == "example.com");
  CHECK(u.port == "8080");
  CHECK(u.EffectivePort() == 8080);
  CHECK(u.Parse("http://example.com/x"));
  CHECK(u.EffectivePort() == 80);

  // IPv6 literal: brackets are parsed and stripped from the host, and the
  // port after the bracket is picked up.
  CHECK(u.Parse("https://[2001:db8::1]:8443/x"));
  CHECK(u.host == "2001:db8::1");
  CHECK(u.port == "8443");
  CHECK(u.EffectivePort() == 8443);

  // Query/fragment with no path normalizes to "/...".
  CHECK(u.Parse("https://example.com?a=1"));
  CHECK(u.path == "/?a=1");
}

static void TestParseRejects() {
  Url u;
  std::string err;
  CHECK(!u.Parse("", &err));
  CHECK(!u.Parse("example.com/x", &err));          // no scheme
  CHECK(!u.Parse("ftp://example.com", &err));      // unsupported scheme
  CHECK(!u.Parse("https://", &err));               // no host
  CHECK(!u.Parse("https://user:pass@example.com", &err));  // userinfo
  CHECK(!u.Parse("https://example.com:99999/x", &err));    // port range
  CHECK(!u.Parse("https://example.com:abc/x", &err));      // non-numeric port
  CHECK(!u.Parse("https://exa mple.com/x", &err));         // space in host
  CHECK(!u.Parse("https://example.com\r\nHost: evil", &err));  // header split
  // A control byte anywhere is rejected, not just in the host.
  CHECK(!u.Parse("https://example.com/\x01", &err));
}

static void TestHostClassification() {
  // IPv4 literals.
  CHECK(IsIpLiteral("1.2.3.4"));
  CHECK(IsIpLiteral("255.255.255.255"));
  CHECK(!IsIpLiteral("example.com"));
  CHECK(!IsIpLiteral("1.2.3"));           // not a quad
  CHECK(!IsIpLiteral("1.2.3.4.5"));
  CHECK(!IsIpLiteral("010.0.0.1"));       // ambiguous octal: rejected
  CHECK(!IsIpLiteral("0x7f.0.0.1"));
  CHECK(!IsIpLiteral("256.0.0.1"));

  // IPv6 literals.
  CHECK(IsIpLiteral("::1"));
  CHECK(IsIpLiteral("2001:db8::1"));
  CHECK(IsIpLiteral("fe80::1%eth0"));
  CHECK(!IsIpLiteral(":::"));             // malformed
  CHECK(!IsIpLiteral("1:2:3:4:5:6:7:8:9"));  // too many groups

  // Loopback.
  CHECK(IsLoopbackHost("127.0.0.1"));
  CHECK(IsLoopbackHost("127.9.9.9"));
  CHECK(IsLoopbackHost("localhost"));
  CHECK(IsLoopbackHost("::1"));
  CHECK(!IsLoopbackHost("10.0.0.1"));

  // Private / reserved.
  CHECK(IsPrivateOrReservedHost("10.0.0.1"));
  CHECK(IsPrivateOrReservedHost("172.16.0.1"));
  CHECK(IsPrivateOrReservedHost("172.31.255.255"));
  CHECK(IsPrivateOrReservedHost("192.168.1.1"));
  CHECK(IsPrivateOrReservedHost("169.254.169.254"));   // cloud metadata
  CHECK(IsPrivateOrReservedHost("100.64.0.1"));        // CGNAT
  CHECK(IsPrivateOrReservedHost("0.0.0.0"));
  CHECK(IsPrivateOrReservedHost("224.0.0.1"));         // multicast
  CHECK(IsPrivateOrReservedHost("fe80::1"));
  CHECK(IsPrivateOrReservedHost("fc00::1"));
  CHECK(IsPrivateOrReservedHost("::ffff:127.0.0.1"));  // v4-mapped loopback
  CHECK(IsPrivateOrReservedHost("::ffff:10.0.0.1"));
  CHECK(IsPrivateOrReservedHost("metadata.google.internal"));
  CHECK(IsPrivateOrReservedHost("localhost"));
  CHECK(IsPrivateOrReservedHost("router"));            // single label
  CHECK(IsPrivateOrReservedHost("box.local"));
  // Public addresses stay public.
  CHECK(!IsPrivateOrReservedHost("8.8.8.8"));
  CHECK(!IsPrivateOrReservedHost("api.example.com"));
  CHECK(!IsPrivateOrReservedHost("2001:4860:4860::8888"));
  // Anything unclassifiable is treated as private (fail closed).
  CHECK(IsPrivateOrReservedHost(""));
  CHECK(IsPrivateOrReservedHost("not a host"));
}

static void TestPolicyDefault() {
  UrlPolicy p;   // https only, no IP literals, no private ranges
  Url u;

  CHECK(u.Parse("https://api.example.com/x"));
  CHECK(p.Check(u) == UrlReject::None);

  // Plaintext is refused: the grant's traffic would be readable on the path.
  CHECK(u.Parse("http://api.example.com/x"));
  CHECK(p.Check(u) == UrlReject::InsecureScheme);

  // IP literals are refused even when public: a grant names a host.
  CHECK(u.Parse("https://8.8.8.8/x"));
  CHECK(p.Check(u) == UrlReject::IpLiteral);

  // Private hosts are refused.
  CHECK(u.Parse("https://127.0.0.1/x"));
  CHECK(p.Check(u) == UrlReject::IpLiteral);   // literal check fires first

  UrlPolicy noLiteral;
  noLiteral.allowIpLiterals = true;
  CHECK(noLiteral.Check(u) == UrlReject::PrivateAddress);

  CHECK(u.Parse("https://169.254.169.254/latest/meta-data/"));
  CHECK(noLiteral.Check(u) == UrlReject::PrivateAddress);

  // Internal names are refused as names, not as addresses.
  CHECK(u.Parse("https://localhost/x"));
  CHECK(p.Check(u) == UrlReject::InternalName);
  CHECK(u.Parse("https://intranet/x"));
  CHECK(p.Check(u) == UrlReject::InternalName);
}

static void TestPolicyCarveOuts() {
  // The host harness needs loopback over plaintext; it is opt-in.
  UrlPolicy loopbackHttp;
  loopbackHttp.allowPrivateRanges = true;
  loopbackHttp.allowLoopbackHttp = true;
  Url u;

  CHECK(u.Parse("http://127.0.0.1:8080/x"));
  CHECK(loopbackHttp.Check(u) == UrlReject::None);
  CHECK(u.Parse("http://localhost:8080/x"));
  CHECK(loopbackHttp.Check(u) == UrlReject::None);
  // The carve-out does not extend to other private hosts. The literal rule
  // fires first here, so a private host still needs allowIpLiterals to be
  // judged on the plaintext rule at all.
  CHECK(u.Parse("http://10.0.0.1/x"));
  CHECK(loopbackHttp.Check(u) == UrlReject::IpLiteral);
  loopbackHttp.allowIpLiterals = true;
  CHECK(loopbackHttp.Check(u) == UrlReject::InsecureScheme);
  // Nor does it disable the https requirement for public hosts.
  CHECK(u.Parse("http://api.example.com/x"));
  CHECK(loopbackHttp.Check(u) == UrlReject::InsecureScheme);

  // Disabling httpsOnly allows plaintext generally.
  UrlPolicy lax;
  lax.httpsOnly = false;
  CHECK(u.Parse("http://api.example.com/x"));
  CHECK(lax.Check(u) == UrlReject::None);
}

static void TestScopeNormalization() {
  // The modern and legacy spellings name the same host.
  CHECK(NormalizeNetworkScope("api.example.com") == "api.example.com");
  CHECK(NormalizeNetworkScope("domain=api.example.com") == "api.example.com");
  CHECK(NormalizeNetworkScope("host=api.example.com") == "api.example.com");
  CHECK(NormalizeNetworkScope("API.Example.COM") == "api.example.com");
  CHECK(NormalizeNetworkScope("api.example.com.") == "api.example.com");
  CHECK(NormalizeNetworkScope("[::1]") == "::1");
  // A scope is a host, never a URL or a wildcard.
  CHECK(NormalizeNetworkScope("").empty());
  CHECK(NormalizeNetworkScope("https://api.example.com").empty());
  CHECK(NormalizeNetworkScope("api.example.com/path").empty());
  CHECK(NormalizeNetworkScope("user@api.example.com").empty());
  CHECK(NormalizeNetworkScope("*").empty());
  CHECK(NormalizeNetworkScope("bogus=api.example.com").empty());
}

static void TestScopeCovers() {
  Capability grant, want;

  // A scoped grant covers its host, however it is spelled on either side.
  CHECK(Capability::Parse("network:api.example.com", &grant));
  CHECK(Capability::Parse("network:domain=api.example.com", &want));
  CHECK(grant.Covers(want));
  CHECK(Capability::Parse("network:domain=api.example.com", &grant));
  CHECK(Capability::Parse("network:api.example.com", &want));
  CHECK(grant.Covers(want));

  // A different host is not covered.
  CHECK(Capability::Parse("network:api.example.com", &grant));
  CHECK(Capability::Parse("network:evil.example.com", &want));
  CHECK(!grant.Covers(want));
  // Nor is a subdomain (a grant is exact, not a suffix rule).
  CHECK(Capability::Parse("network:example.com", &grant));
  CHECK(Capability::Parse("network:api.example.com", &want));
  CHECK(!grant.Covers(want));

  // An unscoped network grant covers nothing: this is the bug where a bare
  // `network` capability reached every host.
  CHECK(Capability::Parse("network", &grant));
  CHECK(Capability::Parse("network:api.example.com", &want));
  CHECK(!grant.Covers(want));
  CHECK(!grant.Covers(Capability{}));   // and not the empty request either

  // A wildcard covers any host.
  CHECK(Capability::Parse("network:*", &grant));
  CHECK(grant.Covers(want));
  CHECK(Capability::Parse("network:anything.example.org", &want));
  CHECK(grant.Covers(want));

  // Kind mismatch never covers.
  CHECK(Capability::Parse("render", &grant));
  CHECK(Capability::Parse("network:api.example.com", &want));
  CHECK(!grant.Covers(want));

  // Set-level: the effective grant decides, not the binding's placeholder.
  CapabilitySet granted = Grant({"network:api.example.com"});
  CHECK(granted.CoversCap(want));
  Capability okHost;
  CHECK(Capability::Parse("network:api.example.com", &okHost));
  CHECK(granted.CoversCap(okHost));
  CapabilitySet bare = Grant({"network"});
  CHECK(!bare.CoversCap(okHost));
}

static void TestHttpGetRefusesOutOfScope() {
  // The binding must refuse before it opens a socket, so an ungranted host
  // fails fast and deterministically. A public host is used so the refusal can
  // only come from the scope check.
  AsyncRuntime rt(nullptr, nullptr);
  ScriptBindings bindings;
  rt.InstallBindings(&bindings);

  ScriptContext ctx("scoped", Grant({"network:api.example.com"}));
  bool ok = true, denied = false;

  // In-scope host: the binding is entered (the request itself will fail to
  // connect in a sandbox, which is fine — we assert the refusal text below).
  bindings.Invoke(ctx, "async.http_get",
                  {ScriptValue::Str("https://api.example.com/x")}, &ok, &denied);
  CHECK(ok);

  // Out-of-scope host: refused, with the reason recorded on the context.
  ctx.ClearError();
  bindings.Invoke(ctx, "async.http_get",
                  {ScriptValue::Str("https://evil.example.com/x")}, &ok, &denied);
  CHECK(ok);
  CHECK(ctx.hasError());
  CHECK(ctx.errorMessage().find("not granted") != std::string::npos);

  // A bare `network` grant is not a licence to reach anything.
  ScriptContext bare("bare", Grant({"network"}));
  bindings.Invoke(bare, "async.http_get",
                  {ScriptValue::Str("https://api.example.com/x")}, &ok, &denied);
  CHECK(bare.hasError());
  CHECK(bare.errorMessage().find("not granted") != std::string::npos);

  // Loopback and metadata addresses are refused by the policy even when the
  // grant names them, so a grant cannot be tricked into SSRF.
  ScriptContext meta("meta", Grant({"network:169.254.169.254"}));
  bindings.Invoke(meta, "async.http_get",
                  {ScriptValue::Str("https://169.254.169.254/latest/meta-data/")},
                  &ok, &denied);
  CHECK(meta.hasError());
  CHECK(meta.errorMessage().find("refused") != std::string::npos);

  ScriptContext local("local", Grant({"network:localhost"}));
  bindings.Invoke(local, "async.http_get",
                  {ScriptValue::Str("https://localhost/x")}, &ok, &denied);
  CHECK(local.hasError());
  CHECK(local.errorMessage().find("refused") != std::string::npos);

  // A malformed URL is a refusal, not a crash.
  ScriptContext bad("bad", Grant({"network:*"}));
  bindings.Invoke(bad, "async.http_get", {ScriptValue::Str("not a url")},
                  &ok, &denied);
  CHECK(bad.hasError());
  CHECK(bad.errorMessage().find("bad url") != std::string::npos);

  // A NaN timeout must not reach the long conversion (UB) or the socket.
  ScriptContext nanCtx("nan", Grant({"network:*"}));
  bindings.Invoke(nanCtx, "async.http_get",
                  {ScriptValue::Str("https://api.example.com/x"),
                   ScriptValue::Num(std::nanf(""))},
                  &ok, &denied);
  CHECK(ok);   // handled, not crashed
}

//----------------------------------------------------------------------------
// End to end: the binding actually reaches a granted host, follows an in-scope
// redirect, and refuses an out-of-scope one. A loopback server stands in for
// the network; the policy is relaxed for loopback only, exactly as the harness
// would.
//----------------------------------------------------------------------------

class RedirectServer final : public TinyTcpAcceptor {
 public:
  // "/hop" redirects to `target`; anything else answers 200 with "ok".
  explicit RedirectServer(std::string target) : target_(std::move(target)) {}

 protected:
  void Handle(int conn) override {
    std::string req = RecvAll(conn, 2000);
    if (req.empty()) return;
    std::string resp;
    if (req.rfind("GET /hop", 0) == 0) {
      resp = "HTTP/1.1 302 Found\r\nLocation: " + target_ +
             "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    } else {
      std::string body = "ok";
      resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
             "Connection: close\r\n\r\n" + body;
    }
    ::send(conn, resp.data(), resp.size(), MSG_NOSIGNAL);
  }

 private:
  std::string target_;
};

static UrlPolicy LoopbackPolicy() {
  UrlPolicy p;
  p.allowPrivateRanges = true;
  p.allowIpLiterals = true;
  p.allowLoopbackHttp = true;
  return p;
}

static void TestHttpGetEndToEnd() {
  RedirectServer server("/ok");
  const int port = server.Start(0);
  CHECK(port > 0);
  if (port <= 0) return;

  const std::string origin = "http://127.0.0.1:" + std::to_string(port);
  AsyncRuntime rt(nullptr, nullptr);
  ScriptBindings bindings;
  rt.InstallBindings(&bindings, LoopbackPolicy());

  // In scope: the request is made and the body comes back.
  ScriptContext ctx("e2e", Grant({"network:127.0.0.1"}));
  bool ok = false, denied = false;
  ScriptValue v = bindings.Invoke(
      ctx, "async.http_get", {ScriptValue::Str(origin + "/")}, &ok, &denied);
  CHECK(ok && !denied);
  CHECK(!ctx.hasError());
  CHECK(v.IsString() && v.AsString() == "ok");

  // In scope, and a same-host redirect is followed to its destination.
  ctx.ClearError();
  v = bindings.Invoke(ctx, "async.http_get", {ScriptValue::Str(origin + "/hop")},
                      &ok, &denied);
  CHECK(ok);
  CHECK(!ctx.hasError());
  CHECK(v.IsString() && v.AsString() == "ok");
}

static void TestHttpGetRefusesRedirectOutOfScope() {
  // A granted host that redirects to a host the script was never granted must
  // not be followed. This is the redirect bypass: following blindly would let
  // one granted host reach anything.
  RedirectServer server("https://evil.example.com/x");
  const int port = server.Start(0);
  CHECK(port > 0);
  if (port <= 0) return;

  const std::string origin = "http://127.0.0.1:" + std::to_string(port);
  AsyncRuntime rt(nullptr, nullptr);
  ScriptBindings bindings;
  rt.InstallBindings(&bindings, LoopbackPolicy());

  ScriptContext ctx("redirect", Grant({"network:127.0.0.1"}));
  bool ok = false, denied = false;
  ScriptValue v = bindings.Invoke(ctx, "async.http_get",
                                  {ScriptValue::Str(origin + "/hop")}, &ok, &denied);
  CHECK(ok);
  CHECK(ctx.hasError());
  CHECK(ctx.errorMessage().find("not granted") != std::string::npos);
  CHECK(v.IsNil());
}

int main() {
  TestParseShape();
  TestParseRejects();
  TestHostClassification();
  TestPolicyDefault();
  TestPolicyCarveOuts();
  TestScopeNormalization();
  TestScopeCovers();
  TestHttpGetRefusesOutOfScope();
  TestHttpGetEndToEnd();
  TestHttpGetRefusesRedirectOutOfScope();
  if (g_fail == 0) printf("test_url: ALL OK\n");
  return g_fail;
}
