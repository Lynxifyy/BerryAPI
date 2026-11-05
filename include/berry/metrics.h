#pragma once
// A small metrics registry (improvement 91): counters, gauges and histograms
// that subsystems bump and a script (or the debug endpoint) reads.
//
// It is deliberately tiny and allocation-light: a metric is looked up by name
// once and then updated through a cached handle, so the hot path is an atomic
// add. Everything is process-wide and thread-safe; there is no per-script
// isolation because these describe the host, not a script.

#include "berry/export.h"
#include "berry/types.h"

#include <atomic>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Berry {

/// A monotonically increasing count.
class BERRY_API Counter {
 public:
  void Add(u64 n = 1) { value_.fetch_add(n, std::memory_order_relaxed); }
  u64 Value() const { return value_.load(std::memory_order_relaxed); }
 private:
  std::atomic<u64> value_{0};
};

/// A value that can go up and down (a queue depth, a live count).
class BERRY_API Gauge {
 public:
  void Set(i64 v) { value_.store(v, std::memory_order_relaxed); }
  void Add(i64 d) { value_.fetch_add(d, std::memory_order_relaxed); }
  i64 Value() const { return value_.load(std::memory_order_relaxed); }
 private:
  std::atomic<i64> value_{0};
};

/// A fixed-bucket histogram with a running count, sum and min/max, so a caller
/// can read a cheap summary without keeping every sample.
class BERRY_API Histogram {
 public:
  /// One bucket per bound plus an overflow bucket. `Buckets()` returns exactly
  /// this many; the array must be this size or the read runs off the end.
  static constexpr size_t kBuckets = 17;
  /// Bucket upper bounds (inclusive), in the metric's own unit (ms for frame
  /// time). A sample above the last bound lands in the overflow bucket.
  static const f64* Bounds();
  static size_t BoundCount();

  void Observe(f64 v);
  u64 Count() const { return count_.load(std::memory_order_relaxed); }
  f64 Sum() const { return sum_.load(std::memory_order_relaxed); }
  f64 Min() const { return min_.load(std::memory_order_relaxed); }
  f64 Max() const { return max_.load(std::memory_order_relaxed); }
  /// The bucket counts, `BoundCount()+1` of them (the last is overflow).
  std::vector<u64> Buckets() const;
  /// The mean, or 0 when nothing was observed.
  f64 Mean() const;

 private:
  std::atomic<u64> count_{0};
  std::atomic<f64> sum_{0};
  /// +inf until the first sample, so an all-positive series reports a real min.
  std::atomic<f64> min_{std::numeric_limits<f64>::infinity()};
  std::atomic<f64> max_{0};
  std::atomic<u64> buckets_[kBuckets]{};   // last is overflow
};

/// The registry: name -> metric. One process-wide instance.
class BERRY_API MetricsRegistry {
 public:
  static MetricsRegistry& Instance();

  /// Returns (creating on first use) a metric handle. The name is
  /// `subsystem.metric`, e.g. "frame.time_ms".
  Counter* CounterFor(const std::string& name);
  Gauge* GaugeFor(const std::string& name);
  Histogram* HistogramFor(const std::string& name);

  /// A snapshot for tooling: every counter/gauge as (name, value).
  std::vector<std::pair<std::string, i64>> Snapshot() const;
  /// Every histogram as a summary.
  struct HistogramSummary {
    std::string name;
    u64 count;
    f64 mean;
    f64 min;
    f64 max;
  };
  std::vector<HistogramSummary> HistogramSnapshot() const;

  /// Drops every metric (tests, or a hard reset).
  void Clear();
  size_t Count() const;

 private:
  MetricsRegistry() = default;
  mutable std::mutex mutex_;
  std::map<std::string, std::unique_ptr<Counter>> counters_;
  std::map<std::string, std::unique_ptr<Gauge>> gauges_;
  std::map<std::string, std::unique_ptr<Histogram>> histograms_;
};

}  // namespace Berry
