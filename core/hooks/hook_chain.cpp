// Hook-chain slot bookkeeping. See include/berry/hook_chain.h.
#include "berry/hook_chain.h"

#include <mutex>

namespace Berry {
namespace detail {

namespace {
std::mutex g_slotMutex;
bool g_slotUsed[kMaxHookChains] = {};
}  // namespace

ChainSlot* ChainSlots() {
  static ChainSlot slots[kMaxHookChains];
  return slots;
}

size_t ClaimSlot() {
  std::lock_guard<std::mutex> lk(g_slotMutex);
  for (size_t i = 0; i < kMaxHookChains; ++i) {
    if (!g_slotUsed[i]) {
      g_slotUsed[i] = true;
      ChainSlots()[i].used = true;
      return i;
    }
  }
  return kMaxHookChains;
}

void ReleaseSlot(size_t slot) {
  if (slot >= kMaxHookChains) return;
  std::lock_guard<std::mutex> lk(g_slotMutex);
  g_slotUsed[slot] = false;
  ChainSlots()[slot].used = false;
  ChainSlots()[slot].chain = nullptr;
}

}  // namespace detail
}  // namespace Berry
