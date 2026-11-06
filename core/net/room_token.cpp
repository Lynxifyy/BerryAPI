// Room tokens for the voice relay. See include/berry/room_token.h for the flow,
// the two-stage verification and the fail-closed rules.
#include "berry/room_token.h"

#include "../script/json_min.h"
#include "../script/json_write.h"
#include "berry/logger.h"

#include <cstdio>

namespace Berry {

namespace {

/// A fixed, domain-separated serialisation of the fields a backend signs. Built
/// with explicit lengths so no field can be spliced into another (a room name
/// containing a newline cannot shift the layout).
void AppendField(std::string* out, const std::string& v) {
  out->append(std::to_string(v.size()));
  out->push_back(':');
  out->append(v);
  out->push_back('\n');
}

/// Reads a JSON string member; false when absent or not a string.
bool GetString(const detail::json::Value& obj, const char* key,
               std::string* out) {
  const detail::json::Value* v = obj.Get(key);
  if (v == nullptr || !v->IsString()) return false;
  *out = v->str;
  return true;
}

/// True when `host` is a bare IPv4 literal inside a loopback, link-local,
/// private or CGNAT range, or an IPv6 loopback/link-local/ULA literal. A relay
/// host like this is refused by default so a malicious token cannot point the
/// client at its own LAN (bug 56).
bool IsPrivateOrLocalHost(const std::string& host) {
  // IPv6 loopback / link-local / unique-local.
  if (host == "::1" || host == "::") return true;
  if (host.size() >= 2 &&
      (host.rfind("fe80:", 0) == 0 || host.rfind("FE80:", 0) == 0 ||
       host.rfind("fc", 0) == 0 || host.rfind("fd", 0) == 0 ||
       host.rfind("FC", 0) == 0 || host.rfind("FD", 0) == 0)) {
    return true;
  }
  // Dotted-quad IPv4.
  unsigned a = 0, b = 0, c = 0, d = 0;
  char extra = 0;
  if (sscanf(host.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) == 4) {
    if (a > 255 || b > 255 || c > 255 || d > 255) return true;   // malformed
    if (a == 127) return true;                    // loopback
    if (a == 10) return true;                     // private
    if (a == 192 && b == 168) return true;        // private
    if (a == 172 && b >= 16 && b <= 31) return true;  // private
    if (a == 169 && b == 254) return true;        // link-local
    if (a == 100 && b >= 64 && b <= 127) return true; // CGNAT
    if (a == 0) return true;                      // "this network"
  }
  return false;
}

}  // namespace

std::string RoomTokenSignedBytes(const RoomToken& token) {
  std::string out = "berry-room-token-v1\n";
  AppendField(&out, token.room);
  AppendField(&out, crypto::ToHexLower(token.secret));
  AppendField(&out, token.relayHost);
  AppendField(&out, std::to_string((unsigned)token.relayPort));
  AppendField(&out, std::to_string(token.expiresAtMs));
  return out;
}

bool RoomTokenClient::VerifyDocument(const std::string& body,
                                     const std::string& room, u64 nowMs,
                                     RoomToken* out, std::string* error) {
  auto fail = [&](const char* msg) {
    if (error) *error = msg;
    return false;
  };
  if (out == nullptr) return fail("no output");

  detail::json::Value root;
  if (!detail::json::Parse(body, &root) || !root.IsObject()) {
    return fail("malformed token document");
  }

  RoomToken tok;
  if (!GetString(root, "room", &tok.room)) return fail("token has no room");
  if (tok.room != room) return fail("token is for another room");

  std::string secretHex;
  if (!GetString(root, "secret", &secretHex)) return fail("token has no secret");
  // FromHex with no expected size accepts any length, so enforce the minimum the
  // relay requires here rather than at the transport.
  if (!crypto::FromHex(secretHex, &tok.secret) || tok.secret.size() < 16) {
    return fail("room secret too short");
  }

  if (!GetString(root, "relay_host", &tok.relayHost) || tok.relayHost.empty()) {
    return fail("token has no relay host");
  }
  // The relay host must be a plain hostname or address: no control character,
  // no scheme, no path. A CRLF here would be a header-injection vector.
  for (char ch : tok.relayHost) {
    if (ch == '\r' || ch == '\n' || ch == '\0' || ch == '/' || ch == ' ' ||
        ch == '@') {
      return fail("token relay host is malformed");
    }
  }
  // Refuse a relay in the client's own LAN unless the deployment opts in, so a
  // malicious (or compromised-backend) token cannot pivot into the local
  // network (bug 56).
  if (!cfg_.allowPrivateRelayHost && IsPrivateOrLocalHost(tok.relayHost)) {
    return fail("token relay host is in a local/private range");
  }
  const detail::json::Value* portV = root.Get("relay_port");
  if (portV == nullptr || !portV->IsNumber() || portV->number < 1 ||
      portV->number > 65535) {
    return fail("token has no valid relay port");
  }
  // A fractional or non-finite port is refused rather than truncated: 443.7 is
  // not 443 (bug 55).
  if (portV->number != (double)(unsigned long long)portV->number) {
    return fail("token relay port is not an integer");
  }
  tok.relayPort = (u16)portV->number;

  // expires_at is REQUIRED: a token with no expiry, or a non-positive one,
  // never expires, which turns a leaked token into a permanent key (bug 54).
  const detail::json::Value* expV = root.Get("expires_at");
  if (expV == nullptr || !expV->IsNumber() || expV->number <= 0) {
    return fail("token has no expiry");
  }
  if (expV->number != (double)(unsigned long long)expV->number) {
    return fail("token expiry is not an integer");
  }
  if (expV->number > 1.8e18) return fail("token expiry is out of range");
  tok.expiresAtMs = (u64)expV->number;
  if (tok.expired(nowMs)) return fail("token expired");

  // Signature: required and verified. An empty backend key fails closed unless
  // the deployment explicitly opted in to TLS-only authentication (bug 53).
  if (cfg_.backendPublicKeyHex.empty()) {
    if (!cfg_.allowUnsigned) {
      return fail("no backend key configured and unsigned tokens are not allowed");
    }
  } else {
    std::string sigHex;
    if (!GetString(root, "signature", &sigHex)) {
      return fail("token is not signed");
    }
    std::string sig;
    std::string pub;
    if (!crypto::FromHex(sigHex, &sig, 64)) return fail("bad signature hex");
    if (!crypto::Ed25519PublicKeyFromHex(cfg_.backendPublicKeyHex, &pub)) {
      return fail("bad backend public key");
    }
    if (!crypto::Ed25519Verify(RoomTokenSignedBytes(tok), sig, pub)) {
      return fail("token signature does not verify");
    }
  }

  if (!tok.valid()) return fail("token is incomplete");
  *out = std::move(tok);
  if (error) error->clear();
  return true;
}

bool RoomTokenClient::Fetch(const std::string& room,
                            const VoiceIdentity& identity, u64 nowMs,
                            RoomToken* out, std::string* error) {
  auto fail = [&](const char* msg) {
    if (error) *error = msg;
    return false;
  };
  if (cfg_.baseUrl.empty()) return fail("no backend base URL");
  if (!identity.valid()) return fail("no identity");
  if (room.empty()) return fail("no room");

  // https only: a plaintext token fetch would hand the room secret to anything
  // on the path. HttpRequest already refuses an unknown scheme and, with TLS
  // compiled in, performs a verified handshake.
  const std::string url = cfg_.baseUrl + EndpointPath();

  std::string body;
  {
    detail::jsonw::Writer w(&body);
    w.BeginObject();
    w.Member("room", room);
    w.Member("identity", identity.publicKeyHex());
    w.EndObject();
  }

  HttpRequest req(url.c_str());
  req.Method("POST").JsonBody(body.c_str()).Timeout(cfg_.timeoutMs);
  if (req.badScheme()) return fail("backend URL is not a usable https URL");
  if (!req.isTls()) return fail("backend URL must be https");
  HttpResponse resp = req.Do();
  if (!resp.ok()) {
    if (error) {
      char buf[64];
      std::snprintf(buf, sizeof buf, "token fetch failed: HTTP %d",
                    resp.status);
      *error = buf;
    }
    return false;
  }
  return VerifyDocument(resp.body, room, nowMs, out, error);
}

}  // namespace Berry
