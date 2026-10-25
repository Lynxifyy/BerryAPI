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
  const detail::json::Value* portV = root.Get("relay_port");
  if (portV == nullptr || !portV->IsNumber() || portV->number < 1 ||
      portV->number > 65535) {
    return fail("token has no valid relay port");
  }
  tok.relayPort = (u16)portV->number;

  if (const detail::json::Value* expV = root.Get("expires_at")) {
    if (expV->IsNumber() && expV->number > 0) {
      tok.expiresAtMs = (u64)expV->number;
    }
  }
  if (tok.expired(nowMs)) return fail("token expired");

  // Signature: required and verified when a backend key is configured.
  if (!cfg_.backendPublicKeyHex.empty()) {
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
