// Host tests for the script loader: the one path that decides whether script
// code runs and with what capabilities.
//
// Covers:
//   * Verification: a signed manifest + matching source loads; a swapped
//     source, a widened capability list, a foreign key or a bad algorithm is
//     refused.
//   * The source binding: a signature over a capability list alone is not
//     enough, so an unsigned or unbound source never loads.
//   * The approval gate: a script runs with the intersection of its declared
//     capabilities and the user's grant, never more.
//   * Reload: a re-verification failure keeps the previous revision live.
//   * Interop: the C++ verifier accepts a manifest signed by the Python
//     signer (tools/berry-sign.py), which is the publisher-side half.
#include "berry/loader.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

// RFC 8032 §7.1 vector 1 seed, reused as the test signing key so the vectors
// and the manifest tests share one key.
const char* kSeedHex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
const char* kPubHex =
    "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";

const char* kSource =
    "func on_load\n"
    "  push 1\n"
    "  ret 0\n"
    "end\n";

struct Keys {
  std::string seed, pub, sec, pubHex;
};

Keys MakeKeys() {
  Keys k;
  crypto::FromHex(kSeedHex, &k.seed, 32);
  crypto::Ed25519KeypairFromSeed(k.seed, &k.pub, &k.sec);
  k.pubHex = crypto::ToHexLower(k.pub);
  return k;
}

/// Builds a signed manifest JSON for `source` with the given capabilities.
std::string SignedManifest(const Keys& keys, const std::string& name,
                           const std::string& version,
                           const std::vector<std::string>& caps,
                           const std::string& source) {
  ScriptManifest m;
  m.name = name;
  m.version = version;
  m.entry = "main.luau";
  m.author = "Lynxifyy";
  CHECK(CapabilitySet::ParseAll(caps, &m.capabilities, nullptr));
  m.sourceSha256 = ScriptManifest::SourceHash(source);
  CHECK(m.SignEd25519(keys.sec));
  std::string json;
  json += "{\"name\":\"" + m.name + "\",";
  json += "\"version\":\"" + m.version + "\",";
  json += "\"entry\":\"" + m.entry + "\",";
  json += "\"author\":\"" + m.author + "\",";
  json += "\"capabilities\":[";
  for (size_t i = 0; i < caps.size(); ++i) {
    if (i) json += ",";
    json += "\"" + caps[i] + "\"";
  }
  json += "],";
  json += "\"source_sha256\":\"" + m.sourceSha256 + "\",";
  json += "\"payload_hash\":\"" + m.payloadHash + "\",";
  json += "\"signature_algo\":\"" + m.signatureAlgo + "\",";
  json += "\"signature\":\"" + m.signature + "\"}";
  return json;
}

/// A store that has registered and approved exactly `caps` for `name`.
void ApproveScript(ManifestStore* store, const TrustAnchor& anchor,
                   const Keys& keys, const std::string& name,
                   const std::string& version,
                   const std::vector<std::string>& caps,
                   const std::string& source) {
  ScriptManifest m;
  ManifestError err = ManifestError::Ok;
  const std::string json = SignedManifest(keys, name, version, caps, source);
  CHECK(ScriptManifest::Parse(json, &m, &err, nullptr));
  CHECK(store->Register(m, anchor, &err));
  CHECK(err == ManifestError::Ok);
  CHECK(store->Approve(name, 1000));
}

/// Writes a signed script directory into `fs`.
void WriteScript(MemoryScriptFileSystem* fs, const std::string& dir,
                 const Keys& keys, const std::string& name,
                 const std::string& version,
                 const std::vector<std::string>& caps,
                 const std::string& source) {
  fs->Write(dir + "/manifest.json",
            SignedManifest(keys, name, version, caps, source));
  fs->Write(dir + "/main.luau", source);
}

ScriptLoaderConfig SignedConfig(const Keys& keys) {
  ScriptLoaderConfig cfg;
  cfg.anchor = TrustAnchor::Ed25519Key(keys.pubHex);
  cfg.requireSignature = true;
  cfg.requireSourceHash = true;
  return cfg;
}

//----------------------------------------------------------------------------

void TestLoadsSignedScript() {
  const Keys keys = MakeKeys();
  MemoryScriptFileSystem fs;
  const std::string dir = "scripts/good";
  WriteScript(&fs, dir, keys, "good", "1.0.0", {"render", "hud"}, kSource);

  ScriptLoader loader(SignedConfig(keys), &fs);
  LoadedScript script;
  LoadOutcome outcome;
  CHECK(loader.Load(dir, &script, &outcome, nullptr));
  CHECK(outcome.ok());
  CHECK(script.valid());
  CHECK(script.manifest().name == "good");
  CHECK(script.manifest().sourceSha256 ==
        ScriptManifest::SourceHash(kSource));
  // No approval store means the declared set is the effective set.
  CHECK(script.granted().Count() == 2);
  CHECK(script.host() != nullptr);
  CHECK(script.host()->loaded());
  CHECK(script.host()->name() == "good");
}

void TestRefusesSwappedSource() {
  const Keys keys = MakeKeys();
  MemoryScriptFileSystem fs;
  const std::string dir = "scripts/swapped";
  // Sign `kSource`, then replace the entry file with different code. This is
  // the attack the source binding exists to stop.
  WriteScript(&fs, dir, keys, "swapped", "1.0.0", {"render"}, kSource);
  fs.Write(dir + "/main.luau", "func on_load\n  frobnicate\nend\n");

  ScriptLoader loader(SignedConfig(keys), &fs);
  LoadedScript script;
  LoadOutcome outcome;
  CHECK(!loader.Load(dir, &script, &outcome, nullptr));
  CHECK(outcome.error == LoadError::SourceHashMismatch);
  CHECK(!script.valid());
}

void TestRefusesUnboundSource() {
  const Keys keys = MakeKeys();
  MemoryScriptFileSystem fs;
  const std::string dir = "scripts/unbound";
  // A manifest that signs only a capability list: valid signature, no source
  // binding. It must not load.
  ScriptManifest m;
  m.name = "unbound";
  m.version = "1.0.0";
  m.entry = "main.luau";
  CHECK(CapabilitySet::ParseAll({"render"}, &m.capabilities, nullptr));
  CHECK(m.SignEd25519(keys.sec));
  std::string json = "{\"name\":\"unbound\",\"version\":\"1.0.0\","
                     "\"entry\":\"main.luau\",\"capabilities\":[\"render\"],"
                     "\"payload_hash\":\"" + m.payloadHash + "\","
                     "\"signature_algo\":\"ed25519\","
                     "\"signature\":\"" + m.signature + "\"}";
  fs.Write(dir + "/manifest.json", json);
  fs.Write(dir + "/main.luau", kSource);

  ScriptLoader loader(SignedConfig(keys), &fs);
  LoadedScript script;
  LoadOutcome outcome;
  CHECK(!loader.Load(dir, &script, &outcome, nullptr));
  CHECK(outcome.error == LoadError::SourceHashMissing);
}

void TestRefusesForeignKey() {
  const Keys keys = MakeKeys();
  Keys other;
  other.seed = std::string(32, 'q');
  crypto::Ed25519KeypairFromSeed(other.seed, &other.pub, &other.sec);
  other.pubHex = crypto::ToHexLower(other.pub);

  MemoryScriptFileSystem fs;
  const std::string dir = "scripts/foreign";
  WriteScript(&fs, dir, keys, "foreign", "1.0.0", {"render"}, kSource);

  // The loader trusts `other`, the script was signed by `keys`.
  ScriptLoader loader(SignedConfig(other), &fs);
  LoadedScript script;
  LoadOutcome outcome;
  CHECK(!loader.Load(dir, &script, &outcome, nullptr));
  CHECK(outcome.error == LoadError::BadSignature);
}

void TestRefusesWidenedCapabilities() {
  const Keys keys = MakeKeys();
  MemoryScriptFileSystem fs;
  const std::string dir = "scripts/widened";
  WriteScript(&fs, dir, keys, "widened", "1.0.0", {"render"}, kSource);

  // Rewrite the manifest to ask for more, keeping the old signature: the
  // payload hash no longer matches, so this must be refused.
  std::string json = SignedManifest(keys, "widened", "1.0.0", {"render"}, kSource);
  const std::string from = "\"capabilities\":[\"render\"]";
  const size_t at = json.find(from);
  CHECK(at != std::string::npos);
  json.replace(at, from.size(), "\"capabilities\":[\"render\",\"fileio\"]");
  fs.Write(dir + "/manifest.json", json);

  ScriptLoader loader(SignedConfig(keys), &fs);
  LoadedScript script;
  LoadOutcome outcome;
  CHECK(!loader.Load(dir, &script, &outcome, nullptr));
  CHECK(outcome.error == LoadError::BadSignature);
  CHECK(outcome.manifestError == ManifestError::HashMismatch);
}

void TestRefusesBadAnchor() {
  const Keys keys = MakeKeys();
  MemoryScriptFileSystem fs;
  const std::string dir = "scripts/badanchor";
  WriteScript(&fs, dir, keys, "badanchor", "1.0.0", {"render"}, kSource);

  ScriptLoaderConfig cfg = SignedConfig(keys);
  cfg.anchor = TrustAnchor::Ed25519Key("not-a-real-key");
  ScriptLoader loader(cfg, &fs);
  LoadedScript script;
  LoadOutcome outcome;
  CHECK(!loader.Load(dir, &script, &outcome, nullptr));
  CHECK(outcome.error == LoadError::BadSignature);
}

void TestRefusesMissingAndMalformed() {
  const Keys keys = MakeKeys();
  MemoryScriptFileSystem fs;
  ScriptLoader loader(SignedConfig(keys), &fs);
  LoadedScript script;
  LoadOutcome outcome;

  // No manifest at all.
  CHECK(!loader.Load("scripts/absent", &script, &outcome, nullptr));
  CHECK(outcome.error == LoadError::ManifestUnreadable);

  // Manifest present, entry missing.
  fs.Write("scripts/noentry/manifest.json",
           SignedManifest(keys, "noentry", "1.0.0", {"render"}, kSource));
  CHECK(!loader.Load("scripts/noentry", &script, &outcome, nullptr));
  CHECK(outcome.error == LoadError::SourceUnreadable);

  // Manifest is not JSON.
  fs.Write("scripts/badjson/manifest.json", "{not json");
  fs.Write("scripts/badjson/main.luau", kSource);
  CHECK(!loader.Load("scripts/badjson", &script, &outcome, nullptr));
  CHECK(outcome.error == LoadError::ManifestInvalid);

  // An unsafe entry path is rejected before anything is read.
  ScriptManifest traversal;
  traversal.name = "traversal";
  traversal.version = "1.0.0";
  traversal.entry = "../escape.luau";
  CHECK(CapabilitySet::ParseAll({"render"}, &traversal.capabilities, nullptr));
  traversal.sourceSha256 = ScriptManifest::SourceHash(kSource);
  CHECK(traversal.SignEd25519(keys.sec));
  fs.Write("scripts/traversal/manifest.json",
           "{\"name\":\"traversal\",\"version\":\"1.0.0\","
           "\"entry\":\"../escape.luau\",\"capabilities\":[\"render\"],"
           "\"source_sha256\":\"" + traversal.sourceSha256 + "\","
           "\"payload_hash\":\"" + traversal.payloadHash + "\","
           "\"signature_algo\":\"ed25519\","
           "\"signature\":\"" + traversal.signature + "\"}");
  fs.Write("scripts/traversal/main.luau", kSource);
  CHECK(!loader.Load("scripts/traversal", &script, &outcome, nullptr));
  CHECK(outcome.error == LoadError::ManifestInvalid);
}

void TestApprovalGate() {
  const Keys keys = MakeKeys();
  const TrustAnchor anchor = TrustAnchor::Ed25519Key(keys.pubHex);
  MemoryScriptFileSystem fs;
  const std::string dir = "scripts/gated";
  WriteScript(&fs, dir, keys, "gated", "1.0.0", {"render", "hud", "input"},
              kSource);
  ManifestStore& store = ManifestStore::Instance();
  store.Clear();
  ScriptLoader loader(SignedConfig(keys), &fs);
  LoadedScript script;
  LoadOutcome outcome;

  // Registered but not approved: the loader refuses and grants nothing.
  ScriptManifest declared;
  CHECK(ScriptManifest::Parse(
      SignedManifest(keys, "gated", "1.0.0", {"render"}, kSource), &declared,
      nullptr, nullptr));
  ManifestError regErr = ManifestError::Ok;
  CHECK(store.Register(declared, anchor, &regErr));
  CHECK(regErr == ManifestError::Ok);
  CHECK(!store.IsApproved("gated"));
  CHECK(!loader.Load(dir, &script, &outcome, &store));
  CHECK(outcome.error == LoadError::NotApproved);
  CHECK(!script.valid());

  // Approved for `render` only, while the script declares render + hud +
  // input. The effective set is the intersection, so an approval can never
  // widen access beyond what the user reviewed.
  CHECK(store.Approve("gated", 1000));
  CHECK(loader.Load(dir, &script, &outcome, &store));
  CHECK(outcome.ok());
  CHECK(script.granted().Count() == 1);
  CHECK(script.granted().Has(CapKind::Render));
  CHECK(!script.granted().Has(CapKind::Hud));
  CHECK(!script.granted().Has(CapKind::Input));

  // A version bump starts unapproved again: new code cannot ride the old
  // approval, even with an identical capability list.
  store.Clear();
  WriteScript(&fs, dir, keys, "gated", "2.0.0", {"render", "hud", "input"},
              kSource);
  ScriptManifest v2;
  CHECK(ScriptManifest::Parse(
      SignedManifest(keys, "gated", "2.0.0", {"render"}, kSource), &v2,
      nullptr, nullptr));
  CHECK(store.Register(v2, anchor, &regErr));
  CHECK(!store.IsApproved("gated"));
  CHECK(!loader.Load(dir, &script, &outcome, &store));
  CHECK(outcome.error == LoadError::NotApproved);

  // Re-approving the new version lets it through.
  CHECK(store.Approve("gated", 2000));
  CHECK(loader.Load(dir, &script, &outcome, &store));
  CHECK(script.granted().Count() == 1);
  CHECK(script.granted().Has(CapKind::Render));

  store.Clear();
}

void TestEffectiveCapabilitiesIntersection() {
  const Keys keys = MakeKeys();
  const TrustAnchor anchor = TrustAnchor::Ed25519Key(keys.pubHex);
  ManifestStore& store = ManifestStore::Instance();
  store.Clear();

  // The registered/approved manifest declares `render`; the file declares
  // render + a network host. The intersection is what runs.
  ApproveScript(&store, anchor, keys, "mixed", "1.0.0", {"render"}, kSource);
  ScriptManifest fileManifest;
  CHECK(ScriptManifest::Parse(
      SignedManifest(keys, "mixed", "1.0.0",
                     {"render", "network:api.example.com"}, kSource),
      &fileManifest, nullptr, nullptr));

  CapabilitySet eff = ScriptLoader::EffectiveCapabilities(fileManifest, &store);
  CHECK(eff.Count() == 1);
  CHECK(eff.Has(CapKind::Render));
  CHECK(!eff.Has(CapKind::Network));

  // A network grant is host-scoped, so a grant for one host covers only that
  // host and never another.
  store.Clear();
  ApproveScript(&store, anchor, keys, "scoped", "1.0.0",
                {"network:api.example.com"}, kSource);
  ScriptManifest sameHost;
  CHECK(ScriptManifest::Parse(
      SignedManifest(keys, "scoped", "1.0.0", {"network:api.example.com"},
                     kSource), &sameHost, nullptr, nullptr));
  CHECK(ScriptLoader::EffectiveCapabilities(sameHost, &store).Count() == 1);
  ScriptManifest otherHost;
  CHECK(ScriptManifest::Parse(
      SignedManifest(keys, "scoped", "1.0.0", {"network:evil.example.com"},
                     kSource), &otherHost, nullptr, nullptr));
  CHECK(ScriptLoader::EffectiveCapabilities(otherHost, &store).empty());

  // An unknown script has no grant, so no capabilities.
  ScriptManifest ghost;
  CHECK(ScriptManifest::Parse(SignedManifest(keys, "ghost", "1.0.0", {"render"},
                                             kSource),
                              &ghost, nullptr, nullptr));
  CHECK(ScriptLoader::EffectiveCapabilities(ghost, &store).empty());

  // No store means no approval step: the declared set stands.
  CHECK(ScriptLoader::EffectiveCapabilities(fileManifest, nullptr).Count() == 2);

  store.Clear();
}

void TestReloadReverifies() {
  const Keys keys = MakeKeys();
  MemoryScriptFileSystem fs;
  const std::string dir = "scripts/reload";
  WriteScript(&fs, dir, keys, "reload", "1.0.0", {"render"}, kSource);

  ScriptLoader loader(SignedConfig(keys), &fs);
  LoadedScript script;
  LoadOutcome outcome;
  CHECK(loader.Load(dir, &script, &outcome, nullptr));
  CHECK(script.host()->loaded());

  // An unchanged revision is a no-op, not a reload.
  ReloadResult r = script.Reload(false);
  CHECK(!r.attempted || r.unchanged);

  // A verified edit reloads.
  const std::string v2 =
      "func on_load\n  push 2\n  ret 0\nend\n";
  WriteScript(&fs, dir, keys, "reload", "1.0.0", {"render"}, v2);
  r = script.Reload(false);
  CHECK(r.attempted);
  CHECK(r.reloaded);
  CHECK(script.lastVerifyError() == LoadError::Ok);

  // An edit that breaks verification (source no longer matches the signed
  // hash) must be refused, and the verified revision must stay live.
  fs.Write(dir + "/main.luau", "func on_load\n  evil\nend\n");
  r = script.Reload(false);
  CHECK(r.compileFailed || r.unchanged);
  CHECK(script.lastVerifyError() == LoadError::SourceHashMismatch);
  CHECK(script.lastVerifyMessage().find("source_sha256") != std::string::npos);
  // The live host still runs the verified revision.
  CHECK(script.host() != nullptr);
  CHECK(script.host()->loaded());

  // A swap back to a verifying revision reloads again.
  WriteScript(&fs, dir, keys, "reload", "1.0.0", {"render"}, kSource);
  r = script.Reload(true);
  CHECK(r.attempted);
  CHECK(r.reloaded);
}

void TestDevModeSkipsSignature() {
  const Keys keys = MakeKeys();
  MemoryScriptFileSystem fs;
  const std::string dir = "scripts/dev";
  // An unsigned manifest with no source binding.
  fs.Write(dir + "/manifest.json",
           "{\"name\":\"dev\",\"version\":\"1.0.0\",\"entry\":\"main.luau\","
           "\"capabilities\":[\"render\"]}");
  fs.Write(dir + "/main.luau", kSource);

  ScriptLoaderConfig cfg = ScriptLoaderConfig::DevMode();
  CHECK(!cfg.requireSignature);
  CHECK(!cfg.requireSourceHash);
  ScriptLoader loader(cfg, &fs);
  LoadedScript script;
  LoadOutcome outcome;
  CHECK(loader.Load(dir, &script, &outcome, nullptr));
  CHECK(script.granted().Has(CapKind::Render));

  // ...but a signed loader still refuses the same directory. An unsigned
  // manifest has no source binding either, and that is what is reported.
  ScriptLoader strict(SignedConfig(keys), &fs);
  LoadedScript refused;
  LoadOutcome strictOutcome;
  CHECK(!strict.Load(dir, &refused, &strictOutcome, nullptr));
  CHECK(strictOutcome.error == LoadError::SourceHashMissing);

  // An unsigned manifest that does bind a source hash is refused as
  // unsigned, so the two failures stay distinguishable.
  const std::string dir2 = "scripts/dev2";
  fs.Write(dir2 + "/manifest.json",
           "{\"name\":\"dev2\",\"version\":\"1.0.0\",\"entry\":\"main.luau\","
           "\"capabilities\":[\"render\"],\"source_sha256\":\"" +
               ScriptManifest::SourceHash(kSource) + "\"}");
  fs.Write(dir2 + "/main.luau", kSource);
  CHECK(!strict.Load(dir2, &refused, &strictOutcome, nullptr));
  CHECK(strictOutcome.error == LoadError::BadSignature);
  CHECK(strictOutcome.manifestError == ManifestError::Unsigned);
}

void TestVerifyOnly() {
  const Keys keys = MakeKeys();
  MemoryScriptFileSystem fs;
  ScriptLoader loader(SignedConfig(keys), &fs);

  ScriptManifest out;
  LoadOutcome outcome;
  CHECK(loader.VerifyOnly(SignedManifest(keys, "v", "1.0.0", {"render"}, kSource),
                          kSource, &out, &outcome, nullptr));
  CHECK(outcome.ok());
  CHECK(out.name == "v");

  // Source mismatch is reported, not silently accepted.
  CHECK(!loader.VerifyOnly(SignedManifest(keys, "v", "1.0.0", {"render"}, kSource),
                           "different source", &out, &outcome, nullptr));
  CHECK(outcome.error == LoadError::SourceHashMismatch);
}

//----------------------------------------------------------------------------
// Interop: a manifest produced by tools/berry-sign.py must verify here.
//
// The Python signer is what a publisher actually runs, so a divergence between
// its canonical payload and ScriptManifest::CanonicalPayload would reject every
// real script. The vector below is the exact output of
//   python3 tools/berry-sign.py sign --key <rfc8032 seed> --dir <dir>
// for the source and capability list inlined here. tools/check-signer.sh
// regenerates it and fails the build when the two sides drift.
//----------------------------------------------------------------------------

void TestPythonSignerInterop() {
  const char* manifestJson =
      "{\"name\":\"interop\",\"version\":\"1.0.0\",\"entry\":\"main.luau\","
      "\"author\":\"Lynxifyy\","
      "\"capabilities\":[\"render\",\"hud\",\"input\","
      "\"network:api.example.com\"],"
      "\"source_sha256\":\"c4d89fbd7e19881c9c4c82e4708269d4ee88f98e0d30b561"
      "15d99c0dca335e92\","
      "\"payload_hash\":\"1ea5d676989c072c88274a91fd3ef51785a4180389ce4f57b6"
      "05d1d6634aad5a\","
      "\"signature_algo\":\"ed25519\","
      "\"signature\":\"5b984043108c5e5ff2d0ee68bc112e75dc8aedffb369793ac20bcb"
      "9d596ebb3cf53c367800586cfc5b216dbc67011ec990be52d62446ea11d833215b"
      "4f68b501\"}";

  ScriptLoaderConfig cfg;
  cfg.anchor = TrustAnchor::Ed25519Key(kPubHex);
  cfg.requireSignature = true;
  cfg.requireSourceHash = true;
  MemoryScriptFileSystem fs;
  ScriptLoader loader(cfg, &fs);

  ScriptManifest out;
  LoadOutcome outcome;
  CHECK(loader.VerifyOnly(manifestJson, kSource, &out, &outcome, nullptr));
  CHECK(outcome.ok());
  CHECK(out.name == "interop");
  CHECK(out.capabilities.Count() == 4);
  CHECK(out.capabilities.Has(CapKind::Network));

  // A single changed byte of source fails the binding.
  CHECK(!loader.VerifyOnly(manifestJson, std::string(kSource) + " ", &out,
                           &outcome, nullptr));
  CHECK(outcome.error == LoadError::SourceHashMismatch);
}

}  // namespace

int main() {
  TestLoadsSignedScript();
  TestRefusesSwappedSource();
  TestRefusesUnboundSource();
  TestRefusesForeignKey();
  TestRefusesWidenedCapabilities();
  TestRefusesBadAnchor();
  TestRefusesMissingAndMalformed();
  TestApprovalGate();
  TestEffectiveCapabilitiesIntersection();
  TestReloadReverifies();
  TestDevModeSkipsSignature();
  TestVerifyOnly();
  TestPythonSignerInterop();
  if (g_fail == 0) printf("test_loader: ALL OK\n");
  return g_fail;
}
