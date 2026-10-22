#include "berry/event.h"
#include "berry/logger.h"

namespace Berry {

//----------------------------------------------------------------------------
// EventBus
//----------------------------------------------------------------------------

EventBus& EventBus::Instance() {
  static EventBus eb;
  return eb;
}

EventBus::ListenerId EventBus::On(const char* channel, EventListener fn) {
  std::lock_guard<std::mutex> lk(mutex_);
  ListenerId id = nextId_++;
  channels_[channel].push_back(Entry{id, std::move(fn)});
  return id;
}

bool EventBus::Off(const char* channel, ListenerId id) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = channels_.find(channel);
  if (it == channels_.end()) return false;
  auto& vec = it->second;
  for (auto it2 = vec.begin(); it2 != vec.end(); ++it2) {
    if (it2->id == id) {
      vec.erase(it2);
      return true;
    }
  }
  return false;
}

void EventBus::Clear(const char* channel) {
  std::lock_guard<std::mutex> lk(mutex_);
  channels_.erase(channel);
}

bool EventBus::Emit(const char* channel, void* data, EventContext* out) {
  std::vector<Entry> snapshot;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = channels_.find(channel);
    if (it == channels_.end() || it->second.empty()) return true;
    snapshot = it->second;
  }

  EventContext ctx;
  ctx.id = ((u64)(size_t)channel) ^ 0x9E3779B97F4A7C15ULL;
  ctx.data = data;

  std::vector<Entry> remaining;
  remaining.reserve(snapshot.size());
  bool cancelled = false;
  for (auto& e : snapshot) {
    EventResult r = e.fn(ctx);
    if (r == EventResult::Cancel) {
      cancelled = true;
      break;
    }
    if (r == EventResult::StopPropagation) break;
  }
  BERRY_UNUSED(remaining);

  if (out) *out = ctx;
  if (ctx.cancelled) cancelled = true;
  return !cancelled;
}

bool EventBus::HasListeners(const char* channel) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = channels_.find(channel);
  return it != channels_.end() && !it->second.empty();
}

size_t EventBus::ListenerCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  size_t n = 0;
  for (auto& [k, v] : channels_) n += v.size();
  return n;
}

//----------------------------------------------------------------------------
// Scheduler
//----------------------------------------------------------------------------

void Scheduler::SetCurrentTick(GameTick tick) {
  std::lock_guard<std::mutex> lk(mutex_);
  currentTick_ = tick;
}

void Scheduler::After(GameTick delayTicks, std::function<void()> fn, const char* tag) {
  BERRY_UNUSED(tag);
  std::lock_guard<std::mutex> lk(mutex_);
  Task t;
  t.id = nextId_++;
  t.runAt = currentTick_ + (delayTicks > 0 ? delayTicks : 1);
  t.interval = 0;
  t.repeat = false;
  t.fn = [fn = std::move(fn)](GameTick) { fn(); };
  tasks_.push_back(std::move(t));
}

u64 Scheduler::Every(GameTick intervalTicks, std::function<void(GameTick)> fn) {
  std::lock_guard<std::mutex> lk(mutex_);
  Task t;
  t.id = nextId_++;
  t.runAt = currentTick_ + (intervalTicks > 0 ? intervalTicks : 1);
  t.interval = intervalTicks;
  t.repeat = true;
  t.fn = std::move(fn);
  tasks_.push_back(std::move(t));
  return t.id;
}

bool Scheduler::Cancel(u64 handle) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto& t : tasks_) {
    if (t.id == handle && !t.cancelled) {
      t.cancelled = true;
      return true;
    }
  }
  return false;
}

void Scheduler::Tick(GameTick currentTick) {
  std::vector<Task> due;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& t : tasks_) {
      if (t.cancelled) continue;
      if (!t.repeat && t.runAt <= currentTick) due.push_back(t);
      else if (t.interval > 0 && currentTick >= t.runAt &&
               ((currentTick - t.runAt + t.interval) % t.interval) == 0)
        due.push_back(t);
    }
    // Remove non-repeating due tasks.
    tasks_.erase(
        std::remove_if(tasks_.begin(), tasks_.end(),
                       [&](const Task& t) {
                         if (t.cancelled) return true;
                         if (!t.repeat && t.runAt <= currentTick) return true;
                         return false;
                       }),
        tasks_.end());
  }
  for (auto& task : due) {
    if (task.cancelled) continue;
    if (task.fn) task.fn(currentTick);
  }
}

size_t Scheduler::Pending() const {
  std::lock_guard<std::mutex> lk(mutex_);
  size_t n = 0;
  for (auto& t : tasks_) if (!t.cancelled) ++n;
  return n;
}

//----------------------------------------------------------------------------
// Profiler
//----------------------------------------------------------------------------

Profiler& Profiler::Instance() {
  static Profiler p;
  return p;
}

std::vector<Profiler::Agg*>& Profiler::TlsStack() {
  // One stack per thread: the render thread, the script worker and the game
  // tick each nest independently, and a shared stack would mis-parent a span
  // that merely happened to overlap in time.
  static thread_local std::vector<Agg*> stack;
  return stack;
}

Profiler::Agg* Profiler::ChildFor(Agg* parent, const std::string& name) {
  for (auto& c : parent->children) {
    if (c->name == name) return c.get();
  }
  parent->children.push_back(std::make_unique<Agg>());
  parent->children.back()->name = name;
  return parent->children.back().get();
}

u64 Profiler::Begin(const char* name) {
  if (!enabled_) return 0;
  const std::string spanName = name ? name : "";
  std::lock_guard<std::mutex> lk(mutex_);
  u64 token = nextId_++;
  // The span becomes a child of whatever is open on this thread right now. The
  // node is remembered in the span record, so End charges the right node even
  // if the matching Begin ran on a different thread.
  Agg* parent = TlsStack().empty() ? &root_ : TlsStack().back();
  Agg* node = ChildFor(parent, spanName);
  active_[token] = Span{spanName, NowNs(), node};
  TlsStack().push_back(node);
  return token;
}

void Profiler::End(u64 token) {
  if (!enabled_ || token == 0) return;
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = active_.find(token);
  if (it == active_.end()) return;
  u64 elapsed = NowNs() - it->second.startNs;
  f64 ms = (f64)elapsed / 1e6;
  auto& stats = stats_[it->second.name];
  stats.totalMs += ms;
  stats.count++;
  if (ms > stats.maxMs) stats.maxMs = ms;
  lastFrame_[it->second.name] = ms;

  // Charge the span to the node Begin opened. A cross-thread End pops nothing
  // from this thread's stack, so it cannot unbalance a stack it never pushed.
  Agg* node = (Agg*)it->second.node;
  auto& stack = TlsStack();
  if (!stack.empty() && stack.back() == node) stack.pop_back();
  if (node) {
    node->selfMs += ms;
    node->count++;
    if (!stack.empty()) stack.back()->selfMs -= ms;
  }
  active_.erase(it);
}

Profiler::SpanStats Profiler::Stats(const char* name) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = stats_.find(name ? name : "");
  if (it == stats_.end()) return {};
  return it->second;
}

void Profiler::CopyFlame(const Agg& src, FlameNode* out) {
  out->name = src.name;
  out->selfMs = src.selfMs;
  out->count = src.count;
  out->totalMs = src.selfMs;
  for (const auto& c : src.children) {
    FlameNode child;
    CopyFlame(*c, &child);
    out->totalMs += child.totalMs;
    out->children.push_back(std::move(child));
  }
  std::sort(out->children.begin(), out->children.end(),
            [](const FlameNode& a, const FlameNode& b) {
              if (a.totalMs != b.totalMs) return a.totalMs > b.totalMs;
              return a.name < b.name;
            });
}

Profiler::FlameNode Profiler::Flame() const {
  std::lock_guard<std::mutex> lk(mutex_);
  FlameNode out;
  CopyFlame(root_, &out);
  return out;
}

std::vector<std::pair<std::string, Profiler::SpanStats>> Profiler::Spans() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<std::pair<std::string, SpanStats>> out(stats_.begin(), stats_.end());
  std::sort(out.begin(), out.end(),
            [](const std::pair<std::string, SpanStats>& a,
               const std::pair<std::string, SpanStats>& b) {
              if (a.second.totalMs != b.second.totalMs) {
                return a.second.totalMs > b.second.totalMs;
              }
              return a.first < b.first;
            });
  return out;
}

void Profiler::FlameReset() {
  std::lock_guard<std::mutex> lk(mutex_);
  // The tree is only freed when no span is open: a live span holds a node
  // pointer, so clearing the tree under it would dangle. A reset during a frame
  // still drops the counters, and the tree is reclaimed on the next idle reset.
  if (active_.empty()) {
    root_.children.clear();
    root_.selfMs = 0;
    root_.count = 0;
    TlsStack().clear();
  }
  stats_.clear();
  lastFrame_.clear();
}

void Profiler::FrameClear() {
  std::lock_guard<std::mutex> lk(mutex_);
  lastFrame_.clear();
}

ProfilerScope::ProfilerScope(const char* name) {
  token_ = Profiler::Instance().Begin(name);
}
ProfilerScope::~ProfilerScope() {
  Profiler::Instance().End(token_);
}

}  // namespace Berry