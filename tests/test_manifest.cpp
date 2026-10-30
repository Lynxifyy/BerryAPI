// Host tests for capability manifests + signing (Phase 1).
//
// Covers:
//   * SHA-256 / HMAC-SHA256 against published FIPS-180-4 / RFC 4231 vectors.
//   * Capability parsing, scoping, wildcard coverage and set intersection.
//   * Manifest parsing with fail-closed rejection of malformed input.
//   * Entry-path traversal rejection.
//   * Signature verification: tamper detection + hash-mismatch detection.
//   * ManifestStore registration, one-time approval, version-bound grants,
//     and the declared-set cap on approvals.
#include "berry/manifest.h"
#include "berry/module.h"   // kCap* mask bits

#include <cstdio>
#include <string>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static void TestCryptoVectors() {
  // FIPS 180-4 SHA-256 known-answer tests.
  CHECK(crypto::Sha256Hex(std::string("")) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(crypto::Sha256Hex(std::string("abc")) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(crypto::Sha256Hex(
            std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  // RFC 4231 HMAC-SHA256 test case 1: key = 20 x 0x0b, data = "Hi There".
  const std::string key1(20, '\x0b');
  CHECK(crypto::HmacSha256Hex(key1, "Hi There") ==
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

  // RFC 4231 test case 2: key = "Jefe", data = "what do ya want for nothing?".
  CHECK(crypto::HmacSha256Hex("Jefe", "what do ya want for nothing?") ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

  // Long key (>64 bytes) exercises the key-hashing path.
  const std::string longKey(131, '\xaa');
  CHECK(crypto::HmacSha256Hex(longKey, "Test Using Larger Than Block-Size Key - Hash Key First") ==
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

  // Constant-time compare + hex validation.
  CHECK(crypto::ConstantTimeEquals("abcd", "abcd"));
  CHECK(!crypto::ConstantTimeEquals("abcd", "abce"));
  CHECK(!crypto::ConstantTimeEquals("abcd", "abcde"));
  CHECK(crypto::IsHex("00ff10"));
  CHECK(!crypto::IsHex("00ff1"));
  CHECK(!crypto::IsHex("xyz0"));
}

static void TestCapabilities() {
  Capability cap;
  CHECK(Capability::Parse("render", &cap));
  CHECK(cap.kind == CapKind::Render);
  CHECK(cap.scope.empty());
  CHECK(cap.Canonical() == "render");

  CHECK(Capability::Parse("network:domain=api.example.com", &cap));
  CHECK(cap.kind == CapKind::Network);
  CHECK(cap.scope == "domain=api.example.com");

  CHECK(Capability::Parse("fileio:own-dir", &cap));
  CHECK(cap.kind == CapKind::FileIO);
  CHECK(cap.scope == "own-dir");

  // `packets` is its own kind: inspecting packets is a different authority from
  // sending them, so a network grant must not reach it.
  CHECK(Capability::Parse("packets", &cap));
  CHECK(cap.kind == CapKind::Packets);
  CHECK(CapKindFromName("packets") == CapKind::Packets);
  CHECK(std::string(CapKindName(CapKind::Packets)) == "packets");
  Capability netGrant;
  CHECK(Capability::Parse("network:*", &netGrant));
  Capability wantPackets;
  CHECK(Capability::Parse("packets", &wantPackets));
  CHECK(!netGrant.Covers(wantPackets));

  // Unknown kind and empty scope fail closed.
  CHECK(!Capability::Parse("teleport", &cap));
  CHECK(!Capability::Parse("", &cap));
  CHECK(!Capability::Parse("render:", &cap));
  CHECK(!Capability::Parse(":render", &cap));

  // Wildcard grant covers any scope of the same kind.
  Capability grant, want;
  CHECK(Capability::Parse("network:*", &grant));
  CHECK(Capability::Parse("network:domain=a.com", &want));
  CHECK(grant.Covers(want));

  // A narrow grant does not cover a broader/different scope.
  CHECK(Capability::Parse("network:domain=a.com", &grant));
  CHECK(Capability::Parse("network:domain=b.com", &want));
  CHECK(!grant.Covers(want));

  // Kind mismatch never covers.
  CHECK(Capability::Parse("render", &grant));
  CHECK(Capability::Parse("world", &want));
  CHECK(!grant.Covers(want));

  // Set: duplicates rejected, canonical form is order-independent.
  CapabilitySet a;
  CHECK(a.Add("world"));
  CHECK(a.Add("combat"));
  CHECK(!a.Add("world"));      // duplicate
  CHECK(!a.Add("nonsense"));   // unknown
  CapabilitySet b;
  CHECK(b.Add("combat"));
  CHECK(b.Add("world"));
  CHECK(a.Canonical() == b.Canonical());
  CHECK(a.Canonical() == "combat,world");

  // Coverage + module mask.
  CapabilitySet need;
  CHECK(need.Add("world"));
  CHECK(a.Covers(need));
  CHECK(!need.Covers(a));
  CHECK(a.ToModuleMask() == (kCapWorld | kCapCombat));

  // The crystal advisor has its own grant, separate from combat: a script that
  // asks for combat does not thereby get the advisor, and vice versa.
  CHECK(IsGrantableKind(CapKind::Crystal));
  CHECK(CapKindFromName("crystal") == CapKind::Crystal);
  CHECK(std::string(CapKindName(CapKind::Crystal)) == "crystal");
  CapabilitySet crystal;
  CHECK(crystal.Add("crystal"));
  CHECK(crystal.CoversCap([] { Capability c; c.kind = CapKind::Crystal; return c; }()));
  CapabilitySet combat;
  CHECK(combat.Add("combat"));
  CHECK(!combat.CoversCap([] { Capability c; c.kind = CapKind::Crystal; return c; }()));
  CHECK(!crystal.CoversCap([] { Capability c; c.kind = CapKind::Combat; return c; }()));

  // The read-only game reads (inventory/server/chat/screen) are their own
  // grant, separate from world and combat: a script that asks for the world
  // does not thereby get the inventory, and vice versa.
  CHECK(IsGrantableKind(CapKind::GameReads));
  CHECK(CapKindFromName("gamereads") == CapKind::GameReads);
  CHECK(std::string(CapKindName(CapKind::GameReads)) == "gamereads");
  CapabilitySet reads;
  CHECK(reads.Add("gamereads"));
  CHECK(reads.CoversCap([] { Capability c; c.kind = CapKind::GameReads; return c; }()));
  CHECK(!a.CoversCap([] { Capability c; c.kind = CapKind::GameReads; return c; }()));
  CHECK(!reads.CoversCap([] { Capability c; c.kind = CapKind::World; return c; }()));
}

static const char* kManifestJson =
    "{"
    "\"name\":\"cps-counter\","
    "\"version\":\"1.0.0\","
    "\"entry\":\"main.luau\","
    "\"author\":\"Lynxifyy\","
    "\"description\":\"Clicks per second HUD\","
    "\"capabilities\":[\"render\",\"hud\",\"input\"]"
    "}";

static void TestManifestParse() {
  ScriptManifest m;
  ManifestError err = ManifestError::Ok;
  std::string msg;
  CHECK(ScriptManifest::Parse(kManifestJson, &m, &err, &msg));
  CHECK(err == ManifestError::Ok);
  CHECK(m.name == "cps-counter");
  CHECK(m.version == "1.0.0");
  CHECK(m.entry == "main.luau");
  CHECK(m.author == "Lynxifyy");
  CHECK(m.capabilities.Count() == 3);
  CHECK(m.capabilities.Has(CapKind::Hud));

  // Malformed JSON.
  ScriptManifest bad;
  CHECK(!ScriptManifest::Parse("{not json", &bad, &err, &msg));
  CHECK(err == ManifestError::MalformedJson);
  // Trailing garbage is rejected (fail closed).
  CHECK(!ScriptManifest::Parse("{\"name\":\"a\",\"version\":\"1\",\"entry\":\"m\","
                               "\"capabilities\":[]} junk", &bad, &err, &msg));
  CHECK(err == ManifestError::MalformedJson);

  // Missing fields.
  CHECK(!ScriptManifest::Parse("{\"version\":\"1.0.0\",\"entry\":\"m\","
                               "\"capabilities\":[]}", &bad, &err, &msg));
  CHECK(err == ManifestError::MissingField);

  // Unknown capability.
  CHECK(!ScriptManifest::Parse("{\"name\":\"a\",\"version\":\"1\",\"entry\":\"m\","
                               "\"capabilities\":[\"render\",\"fly\"]}", &bad,
                               &err, &msg));
  CHECK(err == ManifestError::UnknownCapability);

  // Duplicate capability.
  CHECK(!ScriptManifest::Parse("{\"name\":\"a\",\"version\":\"1\",\"entry\":\"m\","
                               "\"capabilities\":[\"render\",\"render\"]}", &bad,
                               &err, &msg));
  CHECK(err == ManifestError::DuplicateCapability);

  // Capabilities must be an array of strings.
  CHECK(!ScriptManifest::Parse("{\"name\":\"a\",\"version\":\"1\",\"entry\":\"m\","
                               "\"capabilities\":\"render\"}", &bad, &err, &msg));
  CHECK(err == ManifestError::MalformedJson);

  // Duplicate object key rejected.
  CHECK(!ScriptManifest::Parse("{\"name\":\"a\",\"name\":\"b\",\"version\":\"1\","
                               "\"entry\":\"m\",\"capabilities\":[]}", &bad, &err,
                               &msg));
  CHECK(err == ManifestError::MalformedJson);
}

static void TestEntryPaths() {
  CHECK(ScriptManifest::ValidEntryPath("main.luau"));
  CHECK(ScriptManifest::ValidEntryPath("src/main.luau"));
  CHECK(ScriptManifest::ValidEntryPath("a/b/c.js"));

  CHECK(!ScriptManifest::ValidEntryPath(""));
  CHECK(!ScriptManifest::ValidEntryPath("/etc/passwd"));
  CHECK(!ScriptManifest::ValidEntryPath("../../etc/passwd"));
  CHECK(!ScriptManifest::ValidEntryPath("src/../../x"));
  CHECK(!ScriptManifest::ValidEntryPath("./main.luau"));
  CHECK(!ScriptManifest::ValidEntryPath("a//b"));
  CHECK(!ScriptManifest::ValidEntryPath("C:\\evil.luau"));
  CHECK(!ScriptManifest::ValidEntryPath("a\\b.luau"));
}

static void TestSigning() {
  const std::string key = "deadbeef0123456789";
  ScriptManifest m;
  ManifestError err = ManifestError::Ok;
  CHECK(ScriptManifest::Parse(kManifestJson, &m, &err, nullptr));

  CHECK(!m.Verify(key, &err));   // unsigned yet
  CHECK(err == ManifestError::Unsigned);

  CHECK(m.Sign(key));
  CHECK(m.signatureAlgo == "hmac-sha256");
  CHECK(m.signature.size() == 64);
  CHECK(m.payloadHash == m.ComputeHash());
  CHECK(m.Verify(key, &err));
  CHECK(err == ManifestError::Ok);

  // Wrong key fails.
  CHECK(!m.Verify("wrong-key", &err));
  CHECK(err == ManifestError::BadSignature);

  // Tampered capability list invalidates both hash and signature.
  ScriptManifest tampered = m;
  CapabilitySet widened;
  CHECK(widened.Add("render"));
  CHECK(widened.Add("hud"));
  CHECK(widened.Add("input"));
  CHECK(widened.Add("network:*"));
  tampered.capabilities = widened;
  CHECK(!tampered.Verify(key, &err));
  CHECK(err == ManifestError::HashMismatch);

  // Forged signature over a tampered payload is still rejected: the attacker
  // cannot produce a valid HMAC without the key.
  ScriptManifest forged = tampered;
  forged.Sign("attacker-key");
  CHECK(!forged.Verify(key, &err));
  CHECK(err == ManifestError::BadSignature);

  // Canonical payload is stable and excludes description/signature.
  ScriptManifest cosmetic = m;
  cosmetic.description = "totally different text";
  CHECK(cosmetic.ComputeHash() == m.ComputeHash());

  // Capability order does not change the hash.
  ScriptManifest reordered;
  CHECK(ScriptManifest::Parse(
      "{\"name\":\"cps-counter\",\"version\":\"1.0.0\",\"entry\":\"main.luau\","
      "\"author\":\"Lynxifyy\",\"capabilities\":[\"input\",\"hud\",\"render\"]}",
      &reordered, &err, nullptr));
  CHECK(reordered.ComputeHash() == m.ComputeHash());
}

static void TestStore() {
  ManifestStore& store = ManifestStore::Instance();
  store.Clear();
  const std::string key = "test-signing-key";

  ScriptManifest m;
  CHECK(ScriptManifest::Parse(kManifestJson, &m, nullptr, nullptr));
  CHECK(m.Sign(key));

  // Unsigned manifest cannot register.
  ScriptManifest unsignedM;
  CHECK(ScriptManifest::Parse(kManifestJson, &unsignedM, nullptr, nullptr));
  ManifestError err = ManifestError::Ok;
  CHECK(!store.Register(unsignedM, key, &err));
  CHECK(err == ManifestError::Unsigned);

  // Signed manifest registers; duplicates rejected.
  CHECK(store.Register(m, key, &err));
  CHECK(err == ManifestError::Ok);
  CHECK(!store.Register(m, key, &err));
  CHECK(store.Count() == 1);

  // Not approved yet: no effective capabilities.
  CHECK(!store.IsApproved("cps-counter"));
  CHECK(store.EffectiveCapabilities("cps-counter").empty());

  // One-time approval grants exactly the declared set.
  CHECK(store.Approve("cps-counter", 1000));
  CHECK(store.IsApproved("cps-counter"));
  CapabilitySet eff = store.EffectiveCapabilities("cps-counter");
  CHECK(eff.Count() == 3);
  CHECK(eff.Has(CapKind::Render));
  CHECK(eff.Has(CapKind::Hud));
  CHECK(eff.Has(CapKind::Input));

  const CapabilityGrant* grant = store.Grant("cps-counter");
  CHECK(grant != nullptr);
  CHECK(grant->version == "1.0.0");
  CHECK(grant->approvedAtMs == 1000);

  // Approving an unknown script fails.
  CHECK(!store.Approve("ghost", 1000));

  // Revoke drops the grant.
  CHECK(store.Revoke("cps-counter"));
  CHECK(!store.IsApproved("cps-counter"));

  // A version bump invalidates an existing approval (new caps cannot ride an
  // old approval).
  store.Clear();
  CHECK(store.Register(m, key, nullptr));
  CHECK(store.Approve("cps-counter", 2000));
  CHECK(store.IsApproved("cps-counter"));

  ScriptManifest v2;
  CHECK(ScriptManifest::Parse(
      "{\"name\":\"cps-counter\",\"version\":\"2.0.0\",\"entry\":\"main.luau\","
      "\"author\":\"Lynxifyy\",\"capabilities\":[\"render\",\"hud\",\"input\","
      "\"network:*\"]}", &v2, nullptr, nullptr));
  CHECK(v2.Sign(key));
  store.Clear();
  CHECK(store.Register(v2, key, nullptr));
  CHECK(store.Approve("cps-counter", 3000));
  const CapabilityGrant* g2 = store.Grant("cps-counter");
  CHECK(g2 != nullptr);
  CHECK(g2->version == "2.0.0");

  store.Clear();
}


//----------------------------------------------------------------------------
// Ed25519 signing + the source binding.
//----------------------------------------------------------------------------

// RFC 8032 §7.1 test vector 1: seed, public key and the empty-message
// signature. Pins the vendored implementation to the standard.
static const char* kRfcSeed1 =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
static const char* kRfcPub1 =
    "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
static const char* kRfcSig1 =
    "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb882159"
    "0a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";

static void TestEd25519Vectors() {
  std::string seed, pub, sec;
  CHECK(crypto::FromHex(kRfcSeed1, &seed, 32));
  CHECK(crypto::Ed25519KeypairFromSeed(seed, &pub, &sec));
  CHECK(sec.size() == 64);
  CHECK(crypto::ToHexLower(pub) == kRfcPub1);

  std::string sig;
  CHECK(crypto::Ed25519Sign("", sec, &sig));
  CHECK(crypto::ToHexLower(sig) == kRfcSig1);

  // Verify accepts the vector, rejects a flipped bit, and refuses bad sizes.
  CHECK(crypto::Ed25519Verify("", sig, pub));
  std::string tampered = sig;
  tampered[0] = (char)(tampered[0] ^ 1);
  CHECK(!crypto::Ed25519Verify("", tampered, pub));
  CHECK(!crypto::Ed25519Verify("x", sig, pub));
  CHECK(!crypto::Ed25519Verify("", sig.substr(0, 63), pub));
  CHECK(!crypto::Ed25519Verify("", sig, pub.substr(0, 31)));
  CHECK(!crypto::Ed25519Sign("", sec.substr(0, 63), &sig));

  // A malformed hex public key never verifies.
  std::string pk;
  CHECK(!crypto::Ed25519PublicKeyFromHex("not-hex", &pk));
  CHECK(!crypto::Ed25519PublicKeyFromHex(std::string(kRfcPub1) + "00", &pk));
  CHECK(crypto::Ed25519PublicKeyFromHex(kRfcPub1, &pk));
}

static void TestEd25519Manifest() {
  std::string seed, pub, sec;
  CHECK(crypto::FromHex(kRfcSeed1, &seed, 32));
  CHECK(crypto::Ed25519KeypairFromSeed(seed, &pub, &sec));
  const std::string pubHex = crypto::ToHexLower(pub);

  ScriptManifest m;
  ManifestError err = ManifestError::Ok;
  CHECK(ScriptManifest::Parse(kManifestJson, &m, &err, nullptr));
  m.sourceSha256 = ScriptManifest::SourceHash("func on_load\nend\n");

  // Signing fills the algorithm and the payload hash, and verification needs
  // no secret at all.
  CHECK(m.SignEd25519(sec));
  CHECK(m.signatureAlgo == "ed25519");
  CHECK(m.signature.size() == 128);
  CHECK(m.payloadHash == m.ComputeHash());
  CHECK(m.VerifyEd25519(pubHex, &err));
  CHECK(err == ManifestError::Ok);

  // A different key is rejected.
  std::string otherPub, otherSec, otherSeed = std::string(32, 'z');
  CHECK(crypto::Ed25519KeypairFromSeed(otherSeed, &otherPub, &otherSec));
  CHECK(!m.VerifyEd25519(crypto::ToHexLower(otherPub), &err));
  CHECK(err == ManifestError::BadSignature);

  // The HMAC verifier must not accept an Ed25519 signature, and vice versa:
  // an algorithm confusion here would let a forged HMAC pass as Ed25519.
  CHECK(!m.Verify("deadbeef", &err));
  CHECK(err == ManifestError::UnsupportedAlgorithm);

  ScriptManifest hmac;
  CHECK(ScriptManifest::Parse(kManifestJson, &hmac, &err, nullptr));
  hmac.sourceSha256 = ScriptManifest::SourceHash("x");
  CHECK(hmac.Sign("shared-secret"));
  CHECK(!hmac.VerifyEd25519(pubHex, &err));
  CHECK(err == ManifestError::UnsupportedAlgorithm);

  // A widened capability list invalidates both the hash and the signature.
  ScriptManifest widened = m;
  CapabilitySet caps;
  CHECK(caps.Add("render"));
  CHECK(caps.Add("hud"));
  CHECK(caps.Add("input"));
  CHECK(caps.Add("network:*"));
  widened.capabilities = caps;
  CHECK(!widened.VerifyEd25519(pubHex, &err));
  CHECK(err == ManifestError::HashMismatch);

  // Swapping the source while keeping the manifest fails the payload hash:
  // this is the whole point of binding source_sha256.
  ScriptManifest swapped = m;
  swapped.sourceSha256 = ScriptManifest::SourceHash("func evil\nend\n");
  CHECK(!swapped.VerifyEd25519(pubHex, &err));
  CHECK(err == ManifestError::HashMismatch);

  // The source binding is mandatory on the Ed25519 path: a manifest that signs
  // only a capability list proves nothing about the code.
  ScriptManifest unsignedCode = m;
  unsignedCode.sourceSha256.clear();
  CHECK(unsignedCode.SignEd25519(sec));
  CHECK(!unsignedCode.VerifyEd25519(pubHex, &err));
  CHECK(err == ManifestError::MissingSourceHash);
  // ...but can still be checked when the caller explicitly waives it.
  CHECK(unsignedCode.VerifyEd25519(pubHex, &err, /*requireSourceHash=*/false));

  // An unsigned manifest reports Unsigned, never a signature failure.
  ScriptManifest blank;
  CHECK(ScriptManifest::Parse(kManifestJson, &blank, &err, nullptr));
  CHECK(!blank.VerifyEd25519(pubHex, &err));
  CHECK(err == ManifestError::Unsigned);

  // Signature must be valid hex of the right length.
  ScriptManifest badSig = m;
  badSig.signature = std::string("zz") + badSig.signature.substr(2);
  CHECK(!badSig.VerifyEd25519(pubHex, &err));
  CHECK(err == ManifestError::BadSignature);
}

//----------------------------------------------------------------------------
// Script API versioning + deprecation policy (Phase 17).
//----------------------------------------------------------------------------

static void TestApiVersionParse() {
  // A declared requirement is read and validated.
  ScriptManifest m;
  ManifestError err = ManifestError::Ok;
  std::string msg;
  CHECK(ScriptManifest::Parse(
      "{\"name\":\"a\",\"version\":\"1.0.0\",\"entry\":\"m\","
      "\"api_version\":\"1.2.0\",\"capabilities\":[\"render\"]}",
      &m, &err, &msg));
  CHECK(err == ManifestError::Ok);
  CHECK(m.apiVersion == "1.2.0");

  // Absent api_version means "no requirement" (pre-versioning manifests).
  ScriptManifest noApi;
  CHECK(ScriptManifest::Parse(kManifestJson, &noApi, &err, &msg));
  CHECK(noApi.apiVersion.empty());

  // A malformed requirement fails closed rather than reading as "no
  // requirement": a typo must not silently disable the compatibility check.
  ScriptManifest bad;
  CHECK(!ScriptManifest::Parse(
      "{\"name\":\"a\",\"version\":\"1.0.0\",\"entry\":\"m\","
      "\"api_version\":\"1.2\",\"capabilities\":[\"render\"]}",
      &bad, &err, &msg));
  CHECK(err == ManifestError::BadApiVersion);

  // A non-string api_version is rejected.
  CHECK(!ScriptManifest::Parse(
      "{\"name\":\"a\",\"version\":\"1.0.0\",\"entry\":\"m\","
      "\"api_version\":7,\"capabilities\":[\"render\"]}",
      &bad, &err, &msg));
  CHECK(err == ManifestError::BadApiVersion);

  // A leading `v` and build metadata are tolerated, as SemVer::Parse does.
  ScriptManifest loose;
  CHECK(ScriptManifest::Parse(
      "{\"name\":\"a\",\"version\":\"1.0.0\",\"entry\":\"m\","
      "\"api_version\":\"v1.0.0+ci.5\",\"capabilities\":[\"render\"]}",
      &loose, &err, &msg));
  CHECK(err == ManifestError::Ok);
  CHECK(loose.apiVersion == "v1.0.0+ci.5");
}

static void TestApiVersionSatisfaction() {
  // No requirement is always satisfied.
  CHECK(ScriptManifest::ApiVersionSatisfied("", "1.0.0"));
  // Equal, older and newer requirements behave per the SemVer total order.
  CHECK(ScriptManifest::ApiVersionSatisfied("1.0.0", "1.0.0"));
  CHECK(ScriptManifest::ApiVersionSatisfied("0.9.0", "1.0.0"));
  CHECK(ScriptManifest::ApiVersionSatisfied("1.0.0", "1.2.0"));
  CHECK(!ScriptManifest::ApiVersionSatisfied("1.1.0", "1.0.0"));
  CHECK(!ScriptManifest::ApiVersionSatisfied("2.0.0", "1.9.9"));
  // A pre-release requirement is satisfied by its release.
  CHECK(ScriptManifest::ApiVersionSatisfied("1.0.0-rc.1", "1.0.0"));
  // A malformed requirement on either side fails closed.
  CHECK(!ScriptManifest::ApiVersionSatisfied("banana", "1.0.0"));
  CHECK(!ScriptManifest::ApiVersionSatisfied("1.0.0", "not-a-version"));
  // The build's own version must satisfy itself.
  CHECK(ScriptManifest::ApiVersionSatisfied(ScriptManifest::BuildApiVersion(),
                                            ScriptManifest::BuildApiVersion()));
  CHECK(std::string(ScriptManifest::BuildApiVersion()) != "");
}

static void TestApiVersionIsSigned() {
  std::string seed, pub, sec;
  CHECK(crypto::FromHex(kRfcSeed1, &seed, 32));
  CHECK(crypto::Ed25519KeypairFromSeed(seed, &pub, &sec));
  const std::string pubHex = crypto::ToHexLower(pub);

  ScriptManifest m;
  ManifestError err = ManifestError::Ok;
  CHECK(ScriptManifest::Parse(
      "{\"name\":\"a\",\"version\":\"1.0.0\",\"entry\":\"m\","
      "\"author\":\"Lynxifyy\",\"api_version\":\"1.0.0\","
      "\"capabilities\":[\"render\"]}",
      &m, &err, nullptr));
  m.sourceSha256 = ScriptManifest::SourceHash("x");
  CHECK(m.SignEd25519(sec));
  CHECK(m.VerifyEd25519(pubHex, &err));

  // The requirement is part of the payload: raising it invalidates the hash and
  // therefore the signature, so a script cannot silently demand a newer API.
  ScriptManifest raised = m;
  raised.apiVersion = "9.9.9";
  CHECK(raised.ComputeHash() != m.ComputeHash());
  CHECK(!raised.VerifyEd25519(pubHex, &err));
  CHECK(err == ManifestError::HashMismatch);

  // Dropping the requirement also changes the payload (it is a signed field).
  ScriptManifest dropped = m;
  dropped.apiVersion.clear();
  CHECK(dropped.ComputeHash() != m.ComputeHash());
  CHECK(!dropped.VerifyEd25519(pubHex, &err));
  CHECK(err == ManifestError::HashMismatch);

  // A manifest that never declared a requirement keeps its pre-versioning
  // payload byte-for-byte, so an already-published signature still verifies.
  ScriptManifest legacy;
  CHECK(ScriptManifest::Parse(kManifestJson, &legacy, &err, nullptr));
  legacy.sourceSha256 = ScriptManifest::SourceHash("x");
  CHECK(legacy.CanonicalPayload().find("api_version=") == std::string::npos);
}

static void TestTrustAnchor() {
  std::string seed, pub, sec;
  CHECK(crypto::FromHex(kRfcSeed1, &seed, 32));
  CHECK(crypto::Ed25519KeypairFromSeed(seed, &pub, &sec));
  const std::string pubHex = crypto::ToHexLower(pub);

  // An anchor is only usable when its key is well formed for its kind.
  CHECK(TrustAnchor::Ed25519Key(pubHex).Valid());
  CHECK(!TrustAnchor::Ed25519Key("short").Valid());
  CHECK(!TrustAnchor::Ed25519Key("").Valid());
  CHECK(TrustAnchor::HmacSecret("s").Valid());
  CHECK(!TrustAnchor::HmacSecret("").Valid());

  ScriptManifest m;
  CHECK(ScriptManifest::Parse(kManifestJson, &m, nullptr, nullptr));
  m.sourceSha256 = ScriptManifest::SourceHash("src");
  CHECK(m.SignEd25519(sec));

  ManifestError err = ManifestError::Ok;
  CHECK(TrustAnchor::Ed25519Key(pubHex).Verify(m, &err));
  CHECK(err == ManifestError::Ok);

  // A bad anchor fails closed rather than skipping the check.
  CHECK(!TrustAnchor::Ed25519Key("garbage").Verify(m, &err));
  CHECK(err == ManifestError::BadSignature);

  // The anchor kind must match the signature algorithm.
  CHECK(!TrustAnchor::HmacSecret("anything").Verify(m, &err));
  CHECK(err == ManifestError::UnsupportedAlgorithm);

  ScriptManifest hmacManifest;
  CHECK(ScriptManifest::Parse(kManifestJson, &hmacManifest, nullptr, nullptr));
  hmacManifest.sourceSha256 = ScriptManifest::SourceHash("src");
  CHECK(hmacManifest.Sign("dev-secret"));
  CHECK(TrustAnchor::HmacSecret("dev-secret").Verify(hmacManifest, &err));
  CHECK(err == ManifestError::Ok);
  CHECK(!TrustAnchor::HmacSecret("wrong").Verify(hmacManifest, &err));
  CHECK(!TrustAnchor::Ed25519Key(pubHex).Verify(hmacManifest, &err));
  CHECK(err == ManifestError::UnsupportedAlgorithm);
}

static void TestStoreRequiresVerifiedSource() {
  std::string seed, pub, sec;
  CHECK(crypto::FromHex(kRfcSeed1, &seed, 32));
  CHECK(crypto::Ed25519KeypairFromSeed(seed, &pub, &sec));
  const TrustAnchor anchor = TrustAnchor::Ed25519Key(crypto::ToHexLower(pub));

  ManifestStore& store = ManifestStore::Instance();
  store.Clear();

  ScriptManifest m;
  CHECK(ScriptManifest::Parse(kManifestJson, &m, nullptr, nullptr));
  m.sourceSha256 = ScriptManifest::SourceHash("code");
  CHECK(m.SignEd25519(sec));

  ManifestError err = ManifestError::Ok;
  CHECK(store.Register(m, anchor, &err));
  CHECK(err == ManifestError::Ok);

  // Registering through the wrong anchor is refused.
  ScriptManifest other;
  CHECK(ScriptManifest::Parse(
      "{\"name\":\"other\",\"version\":\"1.0.0\",\"entry\":\"m.luau\","
      "\"capabilities\":[\"render\"]}", &other, nullptr, nullptr));
  other.sourceSha256 = ScriptManifest::SourceHash("code");
  CHECK(other.SignEd25519(sec));
  CHECK(!store.Register(other, TrustAnchor::HmacSecret("nope"), &err));
  CHECK(err == ManifestError::UnsupportedAlgorithm);
  CHECK(!store.Register(other, TrustAnchor::Ed25519Key("bad-key"), &err));
  CHECK(err == ManifestError::BadSignature);

  store.Clear();
}

int main() {
  TestCryptoVectors();
  TestCapabilities();
  TestManifestParse();
  TestEntryPaths();
  TestSigning();
  TestStore();
  TestEd25519Vectors();
  TestEd25519Manifest();
  TestApiVersionParse();
  TestApiVersionSatisfaction();
  TestApiVersionIsSigned();
  TestTrustAnchor();
  TestStoreRequiresVerifiedSource();
  if (g_fail == 0) printf("test_manifest: ALL OK\n");
  return g_fail;
}
