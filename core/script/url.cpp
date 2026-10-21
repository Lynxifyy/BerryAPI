// URL parsing and the script egress policy. See include/berry/url.h.
//
// This is the SSRF gate for `http_get`. It is deliberately a pure layer: no
// sockets, no DNS, no clock, so every rule is unit-testable and every decision
// is reproducible. Anything it cannot classify as public is treated as
// private, because the cost of a false denial is a broken script while the
// cost of a false allow is a script reading cloud metadata.
#include "berry/url.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace Berry {

namespace {

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

bool AllDigits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

/// Parses a decimal IPv4 dotted quad into 4 bytes. Returns false for anything
/// that is not exactly four 0-255 octets (so "1.2.3", "1.2.3.4.5", "0x7f.1"
/// and leading-zero octal forms are all rejected rather than guessed at).
bool ParseIpv4(const std::string& host, unsigned char out[4]) {
  int part = 0;
  size_t i = 0;
  while (part < 4) {
    size_t start = i;
    while (i < host.size() && host[i] != '.') ++i;
    std::string octet = host.substr(start, i - start);
    if (!AllDigits(octet) || octet.size() > 3) return false;
    // Reject leading zeros: "010" is octal to some resolvers and decimal to
    // others, which is exactly the ambiguity an SSRF bypass needs.
    if (octet.size() > 1 && octet[0] == '0') return false;
    long v = std::strtol(octet.c_str(), nullptr, 10);
    if (v < 0 || v > 255) return false;
    out[part++] = (unsigned char)v;
    if (part < 4) {
      if (i >= host.size() || host[i] != '.') return false;
      ++i;
    }
  }
  return i == host.size();
}

/// Splits an IPv6 literal into its 8 groups, expanding "::" and accepting a
/// trailing embedded IPv4. Returns false when the text is not valid IPv6.
bool ParseIpv6(const std::string& host, unsigned short out[8]) {
  for (int i = 0; i < 8; ++i) out[i] = 0;
  std::string h = host;
  if (!h.empty() && h.front() == '[' && h.back() == ']') {
    h = h.substr(1, h.size() - 2);
  }
  if (h.empty()) return false;
  // Strip a zone id ("fe80::1%eth0"); the zone does not change the range.
  size_t pct = h.find('%');
  if (pct != std::string::npos) h = h.substr(0, pct);
  if (h.find(':') == std::string::npos) return false;

  size_t dcolon = h.find("::");
  if (dcolon != std::string::npos && h.find("::", dcolon + 1) != std::string::npos) {
    return false;   // at most one "::"
  }

  std::vector<std::string> head, tail;
  std::vector<std::string>* cur = &head;
  size_t i = 0;
  while (i <= h.size()) {
    size_t colon = h.find(':', i);
    if (colon == std::string::npos) colon = h.size();
    std::string group = h.substr(i, colon - i);
    if (dcolon != std::string::npos && i == dcolon + 2) cur = &tail;
    if (!group.empty()) cur->push_back(group);
    if (colon == h.size()) break;
    i = colon + 1;
  }

  auto fill = [](const std::vector<std::string>& groups, size_t offset,
                 unsigned short* out) -> bool {
    for (size_t g = 0; g < groups.size(); ++g) {
      const std::string& s = groups[g];
      if (s.find('.') != std::string::npos) {
        unsigned char v4[4];
        if (!ParseIpv4(s, v4)) return false;
        out[offset + g] = (unsigned short)((v4[0] << 8) | v4[1]);
        out[offset + g + 1] = (unsigned short)((v4[2] << 8) | v4[3]);
        continue;
      }
      if (s.size() > 4) return false;
      for (char c : s) {
        if (!std::isxdigit((unsigned char)c)) return false;
      }
      out[offset + g] = (unsigned short)std::strtoul(s.c_str(), nullptr, 16);
    }
    return true;
  };

  size_t total = 0;
  for (const auto& s : head) total += (s.find('.') != std::string::npos) ? 2 : 1;
  for (const auto& s : tail) total += (s.find('.') != std::string::npos) ? 2 : 1;

  if (dcolon == std::string::npos) {
    if (total != 8) return false;
    return fill(head, 0, out);
  }
  if (total >= 8) return false;
  if (!fill(head, 0, out)) return false;
  if (!fill(tail, 8 - total, out)) return false;
  return true;
}

/// True for a hostname that can only refer to the local machine or an internal
/// namespace. Kept conservative: these are names an SSRF attacker relies on.
bool IsInternalName(const std::string& h) {
  static const char* kNames[] = {
      "localhost", "localhost.localdomain", "local", "internal",
      "metadata.google.internal",
  };
  for (const char* n : kNames) {
    if (h == n) return true;
  }
  // Trailing dot form ("localhost.") means the same host.
  if (!h.empty() && h.back() == '.') {
    return IsInternalName(h.substr(0, h.size() - 1));
  }
  // Bare single-label names ("intranet", "router") resolve through the local
  // search domain, so they are internal by definition. Public hosts always
  // carry at least one dot.
  if (h.find('.') == std::string::npos) return true;
  // Reserved for local/interop names.
  if (h.size() >= 6 && h.compare(h.size() - 6, 6, ".local") == 0) return true;
  if (h.size() >= 9 && h.compare(h.size() - 9, 9, ".internal") == 0) return true;
  if (h.size() >= 11 && h.compare(h.size() - 11, 11, ".localdomain") == 0) return true;
  return false;
}

/// True when an IPv4 address is in a loopback, private, link-local, CGNAT,
/// documentation, benchmark or otherwise reserved range.
bool IsReservedIpv4(unsigned char a, unsigned char b, unsigned char c, unsigned char d) {
  (void)c; (void)d;
  if (a == 0) return true;                       // 0.0.0.0/8 "this network"
  if (a == 10) return true;                      // 10/8 private
  if (a == 127) return true;                     // 127/8 loopback
  if (a == 169 && b == 254) return true;         // 169.254/16 link-local
  if (a == 172 && b >= 16 && b <= 31) return true;  // 172.16/12 private
  if (a == 192 && b == 168) return true;         // 192.168/16 private
  if (a == 192 && b == 0 && c == 0) return true; // 192.0.0/24 IETF protocol
  if (a == 192 && b == 0 && c == 2) return true; // 192.0.2/24 TEST-NET-1
  if (a == 192 && b == 88 && c == 99) return true;  // 6to4 relay anycast
  if (a == 198 && (b == 18 || b == 19)) return true;  // 198.18/15 benchmark
  if (a == 198 && b == 51 && c == 100) return true;   // TEST-NET-2
  if (a == 203 && b == 0 && c == 113) return true;    // TEST-NET-3
  if (a >= 224) return true;                     // multicast + reserved + broadcast
  if (a == 100 && b >= 64 && b <= 127) return true;   // 100.64/10 CGNAT
  return false;
}

}  // namespace

//----------------------------------------------------------------------------
// Url
//----------------------------------------------------------------------------

bool Url::Parse(const std::string& text, std::string* error) {
  scheme.clear();
  host.clear();
  port.clear();
  path.clear();
  auto fail = [error](const char* msg) {
    if (error) *error = msg;
    return false;
  };

  if (text.empty()) return fail("empty url");
  // Reject control characters outright: they are how a request gets split or
  // how a "host" smuggles a second line into the header block.
  for (unsigned char c : text) {
    if (c < 0x20 || c == 0x7f) return fail("control character in url");
  }

  size_t sep = text.find("://");
  if (sep == std::string::npos) return fail("missing scheme separator");
  scheme = Lower(text.substr(0, sep));
  if (scheme.empty()) return fail("missing scheme");
  if (scheme != "http" && scheme != "https") return fail("unsupported scheme");

  std::string rest = text.substr(sep + 3);
  size_t slash = rest.find_first_of("/?#");
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  if (slash == std::string::npos) {
    path = "/";
  } else if (rest[slash] == '/') {
    path = rest.substr(slash);
  } else {
    // "?query" or "#fragment" with no path: normalize to "/query".
    path = "/" + rest.substr(slash);
  }
  if (authority.empty()) return fail("missing host");

  // Userinfo is a credential-smuggling vector and a grant names a host, not a
  // login, so it is rejected rather than parsed.
  if (authority.find('@') != std::string::npos) return fail("userinfo not allowed");

  std::string hostPart = authority;
  if (!authority.empty() && authority.front() == '[') {
    size_t close = authority.find(']');
    if (close == std::string::npos) return fail("unterminated ipv6 literal");
    hostPart = authority.substr(0, close + 1);
    std::string after = authority.substr(close + 1);
    if (!after.empty()) {
      if (after[0] != ':') return fail("bad authority");
      port = after.substr(1);
    }
  } else {
    size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
      hostPart = authority.substr(0, colon);
      port = authority.substr(colon + 1);
    }
  }

  host = Lower(hostPart);
  if (host.empty()) return fail("missing host");
  // Strip the brackets for the range checks but keep them out of `host` so a
  // scope comparison never sees two spellings of the same address.
  if (host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
  if (host.empty()) return fail("missing host");

  for (char c : host) {
    if (!(std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_' ||
          c == ':')) {
      return fail("bad character in host");
    }
  }

  if (!port.empty()) {
    if (!AllDigits(port) || port.size() > 5) return fail("bad port");
    long p = std::strtol(port.c_str(), nullptr, 10);
    if (p < 1 || p > 65535) return fail("port out of range");
  }
  return true;
}

u16 Url::EffectivePort() const {
  if (!port.empty()) {
    long p = std::strtol(port.c_str(), nullptr, 10);
    if (p < 1 || p > 65535) return 0;
    return (u16)p;
  }
  if (scheme == "https") return 443;
  if (scheme == "http") return 80;
  return 0;
}

//----------------------------------------------------------------------------
// Host classification
//----------------------------------------------------------------------------

bool IsIpLiteral(const std::string& host) {
  if (host.find(':') != std::string::npos) {
    unsigned short v6[8];
    return ParseIpv6(host, v6);
  }
  unsigned char v4[4];
  return ParseIpv4(host, v4);
}

bool IsLoopbackHost(const std::string& host) {
  std::string h = Lower(host);
  if (h == "localhost") return true;
  if (h.find(':') != std::string::npos) {
    unsigned short v6[8];
    if (!ParseIpv6(h, v6)) return false;
    // ::1 and the v4-mapped ::ffff:127.0.0.1 forms.
    for (int i = 0; i < 7; ++i) {
      if (v6[i] != 0) {
        if (i == 5 && v6[5] == 0xffff) {
          return (v6[6] >> 8) == 127;
        }
        return false;
      }
    }
    return v6[7] == 1;
  }
  unsigned char v4[4];
  if (!ParseIpv4(h, v4)) return false;
  return v4[0] == 127;
}

bool IsPrivateOrReservedHost(const std::string& host) {
  std::string h = Lower(host);
  if (h.empty()) return true;
  if (h.front() == '[' && h.back() == ']') h = h.substr(1, h.size() - 2);

  if (h.find(':') != std::string::npos) {
    unsigned short v6[8];
    if (!ParseIpv6(h, v6)) return true;   // unparseable "ipv6" is not public
    // v4-mapped (::ffff:a.b.c.d) and v4-compatible: classify the inner v4.
    bool v4Mapped = true;
    for (int i = 0; i < 5; ++i) {
      if (v6[i] != 0) { v4Mapped = false; break; }
    }
    if (v4Mapped && (v6[5] == 0xffff || v6[5] == 0)) {
      return IsReservedIpv4((unsigned char)(v6[6] >> 8), (unsigned char)(v6[6] & 0xff),
                            (unsigned char)(v6[7] >> 8), (unsigned char)(v6[7] & 0xff));
    }
    if ((v6[0] & 0xfe00) == 0xfc00) return true;   // fc00::/7 unique local
    if ((v6[0] & 0xffc0) == 0xfe80) return true;   // fe80::/10 link local
    if ((v6[0] & 0xff00) == 0xff00) return true;   // ff00::/8 multicast
    if (v6[0] == 0x2001 && v6[1] == 0x0db8) return true;  // 2001:db8::/32 docs
    if (v6[0] == 0x0064 && v6[1] == 0xff9b) return true;  // 64:ff9b::/96 NAT64
    if (v6[0] == 0x0100) return true;              // 100::/64 discard-only
    return false;
  }

  unsigned char v4[4];
  if (ParseIpv4(h, v4)) {
    return IsReservedIpv4(v4[0], v4[1], v4[2], v4[3]);
  }
  return IsInternalName(h);
}

//----------------------------------------------------------------------------
// Policy
//----------------------------------------------------------------------------

const char* UrlRejectName(UrlReject r) {
  switch (r) {
    case UrlReject::None: return "ok";
    case UrlReject::Empty: return "empty";
    case UrlReject::Malformed: return "malformed";
    case UrlReject::MissingScheme: return "missing-scheme";
    case UrlReject::UnsupportedScheme: return "unsupported-scheme";
    case UrlReject::InsecureScheme: return "insecure-scheme";
    case UrlReject::MissingHost: return "missing-host";
    case UrlReject::UserInfo: return "userinfo";
    case UrlReject::IpLiteral: return "ip-literal";
    case UrlReject::PrivateAddress: return "private-address";
    case UrlReject::InternalName: return "internal-name";
    case UrlReject::BadHostChars: return "bad-host";
    case UrlReject::BadPort: return "bad-port";
  }
  return "?";
}

UrlReject UrlPolicy::Check(const Url& url) const {
  if (url.host.empty()) return UrlReject::MissingHost;
  if (url.scheme.empty()) return UrlReject::MissingScheme;
  if (url.scheme != "http" && url.scheme != "https") return UrlReject::UnsupportedScheme;

  // Loopback over plaintext is the one carve-out the host test harness needs.
  // It is opt-in, never enabled on device, and it lifts both the literal and
  // the range rule so a loopback harness URL can actually be reached.
  const bool loopbackHttp =
      allowLoopbackHttp && url.scheme == "http" && IsLoopbackHost(url.host);

  if (!loopbackHttp) {
    if (IsIpLiteral(url.host) && !allowIpLiterals) return UrlReject::IpLiteral;
    if (IsPrivateOrReservedHost(url.host) && !allowPrivateRanges) {
      return IsIpLiteral(url.host) ? UrlReject::PrivateAddress
                                   : UrlReject::InternalName;
    }
    if (httpsOnly && url.scheme != "https") return UrlReject::InsecureScheme;
  }

  if (url.EffectivePort() == 0) return UrlReject::BadPort;
  return UrlReject::None;
}

}  // namespace Berry
