#pragma once
// BerryAPI intent layer + server policy profiles.
//
// World and Combat are read-only: nothing in the core mutates game state, and
// no binding hands out a writer. That leaves the question of what a script (or
// a client module) may still *ask for*. The answer is an INTENT: a named
// request that a policy layer validates before anything acts on it.
//
// An intent is classified once, in a static table, so the classification is a
// pure function and cannot drift per call site:
//
//   * ClientSide — what normal player input could do, or what stays on this
//     screen: zoom, freelook, FOV, keybinds, HUD, sounds, perspective, local
//     cosmetics. These are allowed; they change nothing the server sees.
//   * WorldWrite — anything that would mutate the world or the local player's
//     authoritative state (place/break a block, attack, move, set the time).
//     These are denied. The client is not a second authority on the world.
//   * Unknown — an action the table does not name. Denied, so a typo or a
//     future name fails closed rather than slipping through.
//
// On top of the classification sits a server policy. Some servers ban client
// mods outright, and a cosmetic another player can see is exactly the kind of
// thing that gets a user banned. A ServerPolicyProfile matches a server address
// and disables whole intent GROUPS (camera, input, HUD, audio, cosmetic) at
// once, so joining a server that bans them turns the group off without the user
// reconfiguring anything. Protecting users from bans is the point.
//
// The two layers are independent: classification says what may ever be done,
// the profile says what may be done *here*.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/manifest.h"

#include <mutex>
#include <string>
#include <vector>

namespace Berry {

//----------------------------------------------------------------------------
// Intents
//----------------------------------------------------------------------------

/// A named request a script or module may make. The set is closed: an action
/// not in this list is `Unknown` and is denied.
enum class IntentKind : u32 {
  Unknown = 0,

  // Client-side camera: what the player's own input and this screen control.
  Zoom,
  Freelook,
  Fov,
  Perspective,

  // Client-side input remapping and toggles.
  Keybind,

  // Client-side presentation.
  Hud,
  Nametag,
  Cosmetic,

  // Client-side audio.
  Sound,

  // Client-side advisory: the crystal-placement advisor. Read-only, and
  // classified client-side because it only ranks spots the client already knows
  // and never places a block or sends a packet. It has its own group so a
  // server that bans the crystal-advisor feature can disable it wholesale.
  CrystalAdvise,

  // World writes: denied by classification, always.
  PlaceBlock,
  BreakBlock,
  Attack,
  Interact,
  Move,
  SetTime,
  SetWeather,

  Count,
};

/// The table's verdict on an intent.
enum class IntentClass : u32 {
  Unknown = 0,   // not named in the table -> deny
  ClientSide,    // allowed (input-equivalent or screen-local)
  WorldWrite,    // mutates authoritative state -> deny
};

/// A group of intents a server policy can enable or disable wholesale. The mask
/// of disabled groups is what a profile carries.
enum class IntentGroup : u32 {
  Unknown = 0,
  Camera = 1 << 0,    // Zoom, Freelook, Fov, Perspective
  Input = 1 << 1,     // Keybind
  Hud = 1 << 2,       // Hud, Nametag
  Audio = 1 << 3,     // Sound
  Cosmetic = 1 << 4,  // Cosmetic
  World = 1 << 5,     // every WorldWrite intent
  Crystal = 1 << 6,   // CrystalAdvise
};

const char* IntentKindName(IntentKind k);
IntentKind IntentKindFromName(const std::string& name);
const char* IntentClassName(IntentClass c);
const char* IntentGroupName(IntentGroup g);
u32 IntentGroupMask(IntentGroup g);
std::string IntentGroupList(u32 mask);   // "camera,hud" or "" for none

/// The group an intent belongs to (Unknown for an unnamed intent).
IntentGroup IntentGroupOf(IntentKind k);

/// The table's classification of an intent. Pure; no state.
IntentClass ClassifyIntent(IntentKind k);

/// The capability a group's actions require, or CapKind::NoCap when a group is
/// not tied to one. Used so the intent gate and the binding gate agree.
CapKind IntentGroupCapability(IntentGroup g);

//----------------------------------------------------------------------------
// Why an intent was refused. Every non-Ok value is a denial.
//----------------------------------------------------------------------------

enum class IntentResult : u32 {
  Ok = 0,
  UnknownIntent,     // not in the table
  WorldWrite,        // the core is read-only
  GroupDisabled,     // the active server profile disabled this group
  NoCapability,      // the caller was not granted the group's capability
};

const char* IntentResultName(IntentResult r);

//----------------------------------------------------------------------------
// Server policy profiles
//----------------------------------------------------------------------------

/// A policy for one class of server. `serverMatch` is a hostname pattern:
///   "*"            — any server (the fallback)
///   "play.example.com" — that exact host
///   "*.example.com"    — any subdomain of example.com (not the apex, matching
///                        the usual TLS/proxy wildcard convention)
/// `disabledGroups` is an IntentGroup mask. WorldWrite intents are denied by
/// classification regardless of the mask, so a profile cannot re-enable them.
struct BERRY_API ServerPolicyProfile {
  std::string name;
  std::string serverMatch = "*";
  u32 disabledGroups = 0;
  std::string reason;

  /// True when this profile matches `host`. Host comparison is case
  /// insensitive; a trailing dot on the host is ignored.
  bool Matches(const std::string& host) const;

  /// A specificity score for match ordering: an exact host beats a longer
  /// suffix wildcard, which beats "*". Returns -1 when it does not match.
  int Specificity(const std::string& host) const;

  bool GroupEnabled(IntentGroup g) const {
    return (disabledGroups & IntentGroupMask(g)) == 0;
  }
};

/// The set of profiles, and the lookup that picks one for a server address.
/// Profiles are data; `BuiltinProfiles()` supplies the safe default plus a
/// documented strict template, and a host may add its own.
class BERRY_API IntentPolicy {
 public:
  IntentPolicy() = default;

  /// Adds a profile. Returns false for one that cannot be used (an empty name,
  /// or an empty server pattern), so a malformed entry is refused rather than
  /// silently becoming a catch-all.
  bool AddProfile(const ServerPolicyProfile& p);

  /// The number of registered profiles.
  size_t ProfileCount() const;
  std::vector<ServerPolicyProfile> Profiles() const;
  void Clear();

  /// The profile that applies to `host`: the most specific match wins (exact
  /// host, then the longest suffix wildcard, then "*"). When nothing matches —
  /// which cannot happen once the default is registered — returns an open
  /// profile, because refusing to run at all is worse than the safe default.
  ServerPolicyProfile ForServer(const std::string& host) const;

  /// The open default ("*", nothing disabled) plus the documented strict
  /// template. Registering these gives a fresh policy a sane baseline.
  static std::vector<ServerPolicyProfile> BuiltinProfiles();

 private:
  mutable std::mutex mutex_;
  std::vector<ServerPolicyProfile> profiles_;
};

//----------------------------------------------------------------------------
// IntentValidator — the per-session gate.
//----------------------------------------------------------------------------

/// Holds the profile for the server the client is currently on and answers
/// intent questions against it. This is the object an action binding consults
/// before it acts, so a profile that disables a group genuinely disables it.
///
/// Thread safety: the profile is set from the session/connect path and read
/// from the script worker, so every accessor locks.
class BERRY_API IntentValidator {
 public:
  IntentValidator();
  explicit IntentValidator(const ServerPolicyProfile& profile);

  void SetProfile(const ServerPolicyProfile& profile);
  ServerPolicyProfile profile() const;

  /// Picks the profile for `host` from `policy`, installs it, and records the
  /// host for display. Returns the profile that was chosen. This is the call
  /// the connect path makes: the profile follows the server.
  ServerPolicyProfile SelectFor(const std::string& host,
                                const IntentPolicy& policy);

  /// Validates an intent against the classification and the profile.
  IntentResult Validate(IntentKind kind) const;

  /// As above, and additionally requires `granted` to cover the group's
  /// capability. Use this where the caller has a script's grant set; the
  /// binding gate already enforces it, so this is defence in depth.
  IntentResult Validate(IntentKind kind, const CapabilitySet& granted) const;

  bool Allowed(IntentKind kind) const {
    return Validate(kind) == IntentResult::Ok;
  }

  bool groupEnabled(IntentGroup g) const;
  u32 disabledGroups() const;

  /// The server address this validator's profile was chosen for, for display.
  std::string server() const;

 private:
  mutable std::mutex mutex_;
  ServerPolicyProfile profile_;
  std::string server_;
};

}  // namespace Berry
