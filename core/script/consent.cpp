// Capability consent state. See include/berry/consent.h.
#include "berry/consent.h"

namespace Berry {

bool ConsentStore::Decide(const std::string& scriptId,
                          const std::string& capability, bool granted) {
  if (scriptId.empty() || capability.empty()) return false;
  // Refuse to record a decision for a capability that is not grantable, so a
  // typo cannot be remembered as granted.
  Capability cap;
  if (!Capability::Parse(capability, &cap) || !IsGrantableKind(cap.kind)) {
    return false;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  decisions_[scriptId][capability] = granted;
  return true;
}

ConsentStore::Decision ConsentStore::Get(const std::string& scriptId,
                                         const std::string& capability) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = decisions_.find(scriptId);
  if (it == decisions_.end()) return Decision::Undecided;
  auto c = it->second.find(capability);
  if (c == it->second.end()) return Decision::Undecided;
  return c->second ? Decision::Granted : Decision::Denied;
}

size_t ConsentStore::Revoke(const std::string& scriptId) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = decisions_.find(scriptId);
  if (it == decisions_.end()) return 0;
  const size_t n = it->second.size();
  decisions_.erase(it);
  return n;
}

bool ConsentStore::RevokeOne(const std::string& scriptId,
                             const std::string& capability) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = decisions_.find(scriptId);
  if (it == decisions_.end()) return false;
  return it->second.erase(capability) > 0;
}

std::vector<ConsentRequest> ConsentStore::Prompt(
    const std::string& scriptId,
    const std::vector<std::pair<std::string, std::string>>& requested) const {
  std::vector<ConsentRequest> out;
  for (const auto& kv : requested) {
    ConsentRequest r;
    r.capability = kv.first;
    r.reason = kv.second;
    const Decision d = Get(scriptId, kv.first);
    r.decided = d != Decision::Undecided;
    r.granted = d == Decision::Granted;
    out.push_back(std::move(r));
  }
  return out;
}

bool ConsentStore::AllGranted(
    const std::string& scriptId,
    const std::vector<std::pair<std::string, std::string>>& requested) const {
  for (const auto& kv : requested) {
    if (Get(scriptId, kv.first) != Decision::Granted) return false;
  }
  return true;
}

void ConsentStore::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  decisions_.clear();
}

}  // namespace Berry
