#pragma once
#include "berry/export.h"
#include "berry/types.h"

#include <string>

namespace Berry {

//----------------------------------------------------------------------------
// URL parsing + egress policy.
//
// Scripts reach the network through `http_get`, which is a classic SSRF sink: a
// script holding any network grant could otherwise point it at loopback, the
// LAN or a cloud-metadata endpoint. The scope check alone is not enough,
// because a hostname can resolve to a private address and a redirect can move
// the request to a host the grant never named.
//
// So the gate is two-stage and both stages are pure functions here (testable
// without a socket):
//   1. `UrlPolicy::Check` — is this destination one a script is allowed to
//      reach at all? HTTPS only, no IP literals, no private/reserved ranges,
//      no userinfo, no internal names.
//   2. `NormalizeNetworkScope` (manifest.h) — does the grant cover the host?
//
// The caller re-runs both stages on every redirect hop.
//----------------------------------------------------------------------------

/// A parsed absolute URL. Only the fields the policy needs are kept.
struct BERRY_API Url {
  std::string scheme;   // lower-cased: "http" or "https"
  std::string host;     // lower-cased, no userinfo, no port, brackets stripped
  std::string port;     // as written; empty when the scheme default applies
  std::string path;     // begins with '/' (defaults to "/"), includes query

  /// Parses an absolute URL. Returns false and sets `error` for anything that
  /// is not `scheme://host[/path]` with a usable host. Never throws.
  bool Parse(const std::string& text, std::string* error = nullptr);

  /// Effective port (defaults by scheme). Returns 0 when the port is invalid.
  u16 EffectivePort() const;
};

/// Why a URL was rejected. Every value except `None` is a denial.
enum class UrlReject : u32 {
  None = 0,
  Empty,
  Malformed,
  MissingScheme,
  UnsupportedScheme,
  InsecureScheme,
  MissingHost,
  UserInfo,
  IpLiteral,
  PrivateAddress,
  InternalName,
  BadHostChars,
  BadPort,
};

const char* UrlRejectName(UrlReject r);

/// Destination rules applied to every request (and every redirect hop).
struct BERRY_API UrlPolicy {
  /// Require https. A plaintext request would leak the grant's traffic to
  /// anything on the path, so it is off by default.
  bool httpsOnly = true;
  /// Permit dotted-quad / bracketed IPv6 hosts. Off: a grant names a hostname,
  /// and an address literal lets a script skip that name entirely.
  bool allowIpLiterals = false;
  /// Permit loopback, link-local, unique-local and the other reserved ranges.
  /// Off: these are the SSRF targets.
  bool allowPrivateRanges = false;
  /// Host-test escape hatch: allow plaintext http to a loopback address so the
  /// loopback harness can exercise the full path. Never enable on device.
  bool allowLoopbackHttp = false;

  /// Checks a parsed URL against the policy.
  UrlReject Check(const Url& url) const;
};

/// True when `host` is an IPv4 dotted quad or a bracketed/bare IPv6 literal.
BERRY_API bool IsIpLiteral(const std::string& host);

/// True when `host` is an address in a loopback, link-local, unique-local,
/// carrier-grade-NAT, documentation or otherwise reserved range, or an
/// internal-only name such as `localhost`. Conservative: anything it cannot
/// classify as public counts as private.
BERRY_API bool IsPrivateOrReservedHost(const std::string& host);

/// True when `host` is a loopback address (`127.0.0.0/8`, `::1`).
BERRY_API bool IsLoopbackHost(const std::string& host);

}  // namespace Berry
