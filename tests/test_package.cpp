// Host tests for the script package manager: semver, constraints, manifest
// parsing, the resolver and require.
//
// Covers:
//   * Semver parse/compare: field ordering, numeric (not lexical) comparison,
//     pre-release ordering, build metadata, and the rejection of malformed
//     versions.
//   * Constraints: exact, comparators, caret, tilde, AND, OR, and the
//     intersection used by the resolver.
//   * Manifest parsing: required fields, unsafe entry paths, unknown keys,
//     duplicate dependencies and malformed constraints.
//   * Resolution: dependency ordering, the highest satisfying version, and
//     every fail-closed path (missing package, no satisfying version, cycle).
//   * `require` path resolution.
//   * The package.* script bindings, including the fail-closed case with no
//     registry bound.
#include "berry/api_bindings.h"
#include "berry/generated_bindings.h"
#include "berry/package.h"
#include "berry/script.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

CapabilitySet Grant(const std::vector<std::string>& caps) {
  CapabilitySet s;
  for (const auto& c : caps) s.Add(c);
  return s;
}

ScriptStatus CallBinding(ScriptBindings& bindings, const std::string& name,
                         const std::vector<ScriptValue>& args,
                         const std::vector<std::string>& grants,
                         ScriptValue* out) {
  ScriptHost host("package-test", Grant(grants), nullptr);
  host.SetBindings(&bindings);
  return host.CallBinding(name, args, out);
}

SemVer V(const std::string& s) {
  SemVer v;
  CHECK(SemVer::Parse(s, &v));
  return v;
}

PackageManifest Manifest(const std::string& json) {
  PackageManifest m;
  PackageError pe = PackageError::Ok;
  std::string err;
  CHECK(PackageManifest::Parse(json, &m, &pe, &err));
  return m;
}

/// A manifest document with the given dependencies object body.
std::string PkgJson(const std::string& name, const std::string& version,
                    const std::string& deps = "") {
  std::string s = "{\"name\":\"" + name + "\",\"version\":\"" + version +
                  "\",\"entry\":\"main.luau\"";
  if (!deps.empty()) s += ",\"dependencies\":" + deps;
  s += "}";
  return s;
}

}  // namespace

//----------------------------------------------------------------------------
// Semver
//----------------------------------------------------------------------------

static void TestSemverParse() {
  SemVer v;
  CHECK(SemVer::Parse("1.2.3", &v));
  CHECK(v.major == 1 && v.minor == 2 && v.patch == 3 && v.prerelease.empty());
  CHECK(SemVer::Parse("v1.2.3", &v));
  CHECK(v.ToString() == "1.2.3");
  CHECK(SemVer::Parse("  1.2.3  ", &v));
  CHECK(v.ToString() == "1.2.3");
  CHECK(SemVer::Parse("1.2.3-rc.1", &v));
  CHECK(v.prerelease == "rc.1");
  CHECK(v.ToString() == "1.2.3-rc.1");
  // Build metadata is dropped from the ordering.
  CHECK(SemVer::Parse("1.2.3+build.7", &v));
  CHECK(v.ToString() == "1.2.3");
  CHECK(SemVer::Parse("1.2.3-rc.1+sha.abc", &v));
  CHECK(v.prerelease == "rc.1");

  // Rejections.
  CHECK(!SemVer::Parse("", &v));
  CHECK(!SemVer::Parse("1.2", &v));
  CHECK(!SemVer::Parse("1.2.3.4", &v));
  CHECK(!SemVer::Parse("1.x.3", &v));
  CHECK(!SemVer::Parse("1.2.3-", &v));       // empty pre-release
  CHECK(!SemVer::Parse("1.2.3-rc..1", &v));  // empty identifier
  CHECK(!SemVer::Parse("4294967296.0.0", &v));  // overflows u32
}

static void TestSemverCompare() {
  // Numeric, not lexical: 1.10.0 > 1.9.0.
  CHECK(V("1.10.0") > V("1.9.0"));
  CHECK(V("1.2.10") > V("1.2.9"));
  CHECK(V("2.0.0") > V("1.99.99"));
  CHECK(V("1.2.3") == V("1.2.3"));

  // A pre-release sorts before its release.
  CHECK(V("1.0.0-rc.1") < V("1.0.0"));
  CHECK(V("1.0.0-alpha") < V("1.0.0-beta"));
  CHECK(V("1.0.0-alpha.1") < V("1.0.0-alpha.2"));
  // Numeric identifiers sort before alphanumeric ones.
  CHECK(V("1.0.0-1") < V("1.0.0-alpha"));
  // A shorter pre-release with an equal prefix sorts first.
  CHECK(V("1.0.0-alpha") < V("1.0.0-alpha.1"));
}

//----------------------------------------------------------------------------
// Constraints
//----------------------------------------------------------------------------

static void TestConstraints() {
  VersionConstraint c;
  std::string err;

  CHECK(VersionConstraint::Parse("*", &c, &err) && c.IsAny());
  CHECK(VersionConstraint::Parse("", &c, &err) && c.IsAny());

  CHECK(VersionConstraint::Parse("1.2.3", &c, &err));
  CHECK(c.Satisfies(V("1.2.3")));
  CHECK(!c.Satisfies(V("1.2.4")));

  CHECK(VersionConstraint::Parse(">=1.2.0", &c, &err));
  CHECK(c.Satisfies(V("1.2.0")) && c.Satisfies(V("2.0.0")));
  CHECK(!c.Satisfies(V("1.1.9")));

  // Caret: >=1.2.0 <2.0.0
  CHECK(VersionConstraint::Parse("^1.2.0", &c, &err));
  CHECK(c.Satisfies(V("1.2.0")) && c.Satisfies(V("1.9.9")));
  CHECK(!c.Satisfies(V("2.0.0")));
  CHECK(!c.Satisfies(V("1.1.9")));
  // ^0.2.3 -> >=0.2.3 <0.3.0
  CHECK(VersionConstraint::Parse("^0.2.3", &c, &err));
  CHECK(c.Satisfies(V("0.2.9")) && !c.Satisfies(V("0.3.0")));

  // Tilde: >=1.2.0 <1.3.0
  CHECK(VersionConstraint::Parse("~1.2.0", &c, &err));
  CHECK(c.Satisfies(V("1.2.9")) && !c.Satisfies(V("1.3.0")));

  // AND
  CHECK(VersionConstraint::Parse(">=1.0.0,<2.0.0", &c, &err));
  CHECK(c.Satisfies(V("1.5.0")) && !c.Satisfies(V("2.0.0")));

  // OR
  CHECK(VersionConstraint::Parse("1.0.0 || 2.0.0", &c, &err));
  CHECK(c.Satisfies(V("1.0.0")) && c.Satisfies(V("2.0.0")));
  CHECK(!c.Satisfies(V("1.5.0")));

  // Bad comparators.
  CHECK(!VersionConstraint::Parse(">=", &c, &err));
  CHECK(!VersionConstraint::Parse("^", &c, &err));
  CHECK(!VersionConstraint::Parse("1.2", &c, &err));

  // Intersection of caret and a specific range.
  VersionConstraint a, b;
  CHECK(VersionConstraint::Parse("^1.0.0", &a, &err));
  CHECK(VersionConstraint::Parse("<1.5.0", &b, &err));
  const VersionConstraint i = Intersect(a, b);
  CHECK(i.Satisfies(V("1.2.0")));
  CHECK(!i.Satisfies(V("1.5.0")));
  CHECK(!i.Satisfies(V("0.9.0")));

  // Intersecting with Any is identity; with None is None.
  CHECK(Intersect(a, VersionConstraint::Any()).Satisfies(V("1.2.0")));
  CHECK(Intersect(a, VersionConstraint::None()).IsNone());
}

//----------------------------------------------------------------------------
// Manifests
//----------------------------------------------------------------------------

static void TestManifestParse() {
  PackageManifest m;
  PackageError pe = PackageError::Ok;
  std::string err;

  CHECK(PackageManifest::Parse(PkgJson("alpha", "1.0.0"), &m, &pe, &err));
  CHECK(m.name == "alpha" && m.version == "1.0.0" && m.entry == "main.luau");
  CHECK(m.parsedVersion == V("1.0.0"));
  CHECK(m.dependencies.empty());

  // Missing fields.
  CHECK(!PackageManifest::Parse("{\"version\":\"1.0.0\",\"entry\":\"m.luau\"}",
                                &m, &pe, &err));
  CHECK(pe == PackageError::MissingName);
  CHECK(!PackageManifest::Parse("{\"name\":\"a\",\"entry\":\"m.luau\"}",
                                &m, &pe, &err));
  CHECK(pe == PackageError::MissingVersion);
  CHECK(!PackageManifest::Parse("{\"name\":\"a\",\"version\":\"1.0.0\"}",
                                &m, &pe, &err));
  CHECK(pe == PackageError::MissingEntry);

  // Bad name / version.
  CHECK(!PackageManifest::Parse(PkgJson("-bad", "1.0.0"), &m, &pe, &err));
  CHECK(pe == PackageError::BadName);
  CHECK(!PackageManifest::Parse(PkgJson("ok", "1.0"), &m, &pe, &err));
  CHECK(pe == PackageError::BadVersion);

  // Unsafe entry paths.
  CHECK(!PackageManifest::Parse(
      "{\"name\":\"a\",\"version\":\"1.0.0\",\"entry\":\"../evil.luau\"}",
      &m, &pe, &err));
  CHECK(pe == PackageError::BadEntry);
  CHECK(!PackageManifest::Parse(
      "{\"name\":\"a\",\"version\":\"1.0.0\",\"entry\":\"/etc/passwd\"}",
      &m, &pe, &err));
  CHECK(pe == PackageError::BadEntry);

  // Unknown key is refused.
  CHECK(!PackageManifest::Parse(
      "{\"name\":\"a\",\"version\":\"1.0.0\",\"entry\":\"m.luau\",\"nope\":1}",
      &m, &pe, &err));
  CHECK(pe == PackageError::BadJson);

  // Dependencies: object of name -> constraint.
  CHECK(PackageManifest::Parse(PkgJson("a", "1.0.0", "{\"b\":\"^1.0.0\"}"),
                               &m, &pe, &err));
  CHECK(m.dependencies.size() == 1);
  CHECK(m.dependencies[0].name == "b");
  CHECK(m.dependencies[0].constraint.Satisfies(V("1.5.0")));

  // A malformed constraint is refused.
  CHECK(!PackageManifest::Parse(PkgJson("a", "1.0.0", "{\"b\":\">=\"}"),
                                &m, &pe, &err));
  CHECK(pe == PackageError::BadConstraint);

  // A non-string constraint is refused.
  CHECK(!PackageManifest::Parse(PkgJson("a", "1.0.0", "{\"b\":1}"),
                                &m, &pe, &err));
  CHECK(pe == PackageError::BadDependencies);

  // Dependencies must be an object.
  CHECK(!PackageManifest::Parse(PkgJson("a", "1.0.0", "[]"), &m, &pe, &err));
  CHECK(pe == PackageError::BadDependencies);
}

//----------------------------------------------------------------------------
// Registry + resolution
//----------------------------------------------------------------------------

static void TestRegistry() {
  PackageRegistry reg;
  CHECK(reg.Count() == 0);

  CHECK(reg.Add(Manifest(PkgJson("a", "1.0.0"))));
  CHECK(reg.Add(Manifest(PkgJson("a", "1.2.0"))));
  CHECK(reg.Add(Manifest(PkgJson("b", "2.0.0"))));
  CHECK(reg.Count() == 3);
  CHECK(reg.Has("a") && reg.Has("b") && !reg.Has("c"));
  CHECK(reg.HasVersion("a", "1.0.0") && !reg.HasVersion("a", "9.9.9"));

  // Versions come back ascending.
  const std::vector<SemVer> vs = reg.Versions("a");
  CHECK(vs.size() == 2);
  CHECK(vs[0] == V("1.0.0") && vs[1] == V("1.2.0"));

  // Duplicate name@version is refused.
  std::string err;
  CHECK(!reg.Add(Manifest(PkgJson("a", "1.0.0")), &err));
  CHECK(!err.empty());

  // Names are sorted and unique.
  const std::vector<std::string> names = reg.Names();
  CHECK(names.size() == 2 && names[0] == "a" && names[1] == "b");

  CHECK(reg.Find("a", "1.2.0") != nullptr);
  CHECK(reg.Find("a", "3.0.0") == nullptr);
}

static void TestResolveSimple() {
  PackageRegistry reg;
  CHECK(reg.Add(Manifest(PkgJson("app", "1.0.0", "{\"lib\":\"^1.0.0\"}"))));
  CHECK(reg.Add(Manifest(PkgJson("lib", "1.0.0"))));
  CHECK(reg.Add(Manifest(PkgJson("lib", "1.5.0"))));
  CHECK(reg.Add(Manifest(PkgJson("lib", "2.0.0"))));

  PackageDependency root;
  root.name = "app";
  root.constraint = VersionConstraint::Any();
  const ResolveResult r = reg.Resolve({root});
  CHECK(r.ok());
  // Highest satisfying version of lib is 1.5.0 (2.0.0 is out of caret range).
  CHECK(r.versions.size() == 2);
  CHECK(r.versions[0] == "app@1.0.0");
  CHECK(r.versions[1] == "lib@1.5.0");
  // Install order: the dependency comes first.
  CHECK(r.order.size() == 2);
  CHECK(r.order[0].name == "lib");
  CHECK(r.order[1].name == "app");
  CHECK(r.order[0].entry == "main.luau");
}

static void TestResolveFailsClosed() {
  // Missing package.
  {
    PackageRegistry reg;
    CHECK(reg.Add(Manifest(PkgJson("app", "1.0.0", "{\"ghost\":\"^1.0.0\"}"))));
    PackageDependency root;
    root.name = "app";
    root.constraint = VersionConstraint::Any();
    const ResolveResult r = reg.Resolve({root});
    CHECK(!r.ok());
    CHECK(r.error == ResolveError::MissingPackage);
    CHECK(r.order.empty());
  }
  // No satisfying version.
  {
    PackageRegistry reg;
    CHECK(reg.Add(Manifest(PkgJson("app", "1.0.0", "{\"lib\":\"^3.0.0\"}"))));
    CHECK(reg.Add(Manifest(PkgJson("lib", "1.0.0"))));
    PackageDependency root;
    root.name = "app";
    root.constraint = VersionConstraint::Any();
    const ResolveResult r = reg.Resolve({root});
    CHECK(!r.ok());
    CHECK(r.error == ResolveError::NoVersion);
  }
  // A root that names a package the registry does not hold.
  {
    PackageRegistry reg;
    PackageDependency root;
    root.name = "nope";
    root.constraint = VersionConstraint::Any();
    const ResolveResult r = reg.Resolve({root});
    CHECK(!r.ok());
    CHECK(r.error == ResolveError::MissingPackage);
  }
  // A cycle is reported, not looped over.
  {
    PackageRegistry reg;
    CHECK(reg.Add(Manifest(PkgJson("a", "1.0.0", "{\"b\":\"*\"}"))));
    CHECK(reg.Add(Manifest(PkgJson("b", "1.0.0", "{\"a\":\"*\"}"))));
    PackageDependency root;
    root.name = "a";
    root.constraint = VersionConstraint::Any();
    const ResolveResult r = reg.Resolve({root});
    CHECK(!r.ok());
    CHECK(r.error == ResolveError::Cycle);
  }
}

static void TestResolvePicksHighestSatisfying() {
  PackageRegistry reg;
  CHECK(reg.Add(Manifest(PkgJson("lib", "1.0.0"))));
  CHECK(reg.Add(Manifest(PkgJson("lib", "1.2.0"))));
  CHECK(reg.Add(Manifest(PkgJson("lib", "1.3.0"))));

  PackageDependency root;
  root.name = "lib";
  CHECK(VersionConstraint::Parse("~1.2.0", &root.constraint));
  const ResolveResult r = reg.Resolve({root});
  CHECK(r.ok());
  CHECK(r.order.size() == 1);
  CHECK(r.order[0].version == "1.2.0");
}

//----------------------------------------------------------------------------
// require
//----------------------------------------------------------------------------

static void TestRequire() {
  PackageRegistry reg;
  CHECK(reg.Add(Manifest(PkgJson("lib", "1.0.0"))));
  CHECK(reg.Add(Manifest(PkgJson("lib", "2.0.0"))));

  std::string name, entry, err;
  // A bare name resolves to the highest version's entry.
  CHECK(ResolveRequire(reg, "lib", &name, &entry, &err));
  CHECK(name == "lib");
  CHECK(entry == "main.luau");

  // A sub-path names a file inside the package.
  CHECK(ResolveRequire(reg, "lib/util.luau", &name, &entry, &err));
  CHECK(name == "lib" && entry == "util.luau");
  CHECK(ResolveRequire(reg, "lib/sub/deep.luau", &name, &entry, &err));
  CHECK(entry == "sub/deep.luau");

  // Fail closed: unknown package, escaping path, empty path.
  CHECK(!ResolveRequire(reg, "ghost", &name, &entry, &err));
  CHECK(!ResolveRequire(reg, "lib/../evil.luau", &name, &entry, &err));
  CHECK(!ResolveRequire(reg, "lib//double.luau", &name, &entry, &err));
  CHECK(!ResolveRequire(reg, "", &name, &entry, &err));
}

//----------------------------------------------------------------------------
// Script bindings
//----------------------------------------------------------------------------

static void TestPackageBindings() {
  PackageRegistry reg;
  ScriptApi::Services services;
  services.packages = &reg;
  ScriptApi api(services);
  ScriptBindings bindings;
  const size_t n = api.InstallBindings(&bindings);
  CHECK(n == api::BindingSpecCount());
  CHECK(bindings.Has("package.add"));
  CHECK(bindings.Has("package.resolve"));

  ScriptValue out;
  CHECK(CallBinding(bindings, "package.add",
                    {ScriptValue::Str(PkgJson("lib", "1.0.0"))},
                    {"fileio"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(CallBinding(bindings, "package.add",
                    {ScriptValue::Str(PkgJson("lib", "1.2.0"))},
                    {"fileio"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());

  // A malformed manifest fails the call.
  CHECK(CallBinding(bindings, "package.add",
                    {ScriptValue::Str("{}")}, {"fileio"}, &out) ==
        ScriptStatus::Error);

  CHECK(CallBinding(bindings, "package.has", {ScriptValue::Str("lib")},
                    {"fileio"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());

  CHECK(CallBinding(bindings, "package.versions", {ScriptValue::Str("lib")},
                    {"fileio"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsString() == "1.0.0,1.2.0");

  CHECK(CallBinding(bindings, "package.resolve",
                    {ScriptValue::Str("lib"), ScriptValue::Str("^1.0.0")},
                    {"fileio"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsString() == "lib@1.2.0");

  CHECK(CallBinding(bindings, "package.require_path", {ScriptValue::Str("lib")},
                    {"fileio"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsString() == "lib/main.luau");

  // A resolution failure raises.
  CHECK(CallBinding(bindings, "package.resolve",
                    {ScriptValue::Str("lib"), ScriptValue::Str("^9.0.0")},
                    {"fileio"}, &out) == ScriptStatus::Error);

  CHECK(CallBinding(bindings, "package.clear", {}, {"fileio"}, &out) ==
        ScriptStatus::Ok);
  CHECK(reg.Count() == 0);

  // The package module carries the fileio grant.
  CHECK(CallBinding(bindings, "package.add",
                    {ScriptValue::Str(PkgJson("x", "1.0.0"))}, {}, &out) ==
        ScriptStatus::CapabilityDenied);

  // With no registry bound, registration fails closed.
  ScriptApi::Services none;
  ScriptApi bare(none);
  ScriptBindings bareBindings;
  bare.InstallBindings(&bareBindings);
  CHECK(CallBinding(bareBindings, "package.add",
                    {ScriptValue::Str(PkgJson("x", "1.0.0"))},
                    {"fileio"}, &out) == ScriptStatus::Error);
  CHECK(!out.Truthy());
}

int main() {
  TestSemverParse();
  TestSemverCompare();
  TestConstraints();
  TestManifestParse();
  TestRegistry();
  TestResolveSimple();
  TestResolveFailsClosed();
  TestResolvePicksHighestSatisfying();
  TestRequire();
  TestPackageBindings();
  if (g_fail == 0) printf("test_package: ALL OK\n");
  return g_fail;
}
