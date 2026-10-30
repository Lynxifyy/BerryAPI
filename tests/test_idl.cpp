// Host tests for the generated script API surface (Phase 5).
//
// The IDL (api/berry.idl) is the single source of truth for binding names,
// their capabilities and their arities. tools/idlgen.py turns it into
// include/berry/generated_bindings.h; these tests pin the properties the
// runtime and the docs rely on:
//   * every generated binding resolves by name,
//   * arity and capability come from the IDL, not from hand-written code,
//   * the per-module tables agree with the flat spec table,
//   * the generated API stays fail-closed (no binding is CapKind::Unknown),
//   * the async layer's bindings are reachable through the same lookup.
#include "berry/async.h"
#include "berry/generated_bindings.h"
#include "berry/manifest.h"
#include "berry/package.h"   // SemVer
#include "berry/script.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static void TestSpecTableWellFormed() {
  const api::BindingSpec* specs = api::BindingSpecs();
  CHECK(api::BindingSpecCount() > 0);
  std::set<std::string> names;
  for (size_t i = 0; i < api::BindingSpecCount(); ++i) {
    const api::BindingSpec& s = specs[i];
    CHECK(s.name && *s.name);
    CHECK(s.module && *s.module);
    CHECK(s.arity >= 0);
    // Fail closed: every generated binding must name a real capability, so the
    // runtime gate can never mistake it for "no capability required".
    CHECK(IsGrantableKind(s.capability));
    CHECK(names.insert(s.name).second);   // no duplicates
    // Names are "<module>.<fn>", lower-case module.
    const std::string full(s.name);
    const size_t dot = full.find('.');
    CHECK(dot != std::string::npos);
    CHECK(dot > 0 && dot + 1 < full.size());
  }
}

static void TestLookupByName() {
  CHECK(api::FindBindingSpec("combat.recent_kills") != nullptr);
  CHECK(api::FindBindingSpec("render.draw_text") != nullptr);
  CHECK(api::FindBindingSpec("hud.tween") != nullptr);
  CHECK(api::FindBindingSpec("perf.set_thermal_mode") != nullptr);
  CHECK(api::FindBindingSpec("does.not_exist") == nullptr);
  CHECK(api::FindBindingSpec("") == nullptr);

  // Arity and capability are the IDL's, not an approximation.
  const api::BindingSpec* dt = api::FindBindingSpec("render.draw_text");
  CHECK(dt->arity == 4);
  CHECK(dt->capability == CapKind::Render);
  CHECK(std::string(dt->module) == "Render");

  const api::BindingSpec* rk = api::FindBindingSpec("combat.recent_kills");
  CHECK(rk->arity == 0);
  CHECK(rk->capability == CapKind::Combat);

  const api::BindingSpec* ng = api::FindBindingSpec("network.get");
  CHECK(ng->arity == 2);
  CHECK(ng->capability == CapKind::Network);
}

static void TestModuleTablesAgree() {
  const api::ModuleBindings* mods = api::ModuleBindingTable();
  CHECK(api::ModuleBindingCount() > 0);
  size_t total = 0;
  for (size_t m = 0; m < api::ModuleBindingCount(); ++m) {
    const api::ModuleBindings& mod = mods[m];
    CHECK(mod.module && *mod.module);
    CHECK(mod.names != nullptr);
    CHECK(mod.count > 0);
    total += mod.count;
    for (size_t i = 0; i < mod.count; ++i) {
      // Every name in a module table must exist in the flat spec table and
      // belong to the same module.
      const api::BindingSpec* spec = api::FindBindingSpec(mod.names[i]);
      CHECK(spec != nullptr);
      if (spec) CHECK(std::string(spec->module) == mod.module);
    }
  }
  // The tables partition the spec table exactly.
  CHECK(total == api::BindingSpecCount());
}

static void TestCapabilityNamesResolve() {
  // Every capability used by the generated API must round-trip through the
  // manifest taxonomy, so a script manifest can request it by name.
  const api::BindingSpec* specs = api::BindingSpecs();
  for (size_t i = 0; i < api::BindingSpecCount(); ++i) {
    const char* name = CapKindName(specs[i].capability);
    CHECK(name != nullptr && std::string(name) != "unknown");
    CHECK(CapKindFromName(name) == specs[i].capability);
  }
}

static void TestAsyncBindingsAreSeparateSurface() {
  // The async layer registers its own bindings (Phase 4); they are not part of
  // the generated static table, and this test pins that boundary so the two
  // cannot silently collide.
  CHECK(api::FindBindingSpec(AsyncBindings::Wait()) == nullptr);
  CHECK(api::FindBindingSpec(AsyncBindings::HttpGet()) == nullptr);
  CHECK(std::string(AsyncBindings::Wait()) == "async.wait");
  CHECK(std::string(AsyncBindings::HttpGet()) == "async.http_get");
}

static void TestScriptApiVersion() {
  // The IDL's api_version is surfaced through the generated header and must be
  // a parseable SemVer, because a manifest's requirement is compared against it
  // with the SemVer total order.
  const char* version = api::ScriptApiVersion();
  CHECK(version != nullptr && *version);
  SemVer v;
  CHECK(SemVer::Parse(version, &v));
  CHECK(v.prerelease.empty());   // a released build, not a pre-release
}

static void TestDeprecationTable() {
  // Deprecation is declared in the IDL, so the table is the contract: a name is
  // either live (null advice) or deprecated with non-empty migration advice.
  const api::BindingSpec* specs = api::BindingSpecs();
  size_t deprecated = 0;
  for (size_t i = 0; i < api::BindingSpecCount(); ++i) {
    const api::BindingSpec& s = specs[i];
    if (s.deprecated == nullptr) {
      CHECK(!api::IsBindingDeprecated(s.name));
      continue;
    }
    ++deprecated;
    CHECK(*s.deprecated != '\0');   // advice is mandatory
    CHECK(api::IsBindingDeprecated(s.name));
    // A deprecated binding is still a real, gated binding: deprecation must
    // never be a way to slip past the capability gate.
    CHECK(IsGrantableKind(s.capability));
  }
  // The surface pins at least the one alias the IDL marks deprecated, so a
  // regression that silently drops the marker is caught.
  CHECK(deprecated >= 1);
  CHECK(api::IsBindingDeprecated("world.block_id_at"));
  CHECK(api::FindBindingSpec("world.block_id_at") != nullptr);
  CHECK(api::FindBindingSpec("world.block_id_at")->deprecated != nullptr);

  // A live binding is not deprecated, and a name that does not exist is not
  // "deprecated" either (it simply is not part of the surface).
  CHECK(!api::IsBindingDeprecated("world.block_at"));
  CHECK(!api::IsBindingDeprecated("does.not.exist"));
  CHECK(!api::IsBindingDeprecated(""));
}

int main() {
  TestSpecTableWellFormed();
  TestLookupByName();
  TestModuleTablesAgree();
  TestCapabilityNamesResolve();
  TestAsyncBindingsAreSeparateSurface();
  TestScriptApiVersion();
  TestDeprecationTable();
  if (g_fail == 0) printf("test_idl: ALL OK\n");
  return g_fail;
}
