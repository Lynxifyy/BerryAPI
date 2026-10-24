// Host tests for OTA signature packs (data-driven signatures + struct layouts).
//
// Covers:
//   * Envelope verification (BEFORE parsing): an unsigned pack, a wrong key, a
//     single flipped byte, an unknown algorithm and an expired pack are all
//     refused; a validly signed pack verifies.
//   * Anti-rollback: a signed pack whose sequence is below the floor is refused,
//     both via the trust's minSequence and via the loader's session floor.
//   * Parse: format/name/build required, unknown key refused, bad pattern and
//     duplicate id refused, bad layout refused, a layout whose build differs
//     from the pack's refused, an empty pack refused.
//   * Apply: gated by the active build fingerprint. A pack for another build
//     (or an empty active build) registers nothing.
//   * Apply: a matching pack registers its rows into SigCache and its layouts
//     into LayoutRegistry, atomically.
//   * Directory load: only `*.sigpack.json` files are picked up, a bad pack
//     does not stop the others, the per-pack report is accurate, and an older
//     pack cannot be applied after a newer one.
//   * Interop: a vector produced by tools/berry-sigpack-sign.py verifies in the
//     C++ verifier, so the two canonical payloads provably agree byte for byte.

#include "berry/sigpack.h"
#include "berry/layout.h"
#include "berry/loader.h"
#include "berry/manifest.h"
#include "berry/sigscan.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

// The RFC 8032 §7.1 test seed, the same one the manifest interop vector uses.
const char* kSeedHex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";

// The interop vector pinned by tools/check-sigpack-signer.py: a pack signed by
// tools/berry-sigpack-sign.py. If the signer's canonical payload drifts from
// the C++ one, this signature stops verifying and the test fails.
const char* kInteropPub =
    "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
const char* kInteropSig =
    "086bb2ede3285eebcfed9dc9b9cbf87240cd182583eb54fa36884388efc37852"
    "59eafcd65b561962c5fd13af66787bab14bcd7dd0bb259a7602024dcafe0c902";

std::string SeedBytes() {
  std::string s;
  CHECK(crypto::FromHex(kSeedHex, &s, 32));
  return s;
}

std::string SecretKey() {
  std::string pub, sec;
  CHECK(crypto::Ed25519KeypairFromSeed(SeedBytes(), &pub, &sec));
  return sec;
}

std::string PublicKeyHex() {
  std::string pub, sec;
  CHECK(crypto::Ed25519KeypairFromSeed(SeedBytes(), &pub, &sec));
  return crypto::ToHexLower(pub);
}

SigPackTrust Trust() {
  SigPackTrust t;
  t.publicKeyHex = PublicKeyHex();
  return t;
}

/// Builds a pack's canonical payload the way the library does, signs it, and
/// returns the complete pack JSON. `bodyJson` is the entries (signatures/
/// layouts) WITHOUT the leading comma, or "" for none.
std::string SignedPack(const std::string& name, const std::string& build,
                       u64 sequence, u64 expires,
                       const std::string& bodyJson) {
  SignaturePack p;
  p.name = name;
  p.build = build;
  p.sequence = sequence;
  p.expires = expires;
  std::string sig64;
  CHECK(crypto::Ed25519Sign(p.CanonicalPayload(), SecretKey(), &sig64));

  std::string out = "{\"format\":1,\"name\":\"" + name + "\",\"build\":\"" +
                    build + "\",\"sequence\":" + std::to_string(sequence) +
                    ",\"expires\":" + std::to_string(expires) +
                    ",\"signature\":{\"algo\":\"ed25519\",\"value\":\"" +
                    crypto::ToHexLower(sig64) + "\"}";
  if (!bodyJson.empty()) out += "," + bodyJson;
  out += "}";
  return out;
}

std::string PackBody(const std::string& sigs, const std::string& layouts = "") {
  std::string body;
  if (!sigs.empty()) body += "\"signatures\":[" + sigs + "]";
  if (!layouts.empty()) {
    if (!body.empty()) body += ",";
    body += "\"layouts\":[" + layouts + "]";
  }
  return body;
}

std::string LayoutJson(const std::string& build) {
  return "{\"name\":\"Actor\",\"version\":\"1.20\",\"build\":\"" + build +
         "\",\"size\":16,\"fields\":["
         "{\"name\":\"health\",\"type\":\"f32\",\"offset\":0},"
         "{\"name\":\"id\",\"type\":\"u64\",\"offset\":8}]}";
}

}  // namespace

//----------------------------------------------------------------------------
// Envelope verification (runs BEFORE parsing)
//----------------------------------------------------------------------------

static void TestEnvelopeVerify() {
  SigPackTrust trust = Trust();
  const std::string body = PackBody("{\"id\":\"a:rule\",\"pattern\":\"AA BB\"}");

  // A validly signed pack verifies.
  {
    SignaturePack env;
    SigPackError e = SigPackError::Ok;
    std::string err;
    const std::string json = SignedPack("p", "BUILD-A", 5, 0, body);
    CHECK(SignaturePack::VerifyEnvelope(json, trust, &env, &e, &err));
    CHECK(e == SigPackError::Ok);
    CHECK(env.name == "p");
    CHECK(env.build == "BUILD-A");
    CHECK(env.sequence == 5);
    CHECK(env.signatureAlgo == "ed25519");
  }

  // A single flipped byte in the signature -> refused.
  {
    std::string json = SignedPack("p", "BUILD-A", 5, 0, body);
    const size_t at = json.rfind("\"value\":\"") + 9;
    json[at] = (json[at] == 'a') ? 'b' : 'a';
    SignaturePack env;
    SigPackError e = SigPackError::Ok;
    CHECK(!SignaturePack::VerifyEnvelope(json, trust, &env, &e));
    CHECK(e == SigPackError::BadPackSignature);
  }

  // A single flipped byte in a SIGNED field (the build) -> refused. This is the
  // point of signing the fingerprint: re-pointing a pack at another build is a
  // tamper.
  {
    std::string json = SignedPack("p", "BUILD-A", 5, 0, body);
    const size_t at = json.find("BUILD-A");
    json[at + 5] = 'B';   // BUILD-A -> BUILD-B, signature untouched
    SignaturePack env;
    SigPackError e = SigPackError::Ok;
    CHECK(!SignaturePack::VerifyEnvelope(json, trust, &env, &e));
    CHECK(e == SigPackError::BadPackSignature);
  }

  // A flipped byte in the signed sequence -> refused (cannot lower it quietly).
  {
    std::string json = SignedPack("p", "BUILD-A", 5, 0, body);
    const size_t at = json.find("\"sequence\":5") + 11;
    json[at] = '4';
    SignaturePack env;
    SigPackError e = SigPackError::Ok;
    CHECK(!SignaturePack::VerifyEnvelope(json, trust, &env, &e));
    CHECK(e == SigPackError::BadPackSignature);
  }

  // Wrong key -> refused.
  {
    std::string json = SignedPack("p", "BUILD-A", 5, 0, body);
    SigPackTrust wrong = trust;
    wrong.publicKeyHex =
        "0000000000000000000000000000000000000000000000000000000000000001";
    SignaturePack env;
    SigPackError e = SigPackError::Ok;
    CHECK(!SignaturePack::VerifyEnvelope(json, wrong, &env, &e));
    CHECK(e == SigPackError::BadPackSignature);
  }

  // Unsigned (no signature block) -> refused.
  {
    const std::string json =
        "{\"format\":1,\"name\":\"p\",\"build\":\"BUILD-A\",\"sequence\":5,"
        "\"signatures\":[{\"id\":\"a\",\"pattern\":\"AA\"}]}";
    SignaturePack env;
    SigPackError e = SigPackError::Ok;
    CHECK(!SignaturePack::VerifyEnvelope(json, trust, &env, &e));
    CHECK(e == SigPackError::Unsigned);
  }

  // Missing sequence -> refused (anti-rollback needs it).
  {
    const std::string json =
        "{\"format\":1,\"name\":\"p\",\"build\":\"BUILD-A\","
        "\"signature\":{\"algo\":\"ed25519\",\"value\":\"" + std::string(128, 'a') +
        "\"}}";
    SignaturePack env;
    SigPackError e = SigPackError::Ok;
    CHECK(!SignaturePack::VerifyEnvelope(json, trust, &env, &e));
    CHECK(e == SigPackError::BadSequence);
  }

  // Unknown algorithm -> refused.
  {
    const std::string json =
        "{\"format\":1,\"name\":\"p\",\"build\":\"BUILD-A\",\"sequence\":1,"
        "\"signature\":{\"algo\":\"hmac-sha256\",\"value\":\"" + std::string(128, 'a') +
        "\"}}";
    SignaturePack env;
    SigPackError e = SigPackError::Ok;
    CHECK(!SignaturePack::VerifyEnvelope(json, trust, &env, &e));
    CHECK(e == SigPackError::UnknownAlgorithm);
  }

  // No trust anchor -> every pack refused (fail closed).
  {
    SigPackTrust none;
    SignaturePack env;
    SigPackError e = SigPackError::Ok;
    const std::string json = SignedPack("p", "BUILD-A", 5, 0, body);
    CHECK(!SignaturePack::VerifyEnvelope(json, none, &env, &e));
    CHECK(e == SigPackError::NoTrustAnchor);
  }
}

static void TestExpiryAndSequenceFloor() {
  const std::string body = PackBody("{\"id\":\"a:rule\",\"pattern\":\"AA BB\"}");
  const std::string json = SignedPack("p", "BUILD-A", 5, 1000, body);

  // Within the window: verifies.
  {
    SigPackTrust t = Trust();
    t.nowUnix = 999;
    SignaturePack env;
    SigPackError e = SigPackError::Ok;
    CHECK(SignaturePack::VerifyEnvelope(json, t, &env, &e));
  }
  // At/after expiry: refused.
  {
    SigPackTrust t = Trust();
    t.nowUnix = 1000;
    SignaturePack env;
    SigPackError e = SigPackError::Ok;
    CHECK(!SignaturePack::VerifyEnvelope(json, t, &env, &e));
    CHECK(e == SigPackError::Expired);
  }
  // Below the min sequence: refused.
  {
    SigPackTrust t = Trust();
    t.minSequence = 6;
    SignaturePack env;
    SigPackError e = SigPackError::Ok;
    CHECK(!SignaturePack::VerifyEnvelope(json, t, &env, &e));
    CHECK(e == SigPackError::StaleSequence);
  }
  // At the min sequence: accepted.
  {
    SigPackTrust t = Trust();
    t.minSequence = 5;
    SignaturePack env;
    SigPackError e = SigPackError::Ok;
    CHECK(SignaturePack::VerifyEnvelope(json, t, &env, &e));
  }
}

//----------------------------------------------------------------------------
// Parse
//----------------------------------------------------------------------------

static void TestParseValid() {
  SignaturePack pack;
  SigPackError e = SigPackError::Ok;
  std::string err;
  const std::string body =
      "\"signatures\":["
      "{\"id\":\"world:levelTick\",\"pattern\":\"48 8D 0D ? ? ? ? E8\"},"
      "{\"id\":\"combat:damageFn\",\"pattern\":\"AA BB\",\"from_end\":2}],"
      "\"layouts\":[" + LayoutJson("BUILD-A") + "]";
  const std::string good = SignedPack("test-pack", "BUILD-A", 1, 0, body);
  CHECK(SignaturePack::Parse(good, &pack, &e, &err));
  CHECK(e == SigPackError::Ok);
  CHECK(pack.name == "test-pack");
  CHECK(pack.build == "BUILD-A");
  CHECK(pack.sequence == 1);
  CHECK(pack.signatures.size() == 2);
  CHECK(pack.signatures[0].id == "world:levelTick");
  CHECK(pack.signatures[0].fromEndOffset == 0);
  CHECK(pack.signatures[1].id == "combat:damageFn");
  CHECK(pack.signatures[1].fromEndOffset == 2);
  CHECK(pack.layouts.size() == 1);
  CHECK(pack.layouts[0].name == "Actor");
}

static void TestParseRejections() {
  SignaturePack pack;
  SigPackError e = SigPackError::Ok;
  std::string err;

  // Not JSON.
  CHECK(!SignaturePack::Parse("not json", &pack, &e, &err));
  CHECK(e == SigPackError::BadJson);

  // Wrong format version.
  CHECK(!SignaturePack::Parse(
      "{\"format\":99,\"name\":\"p\",\"build\":\"B\",\"sequence\":1,"
      "\"signature\":{\"algo\":\"ed25519\",\"value\":\"" + std::string(128, 'a') +
      "\"},\"signatures\":[{\"id\":\"a\",\"pattern\":\"AA\"}]}",
      &pack, &e, &err));
  CHECK(e == SigPackError::BadFormat);

  // Missing build.
  CHECK(!SignaturePack::Parse(
      "{\"format\":1,\"name\":\"p\",\"sequence\":1,"
      "\"signature\":{\"algo\":\"ed25519\",\"value\":\"" + std::string(128, 'a') +
      "\"},\"signatures\":[{\"id\":\"a\",\"pattern\":\"AA\"}]}",
      &pack, &e, &err));
  CHECK(e == SigPackError::MissingBuild);

  // Unknown top-level key.
  CHECK(!SignaturePack::Parse(
      SignedPack("p", "BUILD-A", 1, 0,
                 PackBody("{\"id\":\"a\",\"pattern\":\"AA\"}") + ",\"extra\":1"),
      &pack, &e, &err));
  CHECK(e == SigPackError::BadJson);

  // Unknown key inside a row (typo must fail loudly).
  CHECK(!SignaturePack::Parse(
      SignedPack("p", "BUILD-A", 1, 0,
                 PackBody("{\"id\":\"a\",\"patern\":\"AA\"}")),
      &pack, &e, &err));
  CHECK(e == SigPackError::BadSignature);

  // Duplicate id.
  CHECK(!SignaturePack::Parse(
      SignedPack("p", "BUILD-A", 1, 0,
                 PackBody("{\"id\":\"a\",\"pattern\":\"AA\"},"
                          "{\"id\":\"a\",\"pattern\":\"BB\"}")),
      &pack, &e, &err));
  CHECK(e == SigPackError::DuplicateId);

  // A pattern with no hex bytes is not a real signature.
  CHECK(!SignaturePack::Parse(
      SignedPack("p", "BUILD-A", 1, 0, PackBody("{\"id\":\"a\",\"pattern\":\"   \"}")),
      &pack, &e, &err));
  CHECK(e == SigPackError::BadPattern);

  // Layout targeting a different build than the pack.
  CHECK(!SignaturePack::Parse(
      SignedPack("p", "BUILD-A", 1, 0, PackBody("", LayoutJson("BUILD-B"))),
      &pack, &e, &err));
  CHECK(e == SigPackError::LayoutBuildMismatch);

  // A malformed layout.
  CHECK(!SignaturePack::Parse(
      SignedPack("p", "BUILD-A", 1, 0,
                 PackBody("", "{\"name\":\"X\",\"version\":\"1\",\"build\":\"BUILD-A\","
                              "\"size\":4,\"fields\":[{\"name\":\"f\",\"type\":\"u32\",\"offset\":2}]}")),
      &pack, &e, &err));
  CHECK(e == SigPackError::BadLayout);

  // Empty pack (no signatures, no layouts).
  CHECK(!SignaturePack::Parse(SignedPack("p", "BUILD-A", 1, 0, ""), &pack, &e, &err));
  CHECK(e == SigPackError::NoEntries);
}

//----------------------------------------------------------------------------
// Build-fingerprint gate
//----------------------------------------------------------------------------

static void TestBuildGate() {
  SignaturePack pack;
  SigPackError e = SigPackError::Ok;
  const std::string json = SignedPack(
      "p", "BUILD-A", 1, 0, PackBody("{\"id\":\"gate:rule\",\"pattern\":\"AA BB CC\"}"));
  CHECK(SignaturePack::Parse(json, &pack, &e));

  SigCache& sc = SigCache::Instance();

  // Empty active build: nothing applies (fail closed, never "by default").
  SigPackReport rep;
  CHECK(ApplySignaturePack(pack, "", sc, nullptr, &rep) == SigPackError::BuildMismatch);
  CHECK(rep.signaturesRegistered == 0);

  // A different build: refused.
  CHECK(ApplySignaturePack(pack, "BUILD-B", sc, nullptr, &rep) ==
        SigPackError::BuildMismatch);
  CHECK(rep.signaturesRegistered == 0);

  // Matching build (case- and 0x-insensitive): applies.
  CHECK(ApplySignaturePack(pack, "0xbuild-a", sc, nullptr, &rep) == SigPackError::Ok);
  CHECK(rep.signaturesRegistered == 1);
  CHECK(sc.Has("gate:rule"));
}

//----------------------------------------------------------------------------
// Apply registers into both registries
//----------------------------------------------------------------------------

static void TestApplyRegisters() {
  SignaturePack pack;
  SigPackError e = SigPackError::Ok;
  CHECK(SignaturePack::Parse(
      SignedPack("p", "BUILD-A", 1, 0,
                 PackBody("{\"id\":\"apply:rule\",\"pattern\":\"11 22 33\"}",
                          LayoutJson("BUILD-A"))),
      &pack, &e));

  SigCache& sc = SigCache::Instance();
  LayoutRegistry layouts;
  SigPackReport rep;
  CHECK(ApplySignaturePack(pack, "BUILD-A", sc, &layouts, &rep) == SigPackError::Ok);
  CHECK(rep.signaturesRegistered == 1);
  CHECK(rep.layoutsRegistered == 1);
  CHECK(sc.Has("apply:rule"));
  CHECK(layouts.Count() == 1);

  // The layout resolves only under the active build it was written for.
  layouts.SetActiveBuild("BUILD-A");
  StructLayout out;
  CHECK(layouts.Find("Actor", &out));
  CHECK(out.size == 16);
  layouts.SetActiveBuild("BUILD-Z");
  CHECK(!layouts.Find("Actor", &out));
}

//----------------------------------------------------------------------------
// Directory loading
//----------------------------------------------------------------------------

static void TestDirectoryLoad() {
  MemoryScriptFileSystem fs;
  const std::string good = SignedPack(
      "dir-pack", "BUILD-A", 1, 0, PackBody("{\"id\":\"dir:rule\",\"pattern\":\"44 55 66\"}"));
  const std::string good2 = SignedPack(
      "dir-pack2", "BUILD-A", 2, 0, PackBody("{\"id\":\"dir:rule2\",\"pattern\":\"44 55 66\"}"));
  // A pack with a valid signature but malformed entries -> the entry parser
  // refuses it (proving verification happens first, parsing after).
  const std::string badEntries = SignedPack(
      "dir-bad", "BUILD-A", 3, 0, PackBody("{\"id\":\"x\",\"patern\":\"AA\"}"));
  fs.Write("packs/a.sigpack.json", good);
  fs.Write("packs/b.sigpack.json", good2);
  fs.Write("packs/bad.sigpack.json", badEntries);
  fs.Write("packs/unsigned.sigpack.json",
           "{\"format\":1,\"name\":\"u\",\"build\":\"BUILD-A\","
           "\"signatures\":[{\"id\":\"u\",\"pattern\":\"AA\"}]}");
  fs.Write("packs/notes.json", good);       // wrong suffix -> ignored
  fs.Write("packs/readme.txt", "hello");    // ignored

  SignaturePackLoader loader(&fs, Trust(), "packs");
  SigCache& sc = SigCache::Instance();
  SigPackLoadReport rep = loader.LoadDirectory("BUILD-A", sc, nullptr);

  CHECK(rep.packsTried == 4);        // a, b, bad, unsigned
  CHECK(rep.packsApplied == 2);
  CHECK(rep.packsFailed == 2);
  CHECK(rep.signaturesRegistered == 2);
  CHECK(!rep.truncated);
  CHECK(sc.Has("dir:rule"));
  CHECK(sc.Has("dir:rule2"));
}

//----------------------------------------------------------------------------
// Loader anti-rollback
//----------------------------------------------------------------------------

static void TestLoaderSequenceFloor() {
  MemoryScriptFileSystem fs;
  const std::string newer = SignedPack(
      "p", "BUILD-A", 10, 0, PackBody("{\"id\":\"floor:new\",\"pattern\":\"AA 11\"}"));
  const std::string older = SignedPack(
      "p", "BUILD-A", 4, 0, PackBody("{\"id\":\"floor:old\",\"pattern\":\"AA 22\"}"));
  fs.Write("new.sigpack.json", newer);
  fs.Write("old.sigpack.json", older);

  SignaturePackLoader loader(&fs, Trust());
  SigCache& sc = SigCache::Instance();

  SigPackLoadResult r;
  CHECK(loader.LoadPackFile("new.sigpack.json", "BUILD-A", sc, nullptr, &r) ==
        SigPackError::Ok);
  CHECK(loader.sequenceFloor() == 10);

  // The older pack is refused: its sequence is below the floor.
  CHECK(loader.LoadPackFile("old.sigpack.json", "BUILD-A", sc, nullptr, &r) ==
        SigPackError::StaleSequence);
  CHECK(!sc.Has("floor:old"));
  CHECK(loader.sequenceFloor() == 10);

  // Re-applying the identical pack (equal sequence) is idempotent, not refused.
  CHECK(loader.LoadPackFile("new.sigpack.json", "BUILD-A", sc, nullptr, &r) ==
        SigPackError::Ok);

  // Raising the floor by hand, then the same pack, is now refused.
  loader.SetSequenceFloor(11);
  CHECK(loader.LoadPackFile("new.sigpack.json", "BUILD-A", sc, nullptr, &r) ==
        SigPackError::StaleSequence);

  // The floor never lowers.
  loader.SetSequenceFloor(3);
  CHECK(loader.sequenceFloor() == 11);
}

//----------------------------------------------------------------------------
// Interop with the Python signer
//----------------------------------------------------------------------------

static void TestSignerInterop() {
  // A pack signed by tools/berry-sigpack-sign.py must verify here. This proves
  // the two canonical payloads agree byte for byte.
  const std::string json =
      "{\"format\":1,\"name\":\"interop-pack\",\"build\":\"0xBUILD-INTEROP\","
      "\"sequence\":3,\"expires\":1893456000,"
      "\"signature\":{\"algo\":\"ed25519\",\"value\":\"" + std::string(kInteropSig) +
      "\"},\"signatures\":[{\"id\":\"interop:rule\",\"pattern\":\"DE AD BE EF\"}]}";
  SigPackTrust t;
  t.publicKeyHex = kInteropPub;
  t.nowUnix = 1000;   // well before the pack's expiry
  SignaturePack env;
  SigPackError e = SigPackError::Ok;
  std::string err;
  CHECK(SignaturePack::VerifyEnvelope(json, t, &env, &e, &err));
  CHECK(e == SigPackError::Ok);
  CHECK(env.sequence == 3);

  // And the whole load path applies it under the right build.
  MemoryScriptFileSystem fs;
  fs.Write("i.sigpack.json", json);
  SignaturePackLoader loader(&fs, t);
  SigCache& sc = SigCache::Instance();
  SigPackLoadResult r;
  CHECK(loader.LoadPackFile("i.sigpack.json", "build-interop", sc, nullptr, &r) ==
        SigPackError::Ok);
  CHECK(sc.Has("interop:rule"));
}

int main() {
  TestEnvelopeVerify();
  TestExpiryAndSequenceFloor();
  TestParseValid();
  TestParseRejections();
  TestBuildGate();
  TestApplyRegisters();
  TestDirectoryLoad();
  TestLoaderSequenceFloor();
  TestSignerInterop();
  if (g_fail == 0) printf("test_sigpack: ALL OK\n");
  return g_fail;
}
