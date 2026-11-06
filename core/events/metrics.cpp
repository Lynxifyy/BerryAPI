// Metrics registry implementation. See include/berry/metrics.h.
#include "berry/metrics.h"

#include <algorithm>
#include <limits>

namespace Berry {

namespace {

// Frame-time-style buckets in milliseconds: sub-millisecond through a
// multi-second stall, so a p50/p99 estimate is meaningful without storing
// every sample. Shared by every histogram; a metric with a different unit
// still gets a useful distribution because the bounds are logarithmic-ish.
const f64 kBounds[] = {0.1, 0.25, 0.5, 1, 2, 4, 8, 16,
                       33, 50, 100, 250, 500, 1000, 2000, 5000};

}  // namespace

const f64* Histogram::Bounds() { return kBounds; }
size_t Histogram::BoundCount() { return sizeof(kBounds) / sizeof(kBounds[0]); }

void Histogram::Observe(f64 v) {
  if (v < 0) v = 0;
  count_.fetch_add(1, std::memory_order_relaxed);
  // sum/min/max use relaxed atomics; a reader may see a slightly stale value,
  // which is fine for a metric.
  f64 prev = sum_.load(std::memory_order_relaxed);
  while (!sum_.compare_exchange_weak(prev, prev + v, std::memory_order_relaxed)) {
  }
  // min starts at +inf so the first sample always wins; a 0 initial value would
  // make min permanently 0 for an all-positive series.
  f64 mn = min_.load(std::memory_order_relaxed);
  while (v < mn && !min_.compare_exchange_weak(mn, v, std::memory_order_relaxed)) {
  }
  f64 mx = max_.load(std::memory_order_relaxed);
  while (v > mx && !max_.compare_exchange_weak(mx, v, std::memory_order_relaxed)) {
  }
  size_t b = 0;
  while (b < BoundCount() && v > kBounds[b]) ++b;
  buckets_[b].fetch_add(1, std::memory_order_relaxed);
}

std::vector<u64> Histogram::Buckets() const {
  std::vector<u64> out(BoundCount() + 1);
  for (size_t i = 0; i <= BoundCount(); ++i) {
    out[i] = buckets_[i].load(std::memory_order_relaxed);
  }
  return out;
}

f64 Histogram::Mean() const {
  const u64 c = Count();
  return c ? Sum() / (f64)c : 0.0;
}

MetricsRegistry& MetricsRegistry::Instance() {
  static MetricsRegistry* inst = new MetricsRegistry();
  return *inst;
}

Counter* MetricsRegistry::CounterFor(const std::string& name) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = counters_.find(name);
  if (it != counters_.end()) return it->second.get();
  auto c = std::make_unique<Counter>();
  Counter* p = c.get();
  counters_.emplace(name, std::move(c));
  return p;
}

Gauge* MetricsRegistry::GaugeFor(const std::string& name) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = gauges_.find(name);
  if (it != gauges_.end()) return it->second.get();
  auto g = std::make_unique<Gauge>();
  Gauge* p = g.get();
  gauges_.emplace(name, std::move(g));
  return p;
}

Histogram* MetricsRegistry::HistogramFor(const std::string& name) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = histograms_.find(name);
  if (it != histograms_.end()) return it->second.get();
  auto h = std::make_unique<Histogram>();
  Histogram* p = h.get();
  histograms_.emplace(name, std::move(h));
  return p;
}

std::vector<std::pair<std::string, i64>> MetricsRegistry::Snapshot() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<std::pair<std::string, i64>> out;
  out.reserve(counters_.size() + gauges_.size());
  for (const auto& kv : counters_) out.emplace_back(kv.first, (i64)kv.second->Value());
  for (const auto& kv : gauges_) out.emplace_back(kv.first, kv.second->Value());
  return out;
}

std::vector<MetricsRegistry::HistogramSummary>
MetricsRegistry::HistogramSnapshot() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<HistogramSummary> out;
  out.reserve(histograms_.size());
  for (const auto& kv : histograms_) {
    HistogramSummary s;
    s.name = kv.first;
    s.count = kv.second->Count();
    s.mean = kv.second->Mean();
    s.min = kv.second->Min();
    s.max = kv.second->Max();
    out.push_back(std::move(s));
  }
  return out;
}

void MetricsRegistry::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  counters_.clear();
  gauges_.clear();
  histograms_.clear();
}

size_t MetricsRegistry::Count() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return counters_.size() + gauges_.size() + histograms_.size();
}

}  // namespace Berry
