#pragma once
// BerryAPI scripting runtime host (Phase 2).
//
// The runtime is built around a small, explicit seam so the scripting language
// is a pluggable choice rather than a hard dependency:
//
//   IScriptVm      — the language VM (Luau, QuickJS-ng, or the self-contained
//                    reference VM used by host tests).
//   ScriptBindings — the native functions scripts may call. Every binding
//                    declares the capability it requires, so a script cannot
//                    reach a subsystem it was not granted.
//   ScriptHost     — owns one VM + one script's grant, runs lifecycle hooks
//                    under hard budgets, and isolates failures.
//   ScriptWorker   — the dedicated script thread. Scripts NEVER run on the
//                    render thread (see docs/THREADING.md).
//
// Budgets are non-negotiable and enforced by the VM itself:
//   * instruction budget — a VM interrupt counter, so an infinite loop stops.
//   * memory limit       — the VM refuses an allocation that would exceed it.
//   * wall-clock timeout — a second line of defence for native calls.
//   * error isolation    — a faulting script is unloaded; the game continues.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/manifest.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Berry {

//----------------------------------------------------------------------------
// Values crossing the VM boundary.
//----------------------------------------------------------------------------

struct ScriptValue;

/// A scripting table: an ordered, string-keyed map, with reference semantics
/// (copies share the same table, exactly as a Luau table or a JS object would).
/// Ordered so display, equality-by-content and the hot-reload snapshot are
/// byte-stable.
using ScriptTable = std::map<std::string, ScriptValue>;

struct BERRY_API ScriptValue {
  enum class Type : u8 { Nil, Bool, Number, String, Table };

  Type type = Type::Nil;
  bool boolean = false;
  f64 number = 0;
  std::string string;
  /// Set when `type == Type::Table`. Shared so a table has reference
  /// semantics; null for every other type.
  std::shared_ptr<ScriptTable> table;

  ScriptValue() = default;
  static ScriptValue Nil() { return ScriptValue(); }
  static ScriptValue Bool(bool v);
  static ScriptValue Num(f64 v);
  static ScriptValue Str(std::string v);
  /// Wraps a table, taking ownership of its contents.
  static ScriptValue Tbl(ScriptTable t);

  bool IsNil() const { return type == Type::Nil; }
  bool IsBool() const { return type == Type::Bool; }
  bool IsNumber() const { return type == Type::Number; }
  bool IsString() const { return type == Type::String; }
  bool IsTable() const { return type == Type::Table; }

  /// The table payload, or null when this is not a table.
  const ScriptTable* AsTable() const {
    return type == Type::Table ? table.get() : nullptr;
  }

  /// Field lookup on a table. Returns null when this is not a table or the key
  /// is absent, so callers can fail closed.
  const ScriptValue* Field(const char* key) const;

  /// Truthiness: nil and false are false; 0 is false; "" is false. A table is
  /// always truthy (an empty table included), matching Luau.
  bool Truthy() const;
  f64 AsNumber() const;
  const std::string& AsString() const;

  /// Human-readable form used in error messages and the REPL.
  std::string ToDisplay() const;

  /// Equality. Tables compare by identity (like Luau tables), never by
  /// content, so a self-referential table cannot recurse.
  bool operator==(const ScriptValue& o) const;
  bool operator!=(const ScriptValue& o) const { return !(*this == o); }
};

//----------------------------------------------------------------------------
// Errors + status.
//----------------------------------------------------------------------------

struct BERRY_API ScriptError {
  bool occurred = false;
  std::string message;
  std::string chunk;      // chunk/source name where the error was raised
  i32 line = 0;

  static ScriptError None() { return ScriptError{}; }
  static ScriptError Make(std::string msg, std::string chunk = "", i32 line = 0);
};

/// Result of a script execution step. `Running` means the call is still in
/// progress (used by the resumable Step path); every other value is terminal
/// for the current step.
enum class ScriptStatus : u32 {
  Ok = 0,
  Running,           // call in progress, more Step() calls needed
  Yielded,           // the script suspended (await); resume later
  NotLoaded,
  NoFunction,
  Error,
  BudgetExceeded,
  MemoryExceeded,
  Timeout,
  Interrupted,
  CapabilityDenied,
  Unloaded,
};

const char* ScriptStatusName(ScriptStatus s);

//----------------------------------------------------------------------------
// Budgets.
//----------------------------------------------------------------------------

struct BERRY_API ScriptLimits {
  /// Maximum VM instructions a single hook invocation may execute.
  u64 instructionBudget = 200000;
  /// Maximum VM-managed bytes (strings + state) a script may hold.
  size_t memoryLimitBytes = 8u * 1024 * 1024;
  /// Wall-clock guard for native calls inside one invocation.
  u64 wallClockMs = 100;
};

//----------------------------------------------------------------------------
// Capability-gated native bindings.
//----------------------------------------------------------------------------

class ScriptContext;

/// A native function exposed to scripts. Returning via `ctx` may request a
/// yield (see ScriptContext::RequestYield) for the async layer.
using ScriptNativeFn =
    std::function<ScriptValue(ScriptContext& ctx, const std::vector<ScriptValue>& args)>;

/// Registry of native bindings. Each binding names the capability it needs;
/// invoking a binding without the grant fails closed with
/// ScriptStatus::CapabilityDenied.
class BERRY_API ScriptBindings {
 public:
  void Register(const std::string& name, const Capability& required,
                ScriptNativeFn fn);
  bool Has(const std::string& name) const;
  /// Capability a binding requires; Unknown when the binding is absent.
  Capability Required(const std::string& name) const;

  /// Invokes a binding. On success sets *ok = true. When the capability is not
  /// granted, sets *ok = false and *denied = true (fail closed).
  ScriptValue Invoke(ScriptContext& ctx, const std::string& name,
                     const std::vector<ScriptValue>& args, bool* ok,
                     bool* denied) const;

  size_t Count() const;
  void Clear();

  /// Registered binding names, in registration order. Used by a VM that must
  /// expose the bindings to scripts under their canonical names.
  std::vector<std::string> Names() const;

 private:
  struct Entry {
    Capability required;
    ScriptNativeFn fn;
  };
  std::vector<std::pair<std::string, Entry>> entries_;
};

/// Execution context handed to native bindings: which script is running, what
/// it was granted, and the yield channel the async layer uses.
class BERRY_API ScriptContext {
 public:
  ScriptContext(const std::string& scriptName, const CapabilitySet& granted)
      : scriptName_(scriptName), granted_(granted) {}

  const std::string& scriptName() const { return scriptName_; }
  const CapabilitySet& granted() const { return granted_; }
  bool CanUse(const Capability& cap) const { return granted_.CoversCap(cap); }

  /// True when the script asked the host to suspend this invocation. The async
  /// layer sets a resume token; the plain runtime treats it as a hard stop.
  bool yieldRequested() const { return yieldRequested_; }
  void RequestYield(const std::string& reason) {
    yieldRequested_ = true;
    yieldReason_ = reason;
  }
  void ClearYield() {
    yieldRequested_ = false;
    yieldReason_.clear();
  }
  const std::string& yieldReason() const { return yieldReason_; }

  /// True when a binding is being re-invoked after a yield. Bindings use this
  /// to distinguish "start the operation" from "check if it finished".
  bool resuming() const { return resuming_; }
  void SetResuming(bool r) { resuming_ = r; }

  /// Async task the current binding belongs to (set by the async runtime on
  /// first invocation, read back on resume).
  u64 taskId() const { return taskId_; }
  void SetTaskId(u64 id) { taskId_ = id; }

  /// Raises a script error from inside a binding. The VM turns a set error
  /// into a script failure when the binding returns.
  void RaiseError(std::string message) {
    hasError_ = true;
    errorMessage_ = std::move(message);
  }
  bool hasError() const { return hasError_; }
  const std::string& errorMessage() const { return errorMessage_; }
  void ClearError() {
    hasError_ = false;
    errorMessage_.clear();
  }

  /// Host-owned scratch value slots, keyed by name (REPL/inspector support).
  void SetScratch(const std::string& key, ScriptValue v);
  bool GetScratch(const std::string& key, ScriptValue* out) const;

 private:
  std::string scriptName_;
  CapabilitySet granted_;
  bool yieldRequested_ = false;
  std::string yieldReason_;
  bool resuming_ = false;
  u64 taskId_ = 0;
  bool hasError_ = false;
  std::string errorMessage_;
  std::vector<std::pair<std::string, ScriptValue>> scratch_;
};

//----------------------------------------------------------------------------
// The VM seam.
//----------------------------------------------------------------------------

class BERRY_API IScriptVm {
 public:
  virtual ~IScriptVm() = default;

  virtual const char* Name() const = 0;

  /// Compiles + loads a chunk. Replaces any previously loaded chunk. Fails
  /// closed: a compile error leaves the VM unloaded.
  virtual bool Load(const std::string& source, const std::string& chunkName,
                    ScriptError* err) = 0;
  virtual void Unload() = 0;
  virtual bool loaded() const = 0;
  virtual bool HasFunction(const std::string& name) const = 0;

  /// Begins a call to `name`. Returns NoFunction when absent.
  virtual ScriptStatus Start(const std::string& name,
                             const std::vector<ScriptValue>& args,
                             ScriptError* err) = 0;
  /// Advances the current call by up to `budget` instructions. Returns Running
  /// while the call is unfinished, otherwise the terminal status.
  virtual ScriptStatus Step(u64 budget, ScriptError* err) = 0;
  /// True while a call started by Start() is still in progress.
  virtual bool inCall() const = 0;
  /// True when the VM is suspended at a yield and can be resumed.
  virtual bool suspended() const = 0;
  /// Resumes a call suspended by a yield, substituting `value` for the
  /// suspended binding's return value. Returns false when not suspended.
  virtual bool Resume(const ScriptValue& value, ScriptError* err) = 0;
  /// Return values of the last completed call.
  virtual const std::vector<ScriptValue>& Result() const = 0;
  /// Cancels any in-progress call (used by detach / hard stop).
  virtual void Abort() = 0;

  /// Convenience: Start + Step to completion under the VM's own budget.
  ScriptStatus Call(const std::string& name,
                    const std::vector<ScriptValue>& args,
                    std::vector<ScriptValue>* ret, ScriptError* err);

  virtual void SetLimits(const ScriptLimits& limits) = 0;
  virtual const ScriptLimits& Limits() const = 0;

  virtual void SetBindings(const ScriptBindings* bindings) = 0;
  virtual void SetContext(ScriptContext* ctx) = 0;

  virtual u64 InstructionsExecuted() const = 0;
  virtual size_t MemoryUsed() const = 0;
  /// Resets the per-invocation instruction counter.
  virtual void ResetInstructionCount() = 0;

  /// Cooperative interrupt: the VM stops at the next instruction boundary.
  virtual void RequestInterrupt() = 0;
  virtual bool interrupted() const = 0;
  virtual void ClearInterrupt() = 0;

  /// Deterministic serialization of script-visible state (hot reload).
  virtual std::string SerializeState() const = 0;
  virtual bool RestoreState(const std::string& state, ScriptError* err) = 0;
};

/// Creates the self-contained reference VM (the host-test backend). It speaks
/// a tiny stack-assembler dialect documented in core/script/vm_ref.cpp and
/// exists so the whole runtime is exercisable with no game and no third-party
/// engine. On device, Luau/QuickJS-ng are plugged in behind this same seam.
BERRY_API IScriptVm* CreateReferenceVm();

/// Creates the Luau VM (the device backend). It speaks real Luau: scripts
/// return a table of exported functions, bindings are exposed as globals under
/// their IDL names (render.draw_text), and budgets are enforced through Luau's
/// safepoint interrupt plus a memory-capping allocator. Sandboxed: no require,
/// dofile, loadstring, os filesystem/process calls or debug library.
BERRY_API IScriptVm* CreateLuauVm();

//----------------------------------------------------------------------------
// Script host — one script, one VM, one grant.
//----------------------------------------------------------------------------

enum class ScriptHostState : u32 {
  Idle = 0,
  Loaded,
  Faulted,     // a hook failed; the host refuses further calls until reload
  Unloaded,
};

const char* ScriptHostStateName(ScriptHostState s);

/// Lifecycle hooks the host looks for in a script.
namespace ScriptHooks {
inline const char* OnLoad() { return "on_load"; }
inline const char* OnTick() { return "on_tick"; }
inline const char* OnFrame() { return "on_frame"; }
inline const char* OnEvent() { return "on_event"; }
inline const char* OnUnload() { return "on_unload"; }
}  // namespace ScriptHooks

class BERRY_API ScriptHost {
 public:
  ScriptHost(std::string name, const CapabilitySet& granted,
             std::unique_ptr<IScriptVm> vm);
  ~ScriptHost();

  ScriptHost(const ScriptHost&) = delete;
  ScriptHost& operator=(const ScriptHost&) = delete;

  const std::string& name() const { return name_; }
  const CapabilitySet& granted() const { return granted_; }
  ScriptHostState state() const { return state_; }
  IScriptVm* vm() { return vm_.get(); }

  void SetLimits(const ScriptLimits& limits);
  const ScriptLimits& Limits() const;

  /// Loads source into the VM. On failure the host is Unloaded and the error
  /// is reported. Does NOT run on_load; call RunHook for that.
  bool Load(const std::string& source, const std::string& chunkName,
            ScriptError* err);

  /// Runs a lifecycle hook under the host's budgets. Any error, budget
  /// violation or interrupt faults the host (error isolation) and is reported
  /// in `status`.
  ScriptStatus RunHook(const std::string& hook,
                       const std::vector<ScriptValue>& args = {},
                       std::vector<ScriptValue>* ret = nullptr);

  /// Invokes a named native binding through the capability gate.
  ScriptStatus CallBinding(const std::string& binding,
                           const std::vector<ScriptValue>& args,
                           ScriptValue* ret);

  /// Resumes a VM suspended at an await (ScriptStatus::Yielded) with the
  /// awaited value and drives it to completion or the next await. Budgets and
  /// error isolation apply exactly as in RunHook.
  ScriptStatus ResumeFromYield(const ScriptValue& value,
                               std::vector<ScriptValue>* ret = nullptr);

  /// True when a hook is suspended at an await and can be resumed.
  bool suspended() const;

  /// Faults the host from outside a hook (async timeout, transport error).
  /// Applies error isolation exactly like a hook failure.
  ScriptStatus Fault(const ScriptError& err);

  /// Faults the host: aborts any call, unloads the VM. Idempotent.
  void Unload();
  bool loaded() const { return state_ == ScriptHostState::Loaded; }

  ScriptContext& context() { return ctx_; }
  const ScriptError& lastError() const { return lastError_; }
  u64 Invocations() const { return invocations_; }
  u64 Faults() const { return faults_; }

  /// Sets the bindings this host exposes (shared, not owned).
  void SetBindings(const ScriptBindings* bindings);

  /// Hot-reload support: snapshot/restore script-visible state (Phase 3).
  std::string SerializeState() const;
  bool RestoreState(const std::string& state, ScriptError* err);

 private:
  ScriptStatus Fail(ScriptStatus status, const ScriptError& err);
  ScriptStatus Drive(ScriptStatus initial, const std::string& hook,
                     std::vector<ScriptValue>* ret,
                     const ScriptError& initialErr);

  std::string name_;
  CapabilitySet granted_;
  std::unique_ptr<IScriptVm> vm_;
  ScriptBindings ownedBindings_;   // used when no external bindings are set
  const ScriptBindings* bindings_ = nullptr;
  ScriptContext ctx_;
  ScriptHostState state_ = ScriptHostState::Idle;
  ScriptLimits limits_;
  ScriptError lastError_;
  u64 invocations_ = 0;
  u64 faults_ = 0;
};

//----------------------------------------------------------------------------
// Script worker — the dedicated script thread.
//----------------------------------------------------------------------------

/// Serialized execution queue for script work. Everything runs on one worker
/// thread stamped ThreadRole::Scripts, so scripts never touch the render
/// thread and never run concurrently with each other (no data races in the
/// VM). Blocking native calls are allowed here; they are not on the render
/// thread.
class BERRY_API ScriptWorker {
 public:
  ScriptWorker();
  ~ScriptWorker();

  ScriptWorker(const ScriptWorker&) = delete;
  ScriptWorker& operator=(const ScriptWorker&) = delete;

  /// Starts the worker thread. Idempotent.
  void Start();
  /// Drains queued tasks, then stops. Idempotent.
  void Stop();

  bool running() const { return running_.load(); }

  /// Queues a task. Returns false when the worker is stopped (fail closed:
  /// work is never silently dropped onto the caller's thread).
  bool Post(std::function<void()> task);

  /// Runs all currently-queued tasks on the calling thread. Host tests use
  /// this to drive the worker deterministically instead of racing a thread.
  size_t DrainOnCaller();

  /// Tasks executed since construction (diagnostics).
  u64 Executed() const { return executed_.load(); }
  size_t Pending() const;

 private:
  void Loop();

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopRequested_{false};
  std::atomic<u64> executed_{0};
};

//----------------------------------------------------------------------------
// Timeslice guard (see docs/THREADING.md — ScopedScriptTimeslice).
//----------------------------------------------------------------------------

/// RAII guard for one script callback on the script worker. Asserts the thread
/// role and enforces a wall-clock ceiling by requesting a VM interrupt when
/// the deadline passes.
class BERRY_API ScopedScriptTimeslice {
 public:
  ScopedScriptTimeslice(IScriptVm* vm, u64 maxMs);
  ~ScopedScriptTimeslice();

  ScopedScriptTimeslice(const ScopedScriptTimeslice&) = delete;
  ScopedScriptTimeslice& operator=(const ScopedScriptTimeslice&) = delete;

  /// True when the wall-clock ceiling has been passed.
  bool expired() const;
  u64 elapsedMs() const;

 private:
  IScriptVm* vm_ = nullptr;
  u64 startNs_ = 0;
  u64 deadlineNs_ = 0;
};

}  // namespace Berry
