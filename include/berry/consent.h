#pragma once
// Capability consent state (improvement 64).
//
// A manifest declares the capabilities it wants and, with each, the reason it
// wants it. Before a script runs, the user grants or denies each capability;
// the decision is remembered per script (keyed by a stable script id, e.g. the
// manifest name + version or its signing key) so it is not re-asked every
// launch, and can be revoked later.
//
// The store is pure state; the UI is the embedder's. `Prompt()` returns the
// list of (capability, reason) pairs that still need a decision, so the
// embedder renders a consent screen from it.

#include "berry/export.h"
#include "berry/manifest.h"
#include "berry/types.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Berry {

/// One requested capability and the reason the manifest gave.
struct BERRY_API ConsentRequest {
  std::string capability;   // the capability name, e.g. "network"
  std::string reason;       // the manifest's stated reason (may be empty)
  bool granted = false;     // the remembered decision
  bool decided = false;     // true once the user has answered
};

/// Remembers per-script capability decisions.
class BERRY_API ConsentStore {
 public:
  /// Records the user's decision for (scriptId, capability). Returns false when
  /// the capability is not grantable (an unknown kind), so a bad name can never
  /// be recorded as granted.
  bool Decide(const std::string& scriptId, const std::string& capability,
              bool granted);
  /// The remembered decision, or `undecided` when none.
  enum class Decision : u32 { Undecided = 0, Granted, Denied };
  Decision Get(const std::string& scriptId, const std::string& capability) const;
  /// Revokes every decision for a script (so the next run re-asks).
  size_t Revoke(const std::string& scriptId);
  /// Revokes one capability.
  bool RevokeOne(const std::string& scriptId, const std::string& capability);

  /// Given the capabilities a manifest requests, returns the ones that still
  /// need a decision (with their reasons), so the embedder can render a consent
  /// screen. Already-decided ones carry their decision.
  std::vector<ConsentRequest> Prompt(
      const std::string& scriptId,
      const std::vector<std::pair<std::string, std::string>>& requested) const;

  /// True when every requested capability has been granted (so the script may
  /// run). A denied or undecided one makes this false (fail closed).
  bool AllGranted(
      const std::string& scriptId,
      const std::vector<std::pair<std::string, std::string>>& requested) const;

  void Clear();

 private:
  mutable std::mutex mutex_;
  // scriptId -> capability -> granted
  std::map<std::string, std::map<std::string, bool>> decisions_;
};

}  // namespace Berry
