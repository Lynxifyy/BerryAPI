// Host tests for the intent layer and server policy profiles.
//
// Covers:
//   * The static classification table: every named intent classifies as
//     client-side or world-write, and an unnamed one is unknown.
//   * Name round trips and the closed vocabulary (intent.list).
//   * Group membership and the group mask/list helpers.
//   * Server profile matching: exact host, suffix wildcard, "*" fallback, and
//     the specificity ordering that picks the most specific match.
//   * Profile add validation (empty name or pattern refused).
//   * The validator: world writes are denied by classification and cannot be
//     re-enabled by any profile; a disabled group is refused; an open profile
//     allows client-side intents; a missing capability is refused.
//   * The intent.* script bindings, including the fail-closed case with no
//     validator bound.
#include "berry/api_bindings.h"
#include "berry/generated_bindings.h"
#include "berry/intent.h"
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
  ScriptHost host("intent-test", Grant(grants), nullptr);
  host.SetBindings(&bindings);
  return host.CallBinding(name, args, out);
}

}  // namespace

static void TestClassification() {
  // Every client-side intent classifies as client-side.
  CHECK(ClassifyIntent(IntentKind::Zoom) == IntentClass::ClientSide);
  CHECK(ClassifyIntent(IntentKind::Freelook) == IntentClass::ClientSide);
  CHECK(ClassifyIntent(IntentKind::Fov) == IntentClass::ClientSide);
  CHECK(ClassifyIntent(IntentKind::Perspective) == IntentClass::ClientSide);
  CHECK(ClassifyIntent(IntentKind::Keybind) == IntentClass::ClientSide);
  CHECK(ClassifyIntent(IntentKind::Hud) == IntentClass::ClientSide);
  CHECK(ClassifyIntent(IntentKind::Nametag) == IntentClass::ClientSide);
  CHECK(ClassifyIntent(IntentKind::Cosmetic) == IntentClass::ClientSide);
  CHECK(ClassifyIntent(IntentKind::Sound) == IntentClass::ClientSide);
  // The crystal advisor is read-only and client-side, not a world write.
  CHECK(ClassifyIntent(IntentKind::CrystalAdvise) == IntentClass::ClientSide);

  // Every world write classifies as world-write.
  CHECK(ClassifyIntent(IntentKind::PlaceBlock) == IntentClass::WorldWrite);
  CHECK(ClassifyIntent(IntentKind::BreakBlock) == IntentClass::WorldWrite);
  CHECK(ClassifyIntent(IntentKind::Attack) == IntentClass::WorldWrite);
  CHECK(ClassifyIntent(IntentKind::Interact) == IntentClass::WorldWrite);
  CHECK(ClassifyIntent(IntentKind::Move) == IntentClass::WorldWrite);
  CHECK(ClassifyIntent(IntentKind::SetTime) == IntentClass::WorldWrite);
  CHECK(ClassifyIntent(IntentKind::SetWeather) == IntentClass::WorldWrite);

  // An unnamed intent is unknown (fail closed).
  CHECK(ClassifyIntent(IntentKind::Unknown) == IntentClass::Unknown);
  CHECK(IntentKindFromName("no_such_intent") == IntentKind::Unknown);
  CHECK(IntentKindFromName("") == IntentKind::Unknown);
}

static void TestNameRoundTrip() {
  for (u32 i = 1; i < (u32)IntentKind::Count; ++i) {
    const IntentKind k = (IntentKind)i;
    const std::string name = IntentKindName(k);
    CHECK(!name.empty());
    CHECK(name != "unknown");
    CHECK(IntentKindFromName(name) == k);
    // Case-insensitive.
    CHECK(IntentKindFromName("ZOOM") == IntentKindFromName("zoom"));
  }
}

static void TestGroups() {
  CHECK(IntentGroupOf(IntentKind::Zoom) == IntentGroup::Camera);
  CHECK(IntentGroupOf(IntentKind::Freelook) == IntentGroup::Camera);
  CHECK(IntentGroupOf(IntentKind::Keybind) == IntentGroup::Input);
  CHECK(IntentGroupOf(IntentKind::Hud) == IntentGroup::Hud);
  CHECK(IntentGroupOf(IntentKind::Nametag) == IntentGroup::Hud);
  CHECK(IntentGroupOf(IntentKind::Cosmetic) == IntentGroup::Cosmetic);
  CHECK(IntentGroupOf(IntentKind::Sound) == IntentGroup::Audio);
  CHECK(IntentGroupOf(IntentKind::CrystalAdvise) == IntentGroup::Crystal);
  CHECK(IntentGroupOf(IntentKind::PlaceBlock) == IntentGroup::World);
  CHECK(IntentGroupOf(IntentKind::Unknown) == IntentGroup::Unknown);

  CHECK(IntentGroupMask(IntentGroup::Camera) != IntentGroupMask(IntentGroup::Hud));

  const u32 mask = IntentGroupMask(IntentGroup::Camera) |
                   IntentGroupMask(IntentGroup::Cosmetic);
  CHECK(IntentGroupList(mask) == "camera,cosmetic");
  CHECK(IntentGroupList(0).empty());
  // The crystal group lists after cosmetic and before world, in table order.
  CHECK(IntentGroupList(IntentGroupMask(IntentGroup::Crystal) |
                        IntentGroupMask(IntentGroup::World)) == "crystal,world");

  // Each group names the capability its actions need.
  CHECK(IntentGroupCapability(IntentGroup::Camera) == CapKind::Input);
  CHECK(IntentGroupCapability(IntentGroup::Hud) == CapKind::Hud);
  CHECK(IntentGroupCapability(IntentGroup::Cosmetic) == CapKind::Cosmetic);
  CHECK(IntentGroupCapability(IntentGroup::Audio) == CapKind::Audio);
  CHECK(IntentGroupCapability(IntentGroup::Crystal) == CapKind::Crystal);
  CHECK(IntentGroupCapability(IntentGroup::Unknown) == CapKind::NoCap);
}

static void TestProfileMatching() {
  ServerPolicyProfile exact;
  exact.serverMatch = "play.example.com";
  CHECK(exact.Matches("play.example.com"));
  CHECK(exact.Matches("PLAY.EXAMPLE.COM"));   // case insensitive
  CHECK(exact.Matches("play.example.com."));  // trailing dot ignored
  CHECK(!exact.Matches("other.example.com"));
  CHECK(!exact.Matches("play.example.com.evil.net"));

  ServerPolicyProfile wild;
  wild.serverMatch = "*.example.com";
  CHECK(wild.Matches("play.example.com"));
  CHECK(wild.Matches("a.b.example.com"));
  CHECK(!wild.Matches("example.com"));        // apex is not a subdomain
  CHECK(!wild.Matches("example.com.evil.net"));
  CHECK(!wild.Matches("notexample.com"));

  ServerPolicyProfile any;
  any.serverMatch = "*";
  CHECK(any.Matches("anything.at.all"));
  CHECK(any.Matches(""));

  // Specificity orders exact > longer wildcard > "*".
  CHECK(exact.Specificity("play.example.com") >
        wild.Specificity("play.example.com"));
  CHECK(wild.Specificity("play.example.com") > any.Specificity("play.example.com"));
  CHECK(any.Specificity("play.example.com") == 0);
  CHECK(exact.Specificity("nope") == -1);
}

static void TestPolicySelection() {
  IntentPolicy policy;
  CHECK(policy.ProfileCount() == 0);

  ServerPolicyProfile open;
  open.name = "open";
  open.serverMatch = "*";
  CHECK(policy.AddProfile(open));

  ServerPolicyProfile wild;
  wild.name = "wild";
  wild.serverMatch = "*.example.com";
  wild.disabledGroups = IntentGroupMask(IntentGroup::Cosmetic);
  CHECK(policy.AddProfile(wild));

  ServerPolicyProfile exact;
  exact.name = "exact";
  exact.serverMatch = "strict.example.com";
  exact.disabledGroups = IntentGroupMask(IntentGroup::Cosmetic) |
                         IntentGroupMask(IntentGroup::Hud);
  CHECK(policy.AddProfile(exact));

  // A host with no specific profile falls back to "*".
  CHECK(policy.ForServer("somewhere.net").name == "open");
  // A subdomain gets the wildcard.
  CHECK(policy.ForServer("play.example.com").name == "wild");
  // The exact host wins over the wildcard.
  const ServerPolicyProfile p = policy.ForServer("strict.example.com");
  CHECK(p.name == "exact");
  CHECK((p.disabledGroups & IntentGroupMask(IntentGroup::Hud)) != 0);
  CHECK((p.disabledGroups & IntentGroupMask(IntentGroup::Cosmetic)) != 0);

  // A malformed profile is refused rather than becoming a catch-all.
  ServerPolicyProfile bad;
  bad.name = "";
  bad.serverMatch = "*";
  CHECK(!policy.AddProfile(bad));
  ServerPolicyProfile bad2;
  bad2.name = "x";
  bad2.serverMatch = "";
  CHECK(!policy.AddProfile(bad2));

  policy.Clear();
  CHECK(policy.ProfileCount() == 0);

  // Builtins: a default (crystal opt-in) plus the documented strict template.
  const auto builtins = IntentPolicy::BuiltinProfiles();
  CHECK(builtins.size() == 2);
  CHECK(builtins[0].name == "default");
  CHECK((builtins[0].disabledGroups & IntentGroupMask(IntentGroup::Crystal)) != 0);
  CHECK(builtins[1].name == "strict");
  CHECK(builtins[1].disabledGroups != 0);
}

static void TestValidator() {
  IntentValidator v;
  // The default profile is open: client-side allowed.
  CHECK(v.Allowed(IntentKind::Zoom));
  CHECK(v.Allowed(IntentKind::Cosmetic));
  CHECK(v.Allowed(IntentKind::Sound));
  // World writes are denied by classification, under any profile.
  CHECK(v.Validate(IntentKind::PlaceBlock) == IntentResult::WorldWrite);
  CHECK(v.Validate(IntentKind::Attack) == IntentResult::WorldWrite);
  // An unnamed intent is denied.
  CHECK(v.Validate(IntentKind::Unknown) == IntentResult::UnknownIntent);
  CHECK(v.Validate((IntentKind)9999) == IntentResult::UnknownIntent);

  // A profile that disables the cosmetic group refuses only that group.
  ServerPolicyProfile p;
  p.name = "no-cosmetics";
  p.serverMatch = "*";
  p.disabledGroups = IntentGroupMask(IntentGroup::Cosmetic);
  v.SetProfile(p);
  CHECK(v.Validate(IntentKind::Cosmetic) == IntentResult::GroupDisabled);
  CHECK(v.Validate(IntentKind::Hud) == IntentResult::Ok);
  CHECK(v.groupEnabled(IntentGroup::Hud));
  CHECK(!v.groupEnabled(IntentGroup::Cosmetic));
  CHECK(v.disabledGroups() == IntentGroupMask(IntentGroup::Cosmetic));

  // Even a profile that tries to enable everything cannot enable a world write.
  ServerPolicyProfile all;
  all.name = "all";
  all.serverMatch = "*";
  all.disabledGroups = 0;
  v.SetProfile(all);
  CHECK(v.Validate(IntentKind::BreakBlock) == IntentResult::WorldWrite);
  CHECK(v.Validate(IntentKind::SetTime) == IntentResult::WorldWrite);

  // Capability check: a client-side intent still needs its group's grant.
  const CapabilitySet none;
  CHECK(v.Validate(IntentKind::Cosmetic, none) == IntentResult::NoCapability);
  const CapabilitySet cosmetic = Grant({"cosmetic"});
  CHECK(v.Validate(IntentKind::Cosmetic, cosmetic) == IntentResult::Ok);
}

// The crystal advisor is its own policy group with its own capability: the
// default denies it (opt-in), a profile that enables it allows it, a profile
// that disables it refuses it, and the group's capability is separate from
// combat.
static void TestCrystalPolicyGroup() {
  // The default denies the crystal group: an unknown server does not silently
  // enable a combat aid.
  IntentValidator v;
  CHECK(!v.Allowed(IntentKind::CrystalAdvise));
  CHECK(v.Validate(IntentKind::CrystalAdvise) == IntentResult::GroupDisabled);
  // Other client-side groups are unaffected by the default.
  CHECK(v.Allowed(IntentKind::Zoom));
  CHECK(v.Allowed(IntentKind::Cosmetic));
  CHECK(v.Allowed(IntentKind::Sound));

  // A profile that explicitly enables everything allows the advisor.
  ServerPolicyProfile open;
  open.name = "open";
  open.serverMatch = "*";
  open.disabledGroups = 0;
  v.SetProfile(open);
  CHECK(v.Allowed(IntentKind::CrystalAdvise));
  const CapabilitySet combatOnly = Grant({"combat"});
  CHECK(v.Validate(IntentKind::CrystalAdvise, combatOnly) ==
        IntentResult::NoCapability);
  const CapabilitySet crystalOnly = Grant({"crystal"});
  CHECK(v.Validate(IntentKind::CrystalAdvise, crystalOnly) == IntentResult::Ok);

  // A server that disables the crystal group denies the advisor and leaves the
  // other client-side groups alone.
  ServerPolicyProfile noCrystal;
  noCrystal.name = "no-crystal";
  noCrystal.serverMatch = "*";
  noCrystal.disabledGroups = IntentGroupMask(IntentGroup::Crystal);
  v.SetProfile(noCrystal);
  CHECK(v.Validate(IntentKind::CrystalAdvise) == IntentResult::GroupDisabled);
  CHECK(!v.groupEnabled(IntentGroup::Crystal));
  CHECK(v.Allowed(IntentKind::Zoom));
  CHECK(v.Allowed(IntentKind::Cosmetic));
  CHECK(v.Allowed(IntentKind::Sound));
  // A world write is still refused by classification, not by the group.
  CHECK(v.Validate(IntentKind::PlaceBlock) == IntentResult::WorldWrite);

  // The capability check does not override a disabled group.
  CHECK(v.Validate(IntentKind::CrystalAdvise, crystalOnly) ==
        IntentResult::GroupDisabled);
}

// A profile selected for a server that bans client mods must genuinely disable
// the crystal group for that host, and a later switch re-enables it.
static void TestCrystalGroupSelectedForServer() {
  IntentPolicy policy;
  for (const auto& p : IntentPolicy::BuiltinProfiles()) policy.AddProfile(p);

  ServerPolicyProfile allowCrystal;
  allowCrystal.name = "allow-crystal";
  allowCrystal.serverMatch = "*.crystal-allowed.example";
  allowCrystal.disabledGroups = 0;   // explicitly opt in
  policy.AddProfile(allowCrystal);

  // An unknown server falls back to the default, which denies the crystal group
  // even though it allows the other client-side groups.
  IntentValidator v;
  v.SelectFor("some.unknown.example", policy);
  CHECK(v.profile().name == "default");
  CHECK(!v.Allowed(IntentKind::CrystalAdvise));
  CHECK(v.Allowed(IntentKind::Zoom));

  // A server whose profile explicitly opts in allows the advisor.
  v.SelectFor("play.crystal-allowed.example", policy);
  CHECK(v.profile().name == "allow-crystal");
  CHECK(v.Allowed(IntentKind::CrystalAdvise));
}

static void TestSelectForServer() {
  IntentPolicy policy;
  for (const auto& p : IntentPolicy::BuiltinProfiles()) policy.AddProfile(p);

  ServerPolicyProfile strict;
  strict.name = "ban-cosmetics";
  strict.serverMatch = "*.banned.example";
  strict.disabledGroups = IntentGroupMask(IntentGroup::Cosmetic);
  policy.AddProfile(strict);

  IntentValidator v;
  const ServerPolicyProfile chosen = v.SelectFor("play.banned.example", policy);
  CHECK(chosen.name == "ban-cosmetics");
  CHECK(v.server() == "play.banned.example");
  CHECK(v.profile().name == "ban-cosmetics");
  CHECK(!v.Allowed(IntentKind::Cosmetic));
  CHECK(v.Allowed(IntentKind::Zoom));

  // Moving to a different server re-selects. An unknown server gets the
  // default profile, which denies the crystal group but allows cosmetic.
  v.SelectFor("fun.server.net", policy);
  CHECK(v.profile().name == "default");
  CHECK(v.Allowed(IntentKind::Cosmetic));
  CHECK(!v.Allowed(IntentKind::CrystalAdvise));
}

//----------------------------------------------------------------------------
// Script bindings
//----------------------------------------------------------------------------

static void TestIntentBindings() {
  IntentValidator v;
  ScriptApi::Services services;
  services.intents = &v;
  ScriptApi api(services);
  ScriptBindings bindings;
  const size_t n = api.InstallBindings(&bindings);
  CHECK(n == api::BindingSpecCount());
  CHECK(bindings.Has("intent.allowed"));
  CHECK(bindings.Has("intent.classify"));
  CHECK(bindings.Has("intent.list"));

  ScriptValue out;
  // The intent module carries the input grant, so a bare call is refused and a
  // granted one succeeds.
  CHECK(CallBinding(bindings, "intent.allowed", {ScriptValue::Str("zoom")}, {}, &out) ==
        ScriptStatus::CapabilityDenied);
  CHECK(CallBinding(bindings, "intent.allowed", {ScriptValue::Str("zoom")}, {"input"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.Truthy());

  CHECK(CallBinding(bindings, "intent.allowed", {ScriptValue::Str("place_block")}, {"input"}, &out) ==
        ScriptStatus::Ok);
  CHECK(!out.Truthy());

  CHECK(CallBinding(bindings, "intent.classify", {ScriptValue::Str("zoom")}, {"input"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsString() == "client-side");
  CHECK(CallBinding(bindings, "intent.classify", {ScriptValue::Str("attack")}, {"input"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsString() == "world-write");

  CHECK(CallBinding(bindings, "intent.list", {}, {"input"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsString().find("zoom") != std::string::npos);
  CHECK(out.AsString().find("place_block") != std::string::npos);

  // With no validator bound, intent.allowed fails closed (false).
  ScriptApi::Services none;
  ScriptApi bare(none);
  ScriptBindings bareBindings;
  bare.InstallBindings(&bareBindings);
  CHECK(CallBinding(bareBindings, "intent.allowed", {ScriptValue::Str("zoom")}, {"input"}, &out) ==
        ScriptStatus::Ok);
  CHECK(!out.Truthy());
  CHECK(CallBinding(bareBindings, "intent.profile", {}, {"input"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsString().empty());
}

// A server profile that disables the camera group must genuinely disable the
// client-side camera action, not just report it as disabled.
static void TestIntentGatesZoom() {
  IntentValidator v;
  ScriptApi::Services services;
  services.intents = &v;
  ScriptApi api(services);
  ScriptBindings bindings;
  api.InstallBindings(&bindings);

  ScriptValue out;
  // Open profile: zoom is applied.
  CHECK(CallBinding(bindings, "render.set_zoom", {ScriptValue::Num(3.0)},
                    {"render"}, &out) == ScriptStatus::Ok);
  CHECK(api.zoomFactor() == 3.0f);

  // A profile that disables the camera group refuses the zoom and leaves the
  // previous value in place.
  ServerPolicyProfile p;
  p.name = "no-camera";
  p.serverMatch = "*";
  p.disabledGroups = IntentGroupMask(IntentGroup::Camera);
  v.SetProfile(p);
  CHECK(CallBinding(bindings, "render.set_zoom", {ScriptValue::Num(5.0)},
                    {"render"}, &out) == ScriptStatus::Error);
  CHECK(api.zoomFactor() == 3.0f);

  // Re-enabling the group allows it again.
  ServerPolicyProfile open;
  open.name = "open";
  open.serverMatch = "*";
  v.SetProfile(open);
  CHECK(CallBinding(bindings, "render.set_zoom", {ScriptValue::Num(5.0)},
                    {"render"}, &out) == ScriptStatus::Ok);
  CHECK(api.zoomFactor() == 5.0f);
}

int main() {
  TestClassification();
  TestNameRoundTrip();
  TestGroups();
  TestProfileMatching();
  TestPolicySelection();
  TestValidator();
  TestCrystalPolicyGroup();
  TestCrystalGroupSelectedForServer();
  TestSelectForServer();
  TestIntentBindings();
  TestIntentGatesZoom();
  if (g_fail == 0) printf("test_intent: ALL OK\n");
  return g_fail;
}
