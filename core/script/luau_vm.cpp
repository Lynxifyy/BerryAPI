// Luau VM adapter. See include/berry/script.h for the IScriptVm contract.
//
// Luau is the language api/berry.idl already describes, and it supplies the
// three things the runtime layer needs: a sandbox (luaL_sandbox), a safepoint
// interrupt callback (lua_Callbacks::interrupt) and native coroutines.
//
// Execution model
// ---------------
// A script chunk returns a table of its exported functions. Each hook call runs
// on a fresh Luau coroutine that pulls the function out of the (registry-rooted)
// exports table and resumes it, so a yield inside a binding is legal and a hook
// call is an ordinary Luau call.
//
// Step / budgets
// --------------
// Luau runs a call to completion (or suspension) inside one lua_resume; there is
// no single-instruction step API exposed to embedders. Budgets are enforced at
// safepoints through the interrupt callback: it counts VM instructions and
// wall-clock time and raises a Luau error when a limit is crossed. A memory cap
// is enforced by the allocator, which refuses an allocation that would exceed
// the limit. Each abort maps back to ScriptStatus::BudgetExceeded / Timeout /
// MemoryExceeded.
//
// Bindings
// --------
// A binding named "render.draw_text" is exposed to scripts both as the global
// `render.draw_text` and as the nested call `render.draw_text(...)`, matching
// the IDL and the generated .luau types. (Suspending a binding for the async
// layer is added in Phase D.)
//
// Sandbox
// -------
// The dangerous base/os helpers are removed before scripts can see them and the
// global table is made read-only after bindings are installed: no require,
// dofile/loadfile/loadstring, setfenv/getfenv, raw mutation, os process or
// filesystem calls, or debug library.

#include "berry/script.h"
#include "berry/logger.h"
#include "berry/value_codec.h"

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace Berry {

class LuauVm;

static u64 SteadyMs() {
  using namespace std::chrono;
  return (u64)duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
      .count();
}

/// Per-coroutine bookkeeping, reached from the interrupt callback (which only
/// receives a lua_State*) via lua_getthreaddata. One instance serves every
/// coroutine of a VM: only one runs at a time.
struct VmContext {
  LuauVm* vm = nullptr;
  u64 startMs = 0;
  bool aborting = false;
};

/// Hand-off of the abort reason from the interrupt callback to the status
/// mapping. A safepoint runs on the executing thread, so a thread_local is safe.
struct AbortInfo {
  ScriptStatus status = ScriptStatus::Ok;
  std::string message;
};
thread_local AbortInfo g_abort;

static void PushString(lua_State* L, const std::string& s) {
  lua_pushlstring(L, s.data(), s.size());
}

static std::string ToString(lua_State* L, int idx) {
  size_t n = 0;
  const char* p = lua_tolstring(L, idx, &n);
  return p ? std::string(p, n) : std::string();
}

static void SplitName(const std::string& name, std::string* ns, std::string* fn) {
  auto dot = name.find('.');
  if (dot == std::string::npos) {
    ns->clear();
    *fn = name;
  } else {
    *ns = name.substr(0, dot);
    *fn = name.substr(dot + 1);
  }
}

//----------------------------------------------------------------------------
// Value marshalling.
//----------------------------------------------------------------------------

static ScriptValue FromLua(lua_State* L, int idx, int depth,
                    std::unordered_set<const void*>* seen) {
  switch (lua_type(L, idx)) {
    case LUA_TNIL:
      return ScriptValue::Nil();
    case LUA_TBOOLEAN:
      return ScriptValue::Bool(lua_toboolean(L, idx) != 0);
    case LUA_TNUMBER:
      return ScriptValue::Num((f64)lua_tonumber(L, idx));
    case LUA_TSTRING:
      return ScriptValue::Str(ToString(L, idx));
    case LUA_TTABLE: {
      // Bound depth and table identity so a hostile table cannot recurse
      // without bound; a repeated table folds to nil.
      if (depth > 32) return ScriptValue::Nil();
      const void* id = lua_topointer(L, idx);
      if (id && seen->count(id)) return ScriptValue::Nil();
      if (id) seen->insert(id);
      idx = lua_absindex(L, idx);
      ScriptTable out;
      lua_pushnil(L);
      while (lua_next(L, idx) != 0) {
        std::string key;
        if (lua_type(L, -2) == LUA_TSTRING) {
          key = ToString(L, -2);
        } else if (lua_type(L, -2) == LUA_TNUMBER) {
          key = std::to_string((long long)lua_tonumber(L, -2));
        }
        if (!key.empty()) out[key] = FromLua(L, -1, depth + 1, seen);
        lua_pop(L, 1);
      }
      if (id) seen->erase(id);
      return ScriptValue::Tbl(std::move(out));
    }
    default:
      // Functions, threads, userdata, vector/buffer are outside the boundary.
      return ScriptValue::Nil();
  }
}

static void PushScriptValue(lua_State* L, const ScriptValue& v, int depth) {
  if (depth > 32) {
    lua_pushnil(L);
    return;
  }
  switch (v.type) {
    case ScriptValue::Type::Nil:
      lua_pushnil(L);
      break;
    case ScriptValue::Type::Bool:
      lua_pushboolean(L, v.boolean ? 1 : 0);
      break;
    case ScriptValue::Type::Number:
      lua_pushnumber(L, v.number);
      break;
    case ScriptValue::Type::String:
      PushString(L, v.string);
      break;
    case ScriptValue::Type::Table: {
      lua_createtable(L, 0, 0);
      const ScriptTable* t = v.AsTable();
      if (t) {
        for (const auto& kv : *t) {
          PushString(L, kv.first);
          PushScriptValue(L, kv.second, depth + 1);
          lua_settable(L, -3);
        }
      }
      break;
    }
  }
}

//----------------------------------------------------------------------------
// LuauVm
//----------------------------------------------------------------------------

class LuauVm final : public IScriptVm {
 public:
  LuauVm() {
    L_ = lua_newstate(&LuauVm::Alloc, this);
    if (!L_) return;
    lua_callbacks(L_)->interrupt = &Interrupt;
    luaL_openlibs(L_);
    StripDangerous();
    Sandbox();
  }

  ~LuauVm() override {
    if (L_) lua_close(L_);   // frees the bound closures first
    for (BoundFn* bf : boundFns_) delete bf;
  }

  const char* Name() const override { return "berry-luau"; }

  //-- lifecycle ------------------------------------------------------------

  bool Load(const std::string& source, const std::string& chunkName,
            ScriptError* err) override {
    Unload();
    if (!L_) {
      if (err) *err = ScriptError::Make("luau state unavailable", chunkName, 0);
      return false;
    }

    size_t bcSize = 0;
    char* bc = luau_compile(source.data(), source.size(), nullptr, &bcSize);
    if (!bc) {
      if (err) *err = ScriptError::Make("luau compile failed", chunkName, 0);
      return false;
    }
    // Run the chunk on a throwaway coroutine to obtain its exports table. The
    // chunk's globals remain the shared global table, which the script sees on
    // every later call.
    lua_State* loader = lua_newthread(L_);
    int status = luau_load(loader, chunkName.c_str(), bc, bcSize, 0);
    std::free(bc);
    if (status != 0) {
      if (err) *err = ScriptError::Make(ToString(loader, -1), chunkName, 0);
      lua_pop(L_, 1);   // drop the loader coroutine
      return false;
    }

    chunk_ = chunkName;
    lua_setthreaddata(loader, &vmctx_);
    vmctx_.vm = this;
    vmctx_.startMs = SteadyMs();
    vmctx_.aborting = false;
    instructions_ = 0;
    interrupt_ = false;

    ScriptError cerr;
    if (!MapResume(lua_resume(loader, nullptr, 0), &cerr)) {
      if (err) *err = cerr;
      lua_pop(L_, 1);
      return false;
    }
    if (!lua_istable(loader, -1)) {
      if (err) {
        *err = ScriptError::Make("script must return a table of functions",
                                 chunkName, 0);
      }
      lua_pop(L_, 1);
      return false;
    }
    lua_pushvalue(loader, -1);
    lua_xmove(loader, L_, 1);          // exports table onto L_'s stack
    exportsRef_ = lua_ref(L_, -1);     // root it in the registry
    lua_pop(L_, 1);                    // drop the table copy
    lua_pop(L_, 1);                    // drop the loader coroutine
    loaded_ = true;
    return true;
  }

  void Unload() override {
    DropCall();
    if (L_ && exportsRef_ != LUA_REFNIL) lua_unref(L_, exportsRef_);
    exportsRef_ = LUA_REFNIL;
    inCall_ = false;
    suspended_ = false;
    instructions_ = 0;
    interrupt_ = false;
    loaded_ = false;
    result_.clear();
    lastError_ = ScriptError::None();
    status_ = ScriptStatus::Ok;
  }

  bool loaded() const override { return loaded_; }

  bool HasFunction(const std::string& name) const override {
    if (!loaded_ || !L_) return false;
    lua_State* t = lua_newthread(L_);
    lua_getref(t, exportsRef_);
    const bool ok = PushFunction(t, name);
    lua_pop(L_, 1);   // drop the scratch coroutine
    return ok;
  }

  //-- invocation -----------------------------------------------------------

  ScriptStatus Start(const std::string& name,
                     const std::vector<ScriptValue>& args,
                     ScriptError* err) override {
    if (!loaded_ || !L_) return ScriptStatus::NotLoaded;
    result_.clear();
    lastError_ = ScriptError::None();

    call_ = lua_newthread(L_);
    lua_getref(call_, exportsRef_);         // exports table at index 1
    if (!PushFunction(call_, name)) {       // function at index 2
      if (err) *err = ScriptError::Make("no such function: " + name, chunk_, 0);
      DropCall();
      return ScriptStatus::NoFunction;
    }
    for (const auto& a : args) PushScriptValue(call_, a, 0);

    lua_setthreaddata(call_, &vmctx_);
    vmctx_.vm = this;
    vmctx_.startMs = SteadyMs();
    vmctx_.aborting = false;
    instructions_ = 0;
    interrupt_ = false;
    inCall_ = true;
    suspended_ = false;

    ScriptStatus st = Categorize(lua_resume(call_, nullptr, (int)args.size()));
    if (st != ScriptStatus::Ok && st != ScriptStatus::Yielded && err) {
      *err = lastError_;
    }
    return st;
  }

  ScriptStatus Step(u64 /*budget*/, ScriptError* err) override {
    // Luau produces its terminal status inside Start/Resume; Step reports it so
    // the host's resume path (which drives through Step) sees the real outcome.
    if (err) *err = lastError_;
    return status_;
  }

  bool inCall() const override { return inCall_; }
  bool suspended() const override { return suspended_; }

  bool Resume(const ScriptValue& value, ScriptError* err) override {
    (void)err;
    if (!suspended_ || !call_) return false;
    suspended_ = false;
    if (ctx_) ctx_->ClearYield();
    // The resume value is delivered as the result of the suspended binding.
    lua_settop(call_, 0);
    PushScriptValue(call_, value, 0);
    vmctx_.startMs = SteadyMs();
    vmctx_.aborting = false;
    instructions_ = 0;
    inCall_ = true;
    Categorize(lua_resume(call_, nullptr, 1));
    // The host resumes through its Step-driven loop, so leave the call open for
    // one Step() to report the status Categorize just computed.
    inCall_ = true;
    return true;
  }

  const std::vector<ScriptValue>& Result() const override { return result_; }

  void Abort() override {
    DropCall();
    inCall_ = false;
    suspended_ = false;
  }

  void SetLimits(const ScriptLimits& limits) override { limits_ = limits; }
  const ScriptLimits& Limits() const override { return limits_; }

  void SetBindings(const ScriptBindings* bindings) override {
    bindings_ = bindings;
    if (bindings_) InstallBindingGlobals();
  }
  void SetContext(ScriptContext* ctx) override { ctx_ = ctx; }

  u64 InstructionsExecuted() const override { return instructions_; }
  size_t MemoryUsed() const override { return liveBytes_; }

  void ResetInstructionCount() override {
    instructions_ = 0;
    vmctx_.startMs = SteadyMs();
  }

  void RequestInterrupt() override { interrupt_ = true; }
  bool interrupted() const override { return interrupt_; }
  void ClearInterrupt() override { interrupt_ = false; }

  //-- state handoff --------------------------------------------------------

  std::string SerializeState() const override {
    std::string out = "berry-luau-state-v1\n";
    if (!loaded_ || !L_) return out;
    lua_State* t = lua_newthread(L_);
    lua_getref(t, exportsRef_);
    lua_getfield(t, -1, "__state");
    ScriptValue v = FromLua(t, -1, 0, &seen());
    lua_pop(L_, 1);
    out += ValueCodec::Encode(v);
    return out;
  }

  bool RestoreState(const std::string& state, ScriptError* err) override {
    const std::string header = "berry-luau-state-v1\n";
    if (state.compare(0, header.size(), header) != 0) {
      if (err) *err = ScriptError::Make("bad state header", chunk_, 0);
      return false;
    }
    if (!loaded_ || !L_) {
      if (err) *err = ScriptError::Make("not loaded", chunk_, 0);
      return false;
    }
    ScriptValue v;
    size_t pos = header.size();
    if (!ValueCodec::DecodeAt(state, &pos, &v)) {
      if (err) *err = ScriptError::Make("bad state body", chunk_, 0);
      return false;
    }
    lua_State* t = lua_newthread(L_);
    lua_getref(t, exportsRef_);
    PushScriptValue(t, v, 0);
    lua_setfield(t, -2, "__state");
    lua_pop(L_, 1);
    return true;
  }

  //-- used by the free helpers + interrupt callback ------------------------

  void NoteInstruction() { ++instructions_; }
  bool InterruptRequested() const { return interrupt_; }

  int InvokeBound(lua_State* L, const std::string& name) {
    if (!bindings_ || !ctx_) {
      luaL_error(L, "no bindings available");
      return 0;
    }
    const int nargs = lua_gettop(L);
    std::vector<ScriptValue> args((size_t)nargs);
    for (int i = 0; i < nargs; ++i)
      args[(size_t)i] = FromLua(L, i + 1, 0, &seen());
    bool ok = false, denied = false;
    ScriptValue r = bindings_->Invoke(*ctx_, name, args, &ok, &denied);
    if (!ok) {
      luaL_error(L, "%s %s", denied ? "capability denied" : "no such binding", name.c_str());
      return 0;
    }
    if (ctx_->hasError()) {
      const std::string msg = ctx_->errorMessage();
      ctx_->ClearError();
      luaL_error(L, "%s", msg.c_str());
      return 0;
    }
    PushScriptValue(L, r, 0);
    return 1;
  }

 private:
  struct BoundFn {
    LuauVm* vm = nullptr;
    std::string name;
  };

  static int BoundNative(lua_State* L) {
    BoundFn* bf = (BoundFn*)lua_touserdata(L, lua_upvalueindex(1));
    if (!bf || !bf->vm) {
      luaL_error(L, "binding has no VM");
      return 0;
    }
    return bf->vm->InvokeBound(L, bf->name);
  }

  static void Interrupt(lua_State* L, int gc) {
    if (gc >= 0) return;   // a GC-step notification, not our safepoint
    VmContext* vc = (VmContext*)lua_getthreaddata(L);
    if (!vc || !vc->vm) return;
    LuauVm* vm = vc->vm;
    vm->NoteInstruction();
    if (vc->aborting) return;

    ScriptStatus status = ScriptStatus::Ok;
    std::string message;
    if (vm->interrupt_) {
      status = ScriptStatus::Interrupted;
      message = "interrupted";
    } else if (vm->instructions_ > vm->limits_.instructionBudget) {
      status = ScriptStatus::BudgetExceeded;
      message = "instruction budget exceeded";
    } else if (SteadyMs() - vc->startMs > vm->limits_.wallClockMs) {
      status = ScriptStatus::Timeout;
      message = "wall-clock timeout";
    }
    if (status == ScriptStatus::Ok) return;

    vc->aborting = true;
    g_abort.status = status;
    g_abort.message = message;
    // The safepoint runs with the VM mid-instruction; reserve room for the
    // error object before luaL_error longjmps (Luau's own interrupt users do
    // the same).
    lua_rawcheckstack(L, 1);
    luaL_error(L, "%s", message.c_str());   // longjmps out of the resume
  }

  // Allocator with a hard byte cap. Refusing an allocation makes Luau raise an
  // out-of-memory error, which the resume maps onto MemoryExceeded.
  static void* Alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    LuauVm* self = (LuauVm*)ud;
    if (nsize == 0) {
      std::free(ptr);
      if (self) {
        self->liveBytes_ = self->liveBytes_ >= osize ? self->liveBytes_ - osize : 0;
      }
      return nullptr;
    }
    if (self) {
      const size_t delta = nsize > osize ? nsize - osize : 0;
      if (self->liveBytes_ + delta > self->limits_.memoryLimitBytes) {
        self->memoryDenied_ = true;
        return nullptr;
      }
    }
    void* p = std::realloc(ptr, nsize);
    if (self && p) {
      if (nsize >= osize) {
        self->liveBytes_ += nsize - osize;
      } else {
        self->liveBytes_ -= osize - nsize;
      }
    }
    return p;
  }

  // Drops the in-progress coroutine from L_'s stack (leaving L_ empty).
  void DropCall() {
    if (call_) {
      lua_pop(L_, 1);
      call_ = nullptr;
    }
  }

  // Maps a completed lua_resume to a ScriptStatus, collecting results on
  // success, and drops the coroutine on any terminal outcome.
  ScriptStatus Categorize(int luaStatus) {
    if (luaStatus == LUA_YIELD) {
      suspended_ = true;   // inCall_ stays true across a suspension
      status_ = ScriptStatus::Yielded;
      lua_settop(call_, 0);
      return status_;
    }
    if (luaStatus == 0) {
      // Stack after a completed call: [exports table][result...]. The function
      // and its arguments were consumed by the call, so results start at 2.
      result_.clear();
      const int n = lua_gettop(call_);
      for (int i = 2; i <= n; ++i)
        result_.push_back(FromLua(call_, i, 0, &seen()));
      status_ = ScriptStatus::Ok;
    } else if (g_abort.status != ScriptStatus::Ok) {
      lastError_ = ScriptError::Make(g_abort.message, chunk_, 0);
      status_ = g_abort.status;
      g_abort = AbortInfo{};
    } else if (memoryDenied_) {
      lastError_ = ScriptError::Make("memory limit exceeded", chunk_, 0);
      status_ = ScriptStatus::MemoryExceeded;
    } else {
      lastError_ = ScriptError::Make(ToString(call_, -1), chunk_, 0);
      status_ = ScriptStatus::Error;
    }
    memoryDenied_ = false;
    inCall_ = false;
    suspended_ = false;
    DropCall();
    if (ctx_) ctx_->ClearYield();
    return status_;
  }

  // Maps a raw lua_resume status during Load; returns true on success.
  bool MapResume(int luaStatus, ScriptError* err) {
    if (luaStatus == 0) return true;
    if (g_abort.status != ScriptStatus::Ok) {
      if (err) *err = ScriptError::Make(g_abort.message, chunk_, 0);
      g_abort = AbortInfo{};
    } else if (memoryDenied_) {
      if (err) *err = ScriptError::Make("memory limit exceeded", chunk_, 0);
    } else {
      if (err) *err = ScriptError::Make(ToString(L_, -1), chunk_, 0);
    }
    memoryDenied_ = false;
    return false;
  }

  bool PushFunction(lua_State* t, const std::string& name) const {
    lua_getfield(t, -1, name.c_str());
    if (lua_isfunction(t, -1)) return true;
    lua_pop(t, 1);
    std::string ns, fn;
    SplitName(name, &ns, &fn);
    if (ns.empty()) return false;
    lua_getfield(t, -1, ns.c_str());   // exports[ns]
    if (!lua_istable(t, -1)) {
      lua_pop(t, 1);
      return false;
    }
    lua_getfield(t, -1, fn.c_str());
    lua_remove(t, -2);   // drop the namespace table, keep the function
    return lua_isfunction(t, -1);
  }

  // Installs every registered binding as a global function, matching the IDL
  // spelling ("render.draw_text") and the nested call form render.draw_text().
  // Rebuild-safe: luaL_sandbox freezes nested tables, so a second install must
  // remove the previous names (thawing the globals first) before re-nesting.
  // A loaded script may already have captured the previous closures as
  // upvalues, so their BoundFn objects are retained for the VM's lifetime and
  // only the global names are swapped; freeing a captured closure would leave
  // the upvalue dangling.
  void InstallBindingGlobals() {
    if (!L_ || !bindings_) return;
    if (installedFrom_ == bindings_) return;   // idempotent for the same set
    installedFrom_ = bindings_;
    lua_setreadonly(L_, LUA_GLOBALSINDEX, false);
    for (const std::string& ns : installedNamespaces_) {
      lua_pushnil(L_);
      lua_setfield(L_, LUA_GLOBALSINDEX, ns.c_str());
    }
    installedNamespaces_.clear();
    for (const std::string& nm : installedPlain_) {
      lua_pushnil(L_);
      lua_setfield(L_, LUA_GLOBALSINDEX, nm.c_str());
    }
    installedPlain_.clear();

    for (const std::string& name : bindings_->Names()) {
      BoundFn* bf = new BoundFn();
      bf->vm = this;
      bf->name = name;
      boundFns_.push_back(bf);
      lua_pushlightuserdata(L_, bf);
      lua_pushcclosurek(L_, &BoundNative, name.c_str(), 1, nullptr);

      std::string ns, fn;
      SplitName(name, &ns, &fn);
      if (ns.empty()) {
        lua_setfield(L_, LUA_GLOBALSINDEX, fn.c_str());
        installedPlain_.push_back(fn);
      } else {
        lua_getfield(L_, LUA_GLOBALSINDEX, ns.c_str());
        if (!lua_istable(L_, -1)) {
          lua_pop(L_, 1);
          lua_newtable(L_);
          lua_pushvalue(L_, -1);
          lua_setfield(L_, LUA_GLOBALSINDEX, ns.c_str());
          installedNamespaces_.push_back(ns);
        }
        lua_pushvalue(L_, -2);            // the function
        lua_setfield(L_, -2, fn.c_str()); // ns[fn] = function
        lua_pop(L_, 2);                   // ns table + function
      }
    }
    Sandbox();   // re-freeze the globals
  }

  // Removes the helpers a sandboxed script must never reach.
  void StripDangerous() {
    static const char* kDrop[] = {
        "require", "dofile", "loadfile", "loadstring", "load", "collectgarbage",
        "newproxy", "getfenv", "setfenv",
    };
    for (const char* name : kDrop) {
      lua_pushnil(L_);
      lua_setfield(L_, LUA_GLOBALSINDEX, name);
    }
    lua_getfield(L_, LUA_GLOBALSINDEX, "os");
    if (lua_istable(L_, -1)) {
      static const char* kOsDrop[] = {"execute", "remove", "rename", "tmpname",
                                      "exit", "setlocale", "getenv"};
      for (const char* name : kOsDrop) {
        lua_pushnil(L_);
        lua_setfield(L_, -2, name);
      }
    }
    lua_pop(L_, 1);
    lua_pushnil(L_);
    lua_setfield(L_, LUA_GLOBALSINDEX, "debug");
  }

  void Sandbox() { luaL_sandbox(L_); }

  mutable std::unordered_set<const void*> seen_;
  std::unordered_set<const void*>& seen() const {
    seen_.clear();
    return seen_;
  }

  lua_State* L_ = nullptr;
  lua_State* call_ = nullptr;      // coroutine of the in-progress/suspended call
  int exportsRef_ = LUA_REFNIL;    // registry ref to the exports table
  VmContext vmctx_;
  ScriptLimits limits_;
  const ScriptBindings* bindings_ = nullptr;
  ScriptContext* ctx_ = nullptr;
  std::vector<ScriptValue> result_;
  ScriptError lastError_;
  ScriptStatus status_ = ScriptStatus::Ok;
  u64 instructions_ = 0;
  size_t liveBytes_ = 0;
  bool memoryDenied_ = false;
  bool loaded_ = false;
  bool inCall_ = false;
  bool suspended_ = false;
  bool interrupt_ = false;
  std::string chunk_;
  const ScriptBindings* installedFrom_ = nullptr;
  std::vector<BoundFn*> boundFns_;   // retained for the VM's lifetime
  std::vector<std::string> installedNamespaces_;
  std::vector<std::string> installedPlain_;
};

BERRY_API IScriptVm* CreateLuauVm() { return new LuauVm(); }

}  // namespace Berry
