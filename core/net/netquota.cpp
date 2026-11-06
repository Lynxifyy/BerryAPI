// Per-script network quotas. See include/berry/netquota.h.
#include "berry/netquota.h"

namespace Berry {

const char* NetQuotaResultName(NetQuotaResult r) {
  switch (r) {
    case NetQuotaResult::Ok: return "ok";
    case NetQuotaResult::TooManySockets: return "too-many-sockets";
    case NetQuotaResult::RateLimited: return "rate-limited";
    case NetQuotaResult::ByteBudgetExhausted: return "byte-budget-exhausted";
  }
  return "?";
}

NetQuotaTracker::State& NetQuotaTracker::For(const std::string& script) {
  return states_[script];
}

void NetQuotaTracker::SetLimits(const std::string& script,
                                const NetQuotaLimits& limits) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (limits.maxConcurrentSockets == 0 && limits.bytesPerSecond == 0 &&
      limits.requestsPerSecond == 0) {
    states_.erase(script);
    return;
  }
  For(script).limits = limits;
}

NetQuotaLimits NetQuotaTracker::Limits(const std::string& script) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = states_.find(script);
  return it == states_.end() ? NetQuotaLimits{} : it->second.limits;
}

NetQuotaResult NetQuotaTracker::AllowConnect(const std::string& script,
                                             u64 nowMs) {
  std::lock_guard<std::mutex> lk(mutex_);
  State& st = For(script);
  if (st.limits.maxConcurrentSockets > 0 &&
      st.open >= st.limits.maxConcurrentSockets) {
    return NetQuotaResult::TooManySockets;
  }
  if (st.limits.requestsPerSecond > 0) {
    if (nowMs - st.reqWindowStart >= 1000) {
      st.reqWindowStart = nowMs;
      st.reqsThisSecond = 0;
    }
    if (st.reqsThisSecond >= st.limits.requestsPerSecond) {
      return NetQuotaResult::RateLimited;
    }
    ++st.reqsThisSecond;
  }
  return NetQuotaResult::Ok;
}

void NetQuotaTracker::SocketOpened(const std::string& script) {
  std::lock_guard<std::mutex> lk(mutex_);
  ++For(script).open;
}

void NetQuotaTracker::SocketClosed(const std::string& script) {
  std::lock_guard<std::mutex> lk(mutex_);
  State& st = For(script);
  if (st.open > 0) --st.open;
}

NetQuotaResult NetQuotaTracker::ChargeBytes(const std::string& script,
                                            u64 bytes, u64 nowMs) {
  std::lock_guard<std::mutex> lk(mutex_);
  State& st = For(script);
  if (st.limits.bytesPerSecond == 0) return NetQuotaResult::Ok;
  if (nowMs - st.byteWindowStart >= 1000) {
    st.byteWindowStart = nowMs;
    st.bytesThisSecond = 0;
  }
  if (st.bytesThisSecond + bytes > st.limits.bytesPerSecond) {
    return NetQuotaResult::ByteBudgetExhausted;
  }
  st.bytesThisSecond += bytes;
  return NetQuotaResult::Ok;
}

size_t NetQuotaTracker::OpenSockets(const std::string& script) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = states_.find(script);
  return it == states_.end() ? 0 : it->second.open;
}

u64 NetQuotaTracker::BytesRemaining(const std::string& script, u64 nowMs) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = states_.find(script);
  if (it == states_.end() || it->second.limits.bytesPerSecond == 0) return 0;
  const State& st = it->second;
  if (nowMs - st.byteWindowStart >= 1000) return st.limits.bytesPerSecond;
  return st.bytesThisSecond >= st.limits.bytesPerSecond
             ? 0
             : st.limits.bytesPerSecond - st.bytesThisSecond;
}

void NetQuotaTracker::Forget(const std::string& script) {
  std::lock_guard<std::mutex> lk(mutex_);
  states_.erase(script);
}

void NetQuotaTracker::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  states_.clear();
}

}  // namespace Berry
