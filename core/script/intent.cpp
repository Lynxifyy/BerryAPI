// Intent layer + server policy profiles. See include/berry/intent.h for the
// model: a static classification of every intent, and a per-server profile that
// can disable whole groups of client-side intents.
#include "berry/intent.h"

#include <algorithm>
#include <cctype>

namespace Berry {

namespace {

/// The intent table. One row per intent: its name, its group, and how it is
/// classified. Kept as a single table so classification is a pure lookup and
/// cannot differ between call sites.
struct IntentRow {
  IntentKind kind;
  const char* name;
  IntentGroup group;
  IntentClass cls;
};

const IntentRow kIntents[] = {
    // Client-side camera and input: what the player's own controls do.
    {IntentKind::Zoom, "zoom", IntentGroup::Camera, IntentClass::ClientSide},
    {IntentKind::Freelook, "freelook", IntentGroup::Camera, IntentClass::ClientSide},
    {IntentKind::Fov, "fov", IntentGroup::Camera, IntentClass::ClientSide},
    {IntentKind::Perspective, "perspective", IntentGroup::Camera, IntentClass::ClientSide},
    {IntentKind::Keybind, "keybind", IntentGroup::Input, IntentClass::ClientSide},
    // Client-side presentation.
    {IntentKind::Hud, "hud", IntentGroup::Hud, IntentClass::ClientSide},
    {IntentKind::Nametag, "nametag", IntentGroup::Hud, IntentClass::ClientSide},
    {IntentKind::Cosmetic, "cosmetic", IntentGroup::Cosmetic, IntentClass::ClientSide},
    {IntentKind::Sound, "sound", IntentGroup::Audio, IntentClass::ClientSide},
    {IntentKind::CrystalAdvise, "crystal_advise", IntentGroup::Crystal,
     IntentClass::ClientSide},
    // World writes: the client is not a second authority on the world.
    {IntentKind::PlaceBlock, "place_block", IntentGroup::World, IntentClass::WorldWrite},
    {IntentKind::BreakBlock, "break_block", IntentGroup::World, IntentClass::WorldWrite},
    {IntentKind::Attack, "attack", IntentGroup::World, IntentClass::WorldWrite},
    {IntentKind::Interact, "interact", IntentGroup::World, IntentClass::WorldWrite},
    {IntentKind::Move, "move", IntentGroup::World, IntentClass::WorldWrite},
    {IntentKind::SetTime, "set_time", IntentGroup::World, IntentClass::WorldWrite},
    {IntentKind::SetWeather, "set_weather", IntentGroup::World, IntentClass::WorldWrite},
};

const IntentRow* RowFor(IntentKind k) {
  for (const auto& r : kIntents) {
    if (r.kind == k) return &r;
  }
  return nullptr;
}

std::string LowerAscii(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

/// Strips a single trailing dot ("example.com." == "example.com").
std::string NormalizeHost(const std::string& host) {
  std::string h = LowerAscii(host);
  if (!h.empty() && h.back() == '.') h.pop_back();
  return h;
}

/// A host pattern matches when it is "*", equal to the host, or a "*." suffix
/// wildcard whose remainder the host ends with on a label boundary. Like the
/// usual TLS/proxy wildcard, "*." covers subdomains but not the apex, so
/// "*.example.com" matches "play.example.com" but not "example.com".
bool PatternMatches(const std::string& pattern, const std::string& host) {
  if (pattern == "*") return true;
  if (pattern.size() > 2 && pattern.compare(0, 2, "*.") == 0) {
    const std::string suffix = pattern.substr(1);   // ".example.com"
    if (host.size() <= suffix.size()) return false;
    return host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
  }
  return pattern == host;
}

/// The profile returned when nothing matches. Client-side intents are allowed,
/// with one deliberate exception: the crystal-placement advisor is competitive
/// assistance, so its group is denied unless a profile explicitly allows it.
/// Refusing to run at all would be worse, so the fallback is not a blanket deny;
/// but an unknown server must not silently enable a combat aid the operator
/// never opted into.
ServerPolicyProfile DefaultProfile() {
  ServerPolicyProfile p;
  p.name = "default";
  p.serverMatch = "*";
  p.disabledGroups = IntentGroupMask(IntentGroup::Crystal);
  p.reason = "default: client-side intents allowed; crystal advisor opt-in";
  return p;
}

}  // namespace

//----------------------------------------------------------------------------
// Intent table accessors
//----------------------------------------------------------------------------

const char* IntentKindName(IntentKind k) {
  const IntentRow* r = RowFor(k);
  return r ? r->name : "unknown";
}

IntentKind IntentKindFromName(const std::string& name) {
  const std::string n = LowerAscii(name);
  for (const auto& r : kIntents) {
    if (n == r.name) return r.kind;
  }
  return IntentKind::Unknown;
}

const char* IntentClassName(IntentClass c) {
  switch (c) {
    case IntentClass::Unknown: return "unknown";
    case IntentClass::ClientSide: return "client-side";
    case IntentClass::WorldWrite: return "world-write";
  }
  return "unknown";
}

const char* IntentGroupName(IntentGroup g) {
  switch (g) {
    case IntentGroup::Unknown: return "unknown";
    case IntentGroup::Camera: return "camera";
    case IntentGroup::Input: return "input";
    case IntentGroup::Hud: return "hud";
    case IntentGroup::Audio: return "audio";
    case IntentGroup::Cosmetic: return "cosmetic";
    case IntentGroup::Crystal: return "crystal";
    case IntentGroup::World: return "world";
  }
  return "unknown";
}

u32 IntentGroupMask(IntentGroup g) { return (u32)g; }

std::string IntentGroupList(u32 mask) {
  static const IntentGroup kOrder[] = {
      IntentGroup::Camera, IntentGroup::Input, IntentGroup::Hud,
      IntentGroup::Audio, IntentGroup::Cosmetic, IntentGroup::Crystal,
      IntentGroup::World};
  std::string out;
  for (IntentGroup g : kOrder) {
    if (mask & IntentGroupMask(g)) {
      if (!out.empty()) out += ",";
      out += IntentGroupName(g);
    }
  }
  return out;
}

IntentGroup IntentGroupOf(IntentKind k) {
  const IntentRow* r = RowFor(k);
  return r ? r->group : IntentGroup::Unknown;
}

IntentClass ClassifyIntent(IntentKind k) {
  const IntentRow* r = RowFor(k);
  return r ? r->cls : IntentClass::Unknown;
}

CapKind IntentGroupCapability(IntentGroup g) {
  switch (g) {
    case IntentGroup::Camera: return CapKind::Input;
    case IntentGroup::Input: return CapKind::Input;
    case IntentGroup::Hud: return CapKind::Hud;
    case IntentGroup::Audio: return CapKind::Audio;
    case IntentGroup::Cosmetic: return CapKind::Cosmetic;
    case IntentGroup::Crystal: return CapKind::Crystal;
    // A world write is refused by classification before the capability is
    // consulted, so the group's capability is the read-only world grant.
    case IntentGroup::World: return CapKind::World;
    case IntentGroup::Unknown: return CapKind::NoCap;
  }
  return CapKind::NoCap;
}

const char* IntentResultName(IntentResult r) {
  switch (r) {
    case IntentResult::Ok: return "ok";
    case IntentResult::UnknownIntent: return "unknown intent";
    case IntentResult::WorldWrite: return "world write denied";
    case IntentResult::GroupDisabled: return "group disabled by server policy";
    case IntentResult::NoCapability: return "capability not granted";
  }
  return "unknown";
}

//----------------------------------------------------------------------------
// ServerPolicyProfile
//----------------------------------------------------------------------------

bool ServerPolicyProfile::Matches(const std::string& host) const {
  return Specificity(host) >= 0;
}

int ServerPolicyProfile::Specificity(const std::string& host) const {
  if (serverMatch.empty()) return -1;
  const std::string pattern = NormalizeHost(serverMatch);
  const std::string h = NormalizeHost(host);
  if (!PatternMatches(pattern, h)) return -1;
  if (pattern == "*") return 0;
  // A longer pattern (a longer suffix, or an exact host) is more specific.
  return (int)pattern.size();
}

//----------------------------------------------------------------------------
// IntentPolicy
//----------------------------------------------------------------------------

bool IntentPolicy::AddProfile(const ServerPolicyProfile& p) {
  if (p.name.empty() || p.serverMatch.empty()) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  profiles_.push_back(p);
  return true;
}

size_t IntentPolicy::ProfileCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return profiles_.size();
}

std::vector<ServerPolicyProfile> IntentPolicy::Profiles() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return profiles_;
}

void IntentPolicy::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  profiles_.clear();
}

ServerPolicyProfile IntentPolicy::ForServer(const std::string& host) const {
  std::lock_guard<std::mutex> lk(mutex_);
  const ServerPolicyProfile* best = nullptr;
  int bestScore = -1;
  for (const auto& p : profiles_) {
    const int score = p.Specificity(host);
    if (score > bestScore) {
      bestScore = score;
      best = &p;
    }
  }
  if (!best) return DefaultProfile();
  return *best;
}

std::vector<ServerPolicyProfile> IntentPolicy::BuiltinProfiles() {
  std::vector<ServerPolicyProfile> out;

  // The default for any server with no more specific profile: client-side
  // intents are allowed, but the crystal-placement advisor is opt-in. A
  // competitive combat aid is exactly the kind of thing an operator should have
  // to enable deliberately, so an unknown server denies the crystal group.
  // World writes are denied by classification, not by the mask.
  out.push_back(DefaultProfile());

  // The documented strict template: a server that bans client mods outright.
  // Nothing here is enabled by default; an operator copies it and narrows the
  // pattern to the servers that need it.
  ServerPolicyProfile strict;
  strict.name = "strict";
  strict.serverMatch = "*.example-banned.invalid";
  strict.disabledGroups = IntentGroupMask(IntentGroup::Cosmetic) |
                          IntentGroupMask(IntentGroup::Hud) |
                          IntentGroupMask(IntentGroup::Audio);
  strict.reason = "server bans client mods; cosmetic/hud/audio disabled";
  out.push_back(strict);

  return out;
}

//----------------------------------------------------------------------------
// IntentValidator
//----------------------------------------------------------------------------

IntentValidator::IntentValidator() { profile_ = DefaultProfile(); }

IntentValidator::IntentValidator(const ServerPolicyProfile& profile)
    : profile_(profile) {}

void IntentValidator::SetProfile(const ServerPolicyProfile& profile) {
  std::lock_guard<std::mutex> lk(mutex_);
  profile_ = profile;
}

ServerPolicyProfile IntentValidator::profile() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return profile_;
}

std::string IntentValidator::server() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return server_;
}

ServerPolicyProfile IntentValidator::SelectFor(const std::string& host,
                                               const IntentPolicy& policy) {
  const ServerPolicyProfile chosen = policy.ForServer(host);
  std::lock_guard<std::mutex> lk(mutex_);
  profile_ = chosen;
  server_ = host;
  return chosen;
}

IntentResult IntentValidator::Validate(IntentKind kind) const {
  // Classification first: an unnamed intent or a world write is refused before
  // the profile is even consulted, so no profile can enable either.
  const IntentClass cls = ClassifyIntent(kind);
  if (cls == IntentClass::Unknown) return IntentResult::UnknownIntent;
  if (cls == IntentClass::WorldWrite) return IntentResult::WorldWrite;

  std::lock_guard<std::mutex> lk(mutex_);
  if (!profile_.GroupEnabled(IntentGroupOf(kind))) {
    return IntentResult::GroupDisabled;
  }
  return IntentResult::Ok;
}

IntentResult IntentValidator::Validate(IntentKind kind,
                                       const CapabilitySet& granted) const {
  const IntentResult r = Validate(kind);
  if (r != IntentResult::Ok) return r;
  const CapKind need = IntentGroupCapability(IntentGroupOf(kind));
  if (need == CapKind::NoCap) return IntentResult::Ok;
  Capability cap;
  cap.kind = need;
  if (!granted.CoversCap(cap)) return IntentResult::NoCapability;
  return IntentResult::Ok;
}

bool IntentValidator::groupEnabled(IntentGroup g) const {
  std::lock_guard<std::mutex> lk(mutex_);
  return profile_.GroupEnabled(g);
}

u32 IntentValidator::disabledGroups() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return profile_.disabledGroups;
}

}  // namespace Berry
