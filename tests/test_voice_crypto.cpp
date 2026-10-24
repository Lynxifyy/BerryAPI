// Host tests for the voice authentication and encryption layer
// (include/berry/voice_crypto.h).
//
// Covers, end to end and against fixed vectors where one exists:
//   * X25519 — the RFC 7748 section 6.1 vector, plus the all-zero (low-order
//     peer) output being refused;
//   * HKDF-SHA256 — the RFC 5869 test case 1 vector;
//   * identity — key derivation from a seed, sign/verify, a flipped byte and a
//     wrong key both refused;
//   * trust anchors — an empty anchor accepts nothing, an unknown key is
//     refused, a room key too short is refused;
//   * announce framing — a round trip, and a decode that refuses a bad magic,
//     version, length, field size or trailing slack;
//   * the secure channel — two configured peers derive the *same* session key
//     and opposite nonce directions, an untrusted/unsigned/tampered announce is
//     refused, and a reflection of our own announce is refused;
//   * the session AEAD — seal/open round trip, a flipped ciphertext byte and a
//     wrong AAD refused, a non-monotonic send sequence refused, and a replayed
//     receive sequence refused;
//   * the replay window — a burst with reordering in-window is accepted and an
//     old/duplicate frame is refused.
#include "berry/voice_crypto.h"
#include "berry/manifest.h"   // crypto:: helpers, to build vectors

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

std::string HexToRaw(const std::string& hex) {
  std::string out;
  if (!crypto::FromHex(hex, &out, 0)) return {};
  return out;
}

// A deterministic 32-byte seed: byte i = base + i * 7.
std::string Seed(u8 base) {
  std::string s(32, '\0');
  for (size_t i = 0; i < s.size(); ++i) s[i] = (char)(u8)(base + i * 7);
  return s;
}

}  // namespace

//--- primitives -------------------------------------------------------------

static void TestX25519Rfc7748() {
  // RFC 7748 section 6.1: X25519(k, u) = expected.
  const std::string k = HexToRaw(
      "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
  const std::string u = HexToRaw(
      "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
  const std::string expect = HexToRaw(
      "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
  CHECK(k.size() == 32);
  CHECK(u.size() == 32);

  std::string out;
  CHECK(VoiceX25519SharedSecret(k, u, &out));
  CHECK(out == expect);

  // A low-order peer key (u = 0) yields the all-zero secret, which is refused.
  std::string zero(32, '\0');
  std::string bad;
  CHECK(!VoiceX25519SharedSecret(k, zero, &bad));

  // Wrong sizes are refused.
  CHECK(!VoiceX25519SharedSecret(k.substr(0, 31), u, &out));
  CHECK(!VoiceX25519SharedSecret(k, u.substr(0, 31), &out));

  // A public key derives from its own secret and is 32 bytes.
  std::string pub;
  CHECK(VoiceX25519PublicKey(k, &pub));
  CHECK(pub.size() == 32);
}

static void TestHkdfRfc5869() {
  // RFC 5869 test case 1 (SHA-256).
  const std::string ikm = std::string(22, (char)0x0b);
  const std::string salt =
      HexToRaw("000102030405060708090a0b0c");
  const std::string info = HexToRaw("f0f1f2f3f4f5f6f7f8f9");
  const std::string expect = HexToRaw(
      "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf");
  std::string out;
  CHECK(VoiceHkdfSha256(ikm, salt, info, &out));
  CHECK(out.size() == 32);
  CHECK(out == expect);

  // Empty IKM is refused (nothing to derive from).
  CHECK(!VoiceHkdfSha256("", salt, info, &out));
}

//--- identity ---------------------------------------------------------------

static void TestIdentitySignVerify() {
  VoiceIdentity id = VoiceIdentity::FromSeed(Seed(1));
  CHECK(id.valid());
  CHECK(id.canSign());
  CHECK(id.publicKey().size() == 32);
  CHECK(id.publicKeyHex().size() == 64);

  const std::string msg = "berry voice handshake";
  std::string sig;
  CHECK(id.Sign(msg, &sig));
  CHECK(sig.size() == 64);

  // A public-only identity can verify but not sign.
  VoiceIdentity pubOnly = VoiceIdentity::FromPublicKey(id.publicKey());
  CHECK(pubOnly.valid());
  CHECK(!pubOnly.canSign());
  std::string ignored;
  CHECK(!pubOnly.Sign(msg, &ignored));

  // The signature verifies against the public key.
  CHECK(crypto::Ed25519Verify(msg, sig, id.publicKey()));

  // One flipped message byte is refused.
  std::string tampered = msg;
  tampered[3] ^= 1;
  CHECK(!crypto::Ed25519Verify(tampered, sig, id.publicKey()));

  // One flipped signature byte is refused.
  std::string sigBad = sig;
  sigBad[5] ^= 1;
  CHECK(!crypto::Ed25519Verify(msg, sigBad, id.publicKey()));

  // A different key is refused.
  VoiceIdentity other = VoiceIdentity::FromSeed(Seed(99));
  CHECK(!crypto::Ed25519Verify(msg, sig, other.publicKey()));

  // Derivation is deterministic.
  VoiceIdentity again = VoiceIdentity::FromSeed(Seed(1));
  CHECK(again.publicKey() == id.publicKey());
}

//--- trust anchor -----------------------------------------------------------

static void TestTrustAnchor() {
  VoiceTrustAnchor none;
  CHECK(!none.Valid());   // the default accepts nothing

  VoiceTrustAnchor keys;
  keys.kind = VoiceTrustAnchor::Kind::Ed25519Keys;
  CHECK(!keys.Valid());   // no keys yet
  VoiceIdentity a = VoiceIdentity::FromSeed(Seed(1));
  VoiceIdentity b = VoiceIdentity::FromSeed(Seed(2));
  CHECK(keys.AddPublicKeyHex(a.publicKeyHex()));
  CHECK(keys.Valid());
  CHECK(keys.Allows(a.publicKey()));
  CHECK(!keys.Allows(b.publicKey()));

  // A malformed hex key is refused and does not change the anchor.
  CHECK(!keys.AddPublicKeyHex("not-hex"));
  CHECK(!keys.AddPublicKeyHex(std::string(64, 'z')));

  VoiceTrustAnchor room;
  CHECK(!room.SetRoomKey("short"));      // under 16 bytes
  CHECK(room.SetRoomKey(std::string(32, 'k')));
  CHECK(room.Valid());
  CHECK(room.kind == VoiceTrustAnchor::Kind::RoomKey);
}

//--- announce framing -------------------------------------------------------

static void TestAnnounceRoundTripAndRejects() {
  VoiceIdentity id = VoiceIdentity::FromSeed(Seed(3));
  VoiceAnnounce a;
  a.identityKey = id.publicKey();
  a.ephemeralKey = std::string(32, 'E');
  a.nonce = std::string(16, 'N');
  std::string sig;
  CHECK(id.Sign(VoiceAnnounceTranscript(a), &sig));
  a.auth = sig;

  const std::string wire = VoiceEncodeAnnounce(a);
  VoiceAnnounce got;
  CHECK(VoiceDecodeAnnounce(wire, &got));
  CHECK(got.identityKey == a.identityKey);
  CHECK(got.ephemeralKey == a.ephemeralKey);
  CHECK(got.nonce == a.nonce);
  CHECK(got.auth == a.auth);

  // Bad magic.
  std::string bad = wire;
  bad[0] = 'X';
  CHECK(!VoiceDecodeAnnounce(bad, &got));
  // Bad version.
  bad = wire;
  bad[4] = (char)9;
  CHECK(!VoiceDecodeAnnounce(bad, &got));
  // Bad mode.
  bad = wire;
  bad[5] = (char)7;
  CHECK(!VoiceDecodeAnnounce(bad, &got));
  // Truncated.
  CHECK(!VoiceDecodeAnnounce(wire.substr(0, wire.size() - 1), &got));
  // Trailing slack (a byte appended after the auth) is refused.
  CHECK(!VoiceDecodeAnnounce(wire + std::string(1, 'x'), &got));
  // Too short.
  CHECK(!VoiceDecodeAnnounce("BVAN", &got));

  // The announce verifies against an anchor that holds the identity.
  VoiceTrustAnchor anchor;
  anchor.kind = VoiceTrustAnchor::Kind::Ed25519Keys;
  CHECK(anchor.AddPublicKeyHex(id.publicKeyHex()));
  std::string fp;
  CHECK(VoiceVerifyAnnounce(a, anchor, &fp));
  CHECK(fp == id.publicKeyHex());

  // One flipped byte in the transcript field is refused: rebuild the announce
  // with a changed ephemeral key but the old signature.
  VoiceAnnounce tampered = a;
  tampered.ephemeralKey[0] ^= 1;
  CHECK(!VoiceVerifyAnnounce(tampered, anchor, &fp));

  // A flipped signature byte is refused.
  VoiceAnnounce sigBad = a;
  sigBad.auth[7] ^= 1;
  CHECK(!VoiceVerifyAnnounce(sigBad, anchor, &fp));

  // An unknown identity key is refused even with a valid self-signature.
  VoiceIdentity stranger = VoiceIdentity::FromSeed(Seed(77));
  VoiceAnnounce fromStranger;
  fromStranger.identityKey = stranger.publicKey();
  fromStranger.ephemeralKey = std::string(32, 'F');
  fromStranger.nonce = std::string(16, 'M');
  std::string ssig;
  CHECK(stranger.Sign(VoiceAnnounceTranscript(fromStranger), &ssig));
  fromStranger.auth = ssig;
  CHECK(!VoiceVerifyAnnounce(fromStranger, anchor, &fp));
}

//--- sessions ---------------------------------------------------------------

// Two ends of a handshake, sharing a session key and opposite directions.
struct Pair {
  VoiceSecureChannel a;
  VoiceSecureChannel b;
  std::string fpA;   // A's view of B's fingerprint
  std::string fpB;   // B's view of A's fingerprint
};

bool Handshake(Pair* p) {
  const std::string& annA = p->a.announce();
  const std::string& annB = p->b.announce();
  if (annA.empty() || annB.empty()) return false;
  // Cross-deliver, order-independent.
  p->fpA = p->a.AcceptAnnounce(annB);
  p->fpB = p->b.AcceptAnnounce(annA);
  return !p->fpA.empty() && !p->fpB.empty();
}

static void TestSecureChannelHandshake() {
  VoiceIdentity idA = VoiceIdentity::FromSeed(Seed(10));
  VoiceIdentity idB = VoiceIdentity::FromSeed(Seed(20));

  VoiceTrustAnchor anchorA;
  anchorA.kind = VoiceTrustAnchor::Kind::Ed25519Keys;
  CHECK(anchorA.AddPublicKeyHex(idB.publicKeyHex()));
  VoiceTrustAnchor anchorB;
  anchorB.kind = VoiceTrustAnchor::Kind::Ed25519Keys;
  CHECK(anchorB.AddPublicKeyHex(idA.publicKeyHex()));

  Pair p;
  CHECK(p.a.Configure(idA, anchorA));
  CHECK(p.b.Configure(idB, anchorB));
  CHECK(p.a.configured() && p.b.configured());
  CHECK(Handshake(&p));

  // A sees B's identity as the fingerprint; B sees A's.
  CHECK(p.fpA == idB.publicKeyHex());
  CHECK(p.fpB == idA.publicKeyHex());
  CHECK(p.a.sessionCount() == 1 && p.b.sessionCount() == 1);

  VoiceSession* sa = p.a.Session(p.fpA);
  VoiceSession* sb = p.b.Session(p.fpB);
  CHECK(sa != nullptr && sb != nullptr);
  // Both derive the same key, in opposite directions.
  CHECK(sa->key() == sb->key());
  CHECK(sa->direction() != sb->direction());

  // A seals, B opens.
  const std::string aad = "frame-header";
  std::string sealed;
  CHECK(sa->Seal(1, aad, "hello voice", &sealed));
  std::string plain;
  CHECK(sb->Open(1, aad, sealed, &plain));
  CHECK(plain == "hello voice");
}

static void TestSecureChannelRefusals() {
  VoiceIdentity idA = VoiceIdentity::FromSeed(Seed(30));
  VoiceIdentity idB = VoiceIdentity::FromSeed(Seed(40));
  VoiceIdentity stranger = VoiceIdentity::FromSeed(Seed(50));

  VoiceTrustAnchor anchorA;
  anchorA.kind = VoiceTrustAnchor::Kind::Ed25519Keys;
  CHECK(anchorA.AddPublicKeyHex(idB.publicKeyHex()));

  VoiceSecureChannel a;
  CHECK(a.Configure(idA, anchorA));
  CHECK(!a.announce().empty());

  // A peer with an unconfigured (empty) anchor accepts nothing.
  VoiceSecureChannel unconfigured;
  CHECK(!unconfigured.Configure(idA, VoiceTrustAnchor{}));
  CHECK(unconfigured.AcceptAnnounce(a.announce()).empty());

  // A stranger's announce is refused: not in the anchor.
  VoiceTrustAnchor strangerAnchor;
  strangerAnchor.kind = VoiceTrustAnchor::Kind::Ed25519Keys;
  CHECK(strangerAnchor.AddPublicKeyHex(stranger.publicKeyHex()));
  VoiceSecureChannel strangerCh;
  CHECK(strangerCh.Configure(stranger, strangerAnchor));
  CHECK(a.AcceptAnnounce(strangerCh.announce()).empty());

  // Garbage and a truncated announce are refused.
  CHECK(a.AcceptAnnounce("garbage").empty());
  CHECK(a.AcceptAnnounce(a.announce().substr(0, 10)).empty());

  // A reflection of our own announce is refused (no self-session).
  CHECK(a.AcceptAnnounce(a.announce()).empty());

  // A tampered announce is refused.
  std::string tampered = strangerCh.announce();
  tampered[tampered.size() - 1] ^= 1;
  CHECK(a.AcceptAnnounce(tampered).empty());
}

static void TestRoomKeyMode() {
  const std::string roomKey(32, 'R');
  VoiceIdentity idA = VoiceIdentity::FromSeed(Seed(60));
  VoiceIdentity idB = VoiceIdentity::FromSeed(Seed(70));

  VoiceTrustAnchor anchor;
  CHECK(anchor.SetRoomKey(roomKey));

  VoiceSecureChannel a, b;
  CHECK(a.Configure(idA, anchor));
  CHECK(b.Configure(idB, anchor));
  const std::string fpA = a.AcceptAnnounce(b.announce());
  const std::string fpB = b.AcceptAnnounce(a.announce());
  CHECK(!fpA.empty() && !fpB.empty());

  // Both ends derive the same key.
  VoiceSession* sa = a.Session(fpA);
  VoiceSession* sb = b.Session(fpB);
  CHECK(sa && sb);
  CHECK(sa->key() == sb->key());

  std::string sealed, plain;
  CHECK(sa->Seal(1, "h", "room keyed", &sealed));
  CHECK(sb->Open(1, "h", sealed, &plain));
  CHECK(plain == "room keyed");

  // A peer with a different room key is refused.
  VoiceTrustAnchor otherKey;
  CHECK(otherKey.SetRoomKey(std::string(32, 'X')));
  VoiceSecureChannel c;
  CHECK(c.Configure(idA, otherKey));
  CHECK(a.AcceptAnnounce(c.announce()).empty());
}

static void TestSessionAeadAndReplay() {
  VoiceIdentity idA = VoiceIdentity::FromSeed(Seed(80));
  VoiceIdentity idB = VoiceIdentity::FromSeed(Seed(90));
  VoiceTrustAnchor anchor;
  CHECK(anchor.SetRoomKey(std::string(32, 'K')));

  VoiceSecureChannel a, b;
  CHECK(a.Configure(idA, anchor));
  CHECK(b.Configure(idB, anchor));
  const std::string fpA = a.AcceptAnnounce(b.announce());
  const std::string fpB = b.AcceptAnnounce(a.announce());
  CHECK(!fpA.empty() && !fpB.empty());
  VoiceSession* sa = a.Session(fpA);
  VoiceSession* sb = b.Session(fpB);
  CHECK(sa && sb);

  const std::string aad = "berry|frame";
  std::string sealed, plain;

  // A flipped ciphertext byte fails the tag.
  CHECK(sa->Seal(1, aad, "payload-one", &sealed));
  std::string tampered = sealed;
  tampered[0] ^= 1;
  CHECK(!sb->Open(1, aad, tampered, &plain));
  // The real frame still opens (the forged one did not consume the window).
  CHECK(sb->Open(1, aad, sealed, &plain));
  CHECK(plain == "payload-one");

  // A replayed frame is refused.
  CHECK(!sb->Open(1, aad, sealed, &plain));

  // A wrong AAD is refused.
  CHECK(sa->Seal(2, aad, "payload-two", &sealed));
  CHECK(!sb->Open(2, "other-aad", sealed, &plain));
  CHECK(sb->Open(2, aad, sealed, &plain));
  CHECK(plain == "payload-two");

  // A truncated frame is refused.
  CHECK(!sb->Open(3, aad, std::string(4, 'x'), &plain));

  // Non-monotonic send sequence is refused (would reuse a nonce).
  CHECK(!sa->Seal(2, aad, "reuse", &sealed));
  CHECK(!sa->Seal(0, aad, "reuse", &sealed));
  CHECK(sa->Seal(3, aad, "three", &sealed));

  // Reordering inside the replay window is accepted; a duplicate and an
  // out-of-window frame are refused. Seal 4..10 on A, remembering each frame,
  // deliver 10 first, then 6 (in window), then a duplicate 6 (refused) and 1
  // (out of window, refused).
  std::vector<std::string> frames(11);
  for (u64 s = 4; s <= 10; ++s) {
    CHECK(sa->Seal(s, aad, "seq", &frames[s]));
  }
  CHECK(sb->Open(10, aad, frames[10], &plain));
  CHECK(sb->Open(6, aad, frames[6], &plain));       // in window, reordered
  CHECK(!sb->Open(6, aad, frames[6], &plain));      // duplicate: refused
  CHECK(!sb->Open(1, aad, frames[1], &plain));      // out of window: refused
  CHECK(sb->Open(9, aad, frames[9], &plain));       // still in window
}

static void TestSessionReset() {
  VoiceIdentity idA = VoiceIdentity::FromSeed(Seed(100));
  VoiceIdentity idB = VoiceIdentity::FromSeed(Seed(110));
  VoiceTrustAnchor anchor;
  CHECK(anchor.SetRoomKey(std::string(32, 'Z')));
  VoiceSecureChannel a, b;
  CHECK(a.Configure(idA, anchor));
  CHECK(b.Configure(idB, anchor));
  const std::string fpA = a.AcceptAnnounce(b.announce());
  const std::string fpB = b.AcceptAnnounce(a.announce());
  VoiceSession* sa = a.Session(fpA);
  VoiceSession* sb = b.Session(fpB);
  CHECK(sa && sb);

  std::string sealed, plain;
  CHECK(sa->Seal(5, "a", "hi", &sealed));
  CHECK(sb->Open(5, "a", sealed, &plain));

  // Reset forgets the counters: the same sequence can be used again.
  sa->Reset();
  sb->Reset();
  CHECK(sa->Seal(5, "a", "again", &sealed));
  CHECK(sb->Open(5, "a", sealed, &plain));
  CHECK(plain == "again");

  // Rotating the ephemeral drops every session.
  a.RotateEphemeral();
  CHECK(a.sessionCount() == 0);
  CHECK(a.Session(fpA) == nullptr);
}

static void TestCryptoAvailable() {
  // The host test always compiles the backend in; assert the flag and that a
  // CSPRNG draw works.
  CHECK(VoiceCryptoAvailable());
  std::string r(32, '\0');
  CHECK(VoiceRandomBytes(&r[0], r.size()));
  std::string r2(32, '\0');
  CHECK(VoiceRandomBytes(&r2[0], r2.size()));
  CHECK(r != r2);   // a real CSPRNG does not repeat
}

int main() {
  TestCryptoAvailable();
  TestX25519Rfc7748();
  TestHkdfRfc5869();
  TestIdentitySignVerify();
  TestTrustAnchor();
  TestAnnounceRoundTripAndRejects();
  TestSecureChannelHandshake();
  TestSecureChannelRefusals();
  TestRoomKeyMode();
  TestSessionAeadAndReplay();
  TestSessionReset();

  if (g_fail == 0) {
    printf("test_voice_crypto: ALL OK\n");
    return 0;
  }
  printf("test_voice_crypto: %d FAILURES\n", g_fail);
  return 1;
}
