// ScriptHost + ScriptWorker. See include/berry/script.h.
#include "berry/script.h"
#include "berry/event.h"    // NowNs
#include "berry/logger.h"
#include "berry/thread.h"

#include <chrono>

namespace Berry {

//----------------------------------------------------------------------------
// ScriptHost
//----------------------------------------------------------------------------

ScriptHost::ScriptHost(std::string name, const CapabilitySet& granted,
                       std::unique_ptr<IScriptVm> vm)
    : name_(std::move(name)),
      granted_(granted),
      vm_(std::move(vm)),
      ctx_(name_, granted) {
  if (!vm_) vm_.reset(CreateReferenceVm());
  vm_->SetBindings(&ownedBindings_);
  vm_->SetContext(&ctx_);
}

ScriptHost::~ScriptHost() {
  if (vm_ && vm_->inCall()) vm_->Abort();
}

void ScriptHost::SetLimits(const ScriptLimits& limits) {
  limits_ = limits;
  if (vm_) vm_->SetLimits(limits);
}

const ScriptLimits& ScriptHost::Limits() const {
  if (vm_) return vm_->Limits();
  return limits_;
}

void ScriptHost::SetBindings(const ScriptBindings* bindings) {
  bindings_ = bindings;
  if (vm_) vm_->SetBindings(bindings ? bindings : &ownedBindings_);
}

ScriptStatus ScriptHost::Fail(ScriptStatus status, const ScriptError& err) {
  lastError_ = err;
  ++faults_;
  // Error isolation: a faulting script is unloaded so it cannot keep running
  // in a corrupted state. The game keeps going.
  if (vm_) {
    vm_->Abort();
    vm_->Unload();
  }
  state_ = ScriptHostState::Unloaded;
  Log::Error("ScriptHost[%s]: %s (%s)", name_.c_str(), ScriptStatusName(status),
             err.message.c_str());
  return status;
}

bool ScriptHost::Load(const std::string& source, const std::string& chunkName,
                      ScriptError* err) {
  if (!vm_) {
    if (err) *err = ScriptError::Make("no VM");
    return false;
  }
  ScriptError local;
  if (!vm_->Load(source, chunkName, &local)) {
    lastError_ = local;
    state_ = ScriptHostState::Unloaded;
    ++faults_;
    Log::Error("ScriptHost[%s]: load failed: %s", name_.c_str(),
               local.message.c_str());
    if (err) *err = local;
    return false;
  }
  vm_->SetLimits(limits_);
  vm_->SetBindings(bindings_ ? bindings_ : &ownedBindings_);
  vm_->SetContext(&ctx_);
  vm_->ResetInstructionCount();
  vm_->ClearInterrupt();
  state_ = ScriptHostState::Loaded;
  if (err) *err = ScriptError::None();
  return true;
}

ScriptStatus ScriptHost::RunHook(const std::string& hook,
                                 const std::vector<ScriptValue>& args,
                                 std::vector<ScriptValue>* ret) {
  if (state_ != ScriptHostState::Loaded) return ScriptStatus::NotLoaded;
  if (!vm_ || !vm_->loaded()) return ScriptStatus::NotLoaded;
  if (!vm_->HasFunction(hook)) {
    // A missing hook is not an error: most scripts implement only a subset.
    if (ret) ret->clear();
    return ScriptStatus::NoFunction;
  }

  ++invocations_;
  vm_->ResetInstructionCount();
  vm_->ClearInterrupt();
  ScriptError startErr;
  return Drive(vm_->Start(hook, args, &startErr), hook, ret, startErr);
}

ScriptStatus ScriptHost::ResumeFromYield(const ScriptValue& value,
                                         std::vector<ScriptValue>* ret) {
  if (state_ != ScriptHostState::Loaded) return ScriptStatus::NotLoaded;
  if (!vm_ || !vm_->suspended()) return ScriptStatus::NotLoaded;
  vm_->ResetInstructionCount();
  vm_->ClearInterrupt();
  ScriptError err;
  if (!vm_->Resume(value, &err)) {
    return Fail(ScriptStatus::Error,
                err.occurred ? err : ScriptError::Make("resume failed"));
  }
  return Drive(ScriptStatus::Running, "<resume>", ret, ScriptError::None());
}

bool ScriptHost::suspended() const { return vm_ && vm_->suspended(); }

ScriptStatus ScriptHost::Fault(const ScriptError& err) {
  return Fail(ScriptStatus::Error, err);
}

/// Shared driver: steps the VM under the wall-clock guard and maps a terminal
/// status. `initial` is the status Start()/Resume() returned.
ScriptStatus ScriptHost::Drive(ScriptStatus initial, const std::string& hook,
                               std::vector<ScriptValue>* ret,
                               const ScriptError& initialErr) {
  ScriptStatus st = initial;
  ScriptError err;
  // Wall-clock guard around the whole invocation. If it trips we request a VM
  // interrupt so the interpreter stops at the next instruction boundary.
  const u64 deadline = NowNs() + limits_.wallClockMs * 1000000ULL;
  while (st == ScriptStatus::Running) {
    if (!vm_->inCall()) break;
    st = vm_->Step(4096, &err);
    if (st == ScriptStatus::Running && NowNs() >= deadline) {
      vm_->RequestInterrupt();
      ScriptError te = ScriptError::Make("wall-clock timeout", hook, 0);
      return Fail(ScriptStatus::Timeout, te);
    }
  }
  if (st == ScriptStatus::Ok) {
    if (ret) *ret = vm_->Result();
    return ScriptStatus::Ok;
  }
  if (st == ScriptStatus::Yielded) {
    // A suspend is not a fault: leave the host loaded and let the async layer
    // resume it. The VM keeps its state.
    return ScriptStatus::Yielded;
  }
  if (st == ScriptStatus::NoFunction) return st;
  if (st == ScriptStatus::Running) st = ScriptStatus::Error;
  return Fail(st, err.occurred ? err : initialErr);
}

ScriptStatus ScriptHost::CallBinding(const std::string& binding,
                                     const std::vector<ScriptValue>& args,
                                     ScriptValue* ret) {
  const ScriptBindings* b = bindings_ ? bindings_ : &ownedBindings_;
  if (!b) return ScriptStatus::Error;
  bool ok = false, denied = false;
  // A stale error from a previous call must not fail this one.
  if (ctx_.hasError()) ctx_.ClearError();
  ScriptValue v = b->Invoke(ctx_, binding, args, &ok, &denied);
  if (!ok) {
    ScriptError err = ScriptError::Make(
        denied ? ("capability denied for binding: " + binding)
               : ("no such binding: " + binding));
    return Fail(denied ? ScriptStatus::CapabilityDenied : ScriptStatus::Error, err);
  }
  // A binding that raised a script error (bad argument, service unavailable)
  // fails the call, exactly as it does when invoked from script code through
  // the VM's DoNative path.
  if (ctx_.hasError()) {
    ScriptError err = ScriptError::Make(ctx_.errorMessage());
    ctx_.ClearError();
    return Fail(ScriptStatus::Error, err);
  }
  if (ret) *ret = v;
  return ScriptStatus::Ok;
}

void ScriptHost::Unload() {
  if (vm_) {
    vm_->Abort();
    vm_->Unload();
  }
  state_ = ScriptHostState::Unloaded;
}

std::string ScriptHost::SerializeState() const {
  if (!vm_) return {};
  return vm_->SerializeState();
}

bool ScriptHost::RestoreState(const std::string& state, ScriptError* err) {
  if (!vm_) {
    if (err) *err = ScriptError::Make("no VM");
    return false;
  }
  return vm_->RestoreState(state, err);
}

//----------------------------------------------------------------------------
// ScriptWorker
//----------------------------------------------------------------------------

ScriptWorker::ScriptWorker() = default;

ScriptWorker::~ScriptWorker() { Stop(); }

void ScriptWorker::Start() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (running_.load()) return;
  stopRequested_.store(false);
  running_.store(true);
  thread_ = std::thread([this]() { Loop(); });
}

void ScriptWorker::Stop() {
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!running_.load() && !thread_.joinable()) return;
    stopRequested_.store(true);
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  running_.store(false);
}

bool ScriptWorker::Post(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!running_.load()) return false;   // fail closed: never run on caller
    queue_.push_back(std::move(task));
  }
  cv_.notify_one();
  return true;
}

size_t ScriptWorker::DrainOnCaller() {
  // Deterministic drain for host tests: no thread race, same queue semantics.
  std::deque<std::function<void()>> local;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    local.swap(queue_);
  }
  size_t n = 0;
  for (auto& task : local) {
    if (task) task();
    ++n;
    executed_.fetch_add(1);
  }
  return n;
}

size_t ScriptWorker::Pending() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return queue_.size();
}

void ScriptWorker::Loop() {
  // Scripts always run stamped as the script thread so ThreadModel assertions
  // and ScopedScriptTimeslice behave identically on device and host.
  ThreadModel::Instance().Enter(ThreadRole::Scripts);
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lk(mutex_);
      cv_.wait(lk, [this]() { return stopRequested_.load() || !queue_.empty(); });
      if (queue_.empty() && stopRequested_.load()) break;
      if (queue_.empty()) continue;
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    if (task) {
      task();
      executed_.fetch_add(1);
    }
  }
  // Drain remaining work before exiting so Stop() never drops a queued task.
  for (;;) {
    std::function<void()> task;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      if (queue_.empty()) break;
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    if (task) {
      task();
      executed_.fetch_add(1);
    }
  }
  ThreadModel::Instance().Leave();
}

}  // namespace Berry
