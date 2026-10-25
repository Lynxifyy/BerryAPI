// Host tests for the voice-relay room token (F2).
//
// The token is what a presence backend delivers over HTTPS: the relay address
// and the room secret. The tests cover:
//   * the canonical signed bytes (deterministic and field-delimited);
//   * `VerifyDocument` — a well-formed, correctly-signed token is accepted, and a
//     tampered field, a wrong room, a bad signature, a missing signature, an
//     expired token and a short secret are each refused;
//   * `Fetch` over a *real* verified TLS handshake to the throwaway CA, proving
//     the token path uses the verified HTTPS stack (not a plaintext shortcut);
//   * a plaintext http base URL is refused outright.
#include "berry/room_token.h"

#include "shim_tls.h"
#include "berry/manifest.h"
#include "berry/tls.h"

#include <cstdio>
#include <string>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

const std::string kSecretHex = "00112233445566778899aabbccddeeff";
const std::string kRelayHost = "203.0.113.9";

/// Builds the backend's token JSON, optionally signed with `signSecret64`.
std::string TokenJson(const std::string& room, const std::string& secretHex,
                      const std::string& relayHost, u16 relayPort,
                      u64 expiresAtMs, const std::string& signatureHex) {
  std::string body;
  body += "{\"room\":\"" + room + "\"";
  body += ",\"secret\":\"" + secretHex + "\"";
  body += ",\"relay_host\":\"" + relayHost + "\"";
  body += ",\"relay_port\":" + std::to_string((unsigned)relayPort);
  if (expiresAtMs != 0) {
    body += ",\"expires_at\":" + std::to_string(expiresAtMs);
  }
  if (!signatureHex.empty()) {
    body += ",\"signature\":\"" + signatureHex + "\"";
  }
  body += "}";
  return body;
}

/// The canonical bytes the backend would sign for the token above.
std::string Canonical(const std::string& room, const std::string& secretHex,
                      const std::string& relayHost, u16 relayPort,
                      u64 expiresAtMs) {
  RoomToken t;
  t.room = room;
  crypto::FromHex(secretHex, &t.secret);
  t.relayHost = relayHost;
  t.relayPort = relayPort;
  t.expiresAtMs = expiresAtMs;
  return RoomTokenSignedBytes(t);
}

}  // namespace

static void TestCanonicalBytes() {
  const std::string a =
      Canonical("room-1", kSecretHex, kRelayHost, 47825, 1893456000000ULL);
  const std::string b =
      Canonical("room-1", kSecretHex, kRelayHost, 47825, 1893456000000ULL);
  CHECK(a == b);  // deterministic
  // A field change changes the bytes (so a swapped relay fails the signature).
  CHECK(a != Canonical("room-2", kSecretHex, kRelayHost, 47825, 1893456000000ULL));
  CHECK(a != Canonical("room-1", kSecretHex, "203.0.113.10", 47825,
                       1893456000000ULL));
  CHECK(a != Canonical("room-1", kSecretHex, kRelayHost, 47826,
                       1893456000000ULL));
  // The length prefix means a delimiter inside a field cannot splice two fields.
  CHECK(Canonical("a:b", "c", kRelayHost, 1, 1) !=
        Canonical("a", "b:c", kRelayHost, 1, 1));
}

static void TestVerifyDocument() {
  // The backend identity.
  std::string seed(32, 'S');
  std::string pub, sec;
  CHECK(crypto::Ed25519KeypairFromSeed(seed, &pub, &sec));
  const std::string pubHex = crypto::ToHexLower(pub);

  const u64 now = 1000000;
  const u64 expiry = now + 3600000;
  const std::string canon =
      Canonical("room-1", kSecretHex, kRelayHost, 47825, expiry);
  std::string sig;
  CHECK(crypto::Ed25519Sign(canon, sec, &sig));
  const std::string sigHex = crypto::ToHexLower(sig);

  RoomTokenClientConfig cfg;
  cfg.baseUrl = "https://presence.example.com";
  cfg.backendPublicKeyHex = pubHex;
  RoomTokenClient client(cfg);

  RoomToken tok;
  std::string err;
  // A correctly-signed token is accepted.
  CHECK(client.VerifyDocument(
      TokenJson("room-1", kSecretHex, kRelayHost, 47825, expiry, sigHex),
      "room-1", now, &tok, &err));
  CHECK(tok.valid());
  CHECK(tok.room == "room-1");
  CHECK(tok.relayHost == kRelayHost);
  CHECK(tok.relayPort == 47825);
  CHECK(tok.secret.size() == 16);

  // Wrong room (client asked for another) is refused.
  CHECK(!client.VerifyDocument(
      TokenJson("room-1", kSecretHex, kRelayHost, 47825, expiry, sigHex),
      "room-2", now, &tok, &err));

  // A tampered relay host fails the signature (a swapped relay is refused).
  CHECK(!client.VerifyDocument(
      TokenJson("room-1", kSecretHex, "203.0.113.10", 47825, expiry, sigHex),
      "room-1", now, &tok, &err));

  // A tampered secret fails the signature.
  CHECK(!client.VerifyDocument(
      TokenJson("room-1", "ff112233445566778899aabbccddeeff", kRelayHost, 47825,
                expiry, sigHex),
      "room-1", now, &tok, &err));

  // A shortened expiry fails the signature (anti-rollback of the window).
  CHECK(!client.VerifyDocument(
      TokenJson("room-1", kSecretHex, kRelayHost, 47825, expiry - 1, sigHex),
      "room-1", now, &tok, &err));

  // A missing signature is refused when a backend key is configured.
  CHECK(!client.VerifyDocument(
      TokenJson("room-1", kSecretHex, kRelayHost, 47825, expiry, ""),
      "room-1", now, &tok, &err));

  // A signature over the wrong bytes is refused.
  std::string badSig;
  CHECK(crypto::Ed25519Sign("not the token", sec, &badSig));
  CHECK(!client.VerifyDocument(
      TokenJson("room-1", kSecretHex, kRelayHost, 47825, expiry,
                crypto::ToHexLower(badSig)),
      "room-1", now, &tok, &err));

  // An expired token is refused.
  CHECK(!client.VerifyDocument(
      TokenJson("room-1", kSecretHex, kRelayHost, 47825, now - 1, sigHex),
      "room-1", now, &tok, &err));

  // A short secret is refused even when signed.
  const std::string shortCanon = Canonical("room-1", "aabb", kRelayHost, 47825, expiry);
  std::string shortSig;
  CHECK(crypto::Ed25519Sign(shortCanon, sec, &shortSig));
  CHECK(!client.VerifyDocument(
      TokenJson("room-1", "aabb", kRelayHost, 47825, expiry,
                crypto::ToHexLower(shortSig)),
      "room-1", now, &tok, &err));

  // A malformed document is refused.
  CHECK(!client.VerifyDocument("{not json", "room-1", now, &tok, &err));

  // With no backend key configured, TLS alone authenticates: an unsigned token
  // is accepted.
  RoomTokenClientConfig tlsOnly;
  tlsOnly.baseUrl = "https://presence.example.com";
  RoomTokenClient client2(tlsOnly);
  CHECK(client2.VerifyDocument(
      TokenJson("room-1", kSecretHex, kRelayHost, 47825, expiry, ""),
      "room-1", now, &tok, &err));
}

static void TestFetchOverTls() {
  // Point the trust store at the throwaway CA the shim server serves, exactly as
  // the TLS test does, so the fetch completes a real verified handshake.
  const std::string caPath = "/tmp/berry_roomtoken_ca.pem";
  {
    FILE* f = fopen(caPath.c_str(), "wb");
    CHECK(f != nullptr);
    if (f) { fputs(berry_test::kTestCaPem(), f); fclose(f); }
  }
  TlsSocket::SetCaBundlePath(caPath);
  TlsSocket::ReloadTrustStore();
  CHECK(TlsSocket::TrustStoreReady());

  std::string seed(32, 'S');
  std::string pub, sec;
  CHECK(crypto::Ed25519KeypairFromSeed(seed, &pub, &sec));
  const u64 now = 1000000;
  const u64 expiry = now + 3600000;
  const std::string canon =
      Canonical("room-1", kSecretHex, kRelayHost, 47825, expiry);
  std::string sig;
  CHECK(crypto::Ed25519Sign(canon, sec, &sig));
  const std::string body =
      TokenJson("room-1", kSecretHex, kRelayHost, 47825, expiry,
                crypto::ToHexLower(sig));

  berry_test::TinyTlsServer srv;
  const int port = srv.Start(0, body);
  CHECK(port > 0);
  if (port > 0) {
    RoomTokenClientConfig cfg;
    cfg.baseUrl = "https://localhost:" + std::to_string(port);
    cfg.backendPublicKeyHex = crypto::ToHexLower(pub);
    RoomTokenClient client(cfg);

    VoiceIdentity id = VoiceIdentity::FromSeed(std::string(32, 'I'));
    RoomToken tok;
    std::string err;
    CHECK(client.Fetch("room-1", id, now, &tok, &err));
    if (!tok.valid()) printf("fetch error: %s\n", err.c_str());
    CHECK(tok.valid());
    CHECK(tok.relayHost == kRelayHost);
    srv.Stop();
  }

  // A plaintext http base URL is refused: the token must not travel in the clear.
  {
    RoomTokenClientConfig cfg;
    cfg.baseUrl = "http://localhost:1";
    RoomTokenClient client(cfg);
    VoiceIdentity id = VoiceIdentity::FromSeed(std::string(32, 'I'));
    RoomToken tok;
    std::string err;
    CHECK(!client.Fetch("room-1", id, now, &tok, &err));
  }

  TlsSocket::SetCaBundlePath("/etc/ssl/certs/ca-certificates.crt");
  TlsSocket::ReloadTrustStore();
  remove(caPath.c_str());
}

int main() {
  TestCanonicalBytes();
  TestVerifyDocument();
  TestFetchOverTls();

  if (g_fail == 0) {
    printf("test_room_token: ALL OK\n");
    return 0;
  }
  printf("test_room_token: %d FAILURES\n", g_fail);
  return 1;
}
