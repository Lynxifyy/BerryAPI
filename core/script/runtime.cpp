// Script runtime primitives: values, errors, status names, the
// capability-gated binding registry, and the VM-facing convenience paths.
#include "berry/script.h"
#include "berry/event.h"     // NowNs
#include "berry/logger.h"
#include "berry/thread.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Berry {

//----------------------------------------------------------------------------
// ScriptValue
//----------------------------------------------------------------------------

ScriptValue ScriptValue::Bool(bool v) {
  ScriptValue s;
  s.type = Type::Bool;
  s.boolean = v;
  return s;
}

ScriptValue ScriptValue::Num(f64 v) {
  ScriptValue s;
  s.type = Type::Number;
  s.number = v;
  return s;
}

ScriptValue ScriptValue::Str(std::string v) {
  ScriptValue s;
  s.type = Type::String;
  s.string = std::move(v);
  return s;
}

ScriptValue ScriptValue::Tbl(ScriptTable t) {
  ScriptValue s;
  s.type = Type::Table;
  s.table = std::make_shared<ScriptTable>(std::move(t));
  return s;
}

const ScriptValue* ScriptValue::Field(const char* key) const {
  if (type != Type::Table || !table || !key) return nullptr;
  auto it = table->find(key);
  return it == table->end() ? nullptr : &it->second;
}

bool ScriptValue::Truthy() const {
  switch (type) {
    case Type::Nil: return false;
    case Type::Bool: return boolean;
    case Type::Number: return number != 0;
    case Type::String: return !string.empty();
    case Type::Table: return true;   // an empty table is truthy, as in Luau
  }
  return false;
}

f64 ScriptValue::AsNumber() const {
  switch (type) {
    case Type::Number: return number;
    case Type::Bool: return boolean ? 1 : 0;
    case Type::String: {
      // Numeric strings coerce; anything else is 0 (deterministic, no throw).
      char* end = nullptr;
      double v = std::strtod(string.c_str(), &end);
      if (end && end != string.c_str() && *end == '\0') return (f64)v;
      return 0;
    }
    case Type::Nil: return 0;
    case Type::Table: return 0;   // tables have no numeric value
  }
  return 0;
}

const std::string& ScriptValue::AsString() const {
  static const std::string kEmpty;
  return type == Type::String ? string : kEmpty;
}

std::string ScriptValue::ToDisplay() const {
  switch (type) {
    case Type::Nil: return "nil";
    case Type::Bool: return boolean ? "true" : "false";
    case Type::Number: {
      // Integers print without a decimal point; fractions keep precision.
      if (std::isfinite(number) && number == std::floor(number) &&
          std::fabs(number) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", (long long)number);
        return buf;
      }
      char buf[40];
      std::snprintf(buf, sizeof(buf), "%.14g", number);
      return buf;
    }
    case Type::String: return string;
    case Type::Table: {
      if (!table) return "{}";
      // Deterministic key order (std::map) so the form is stable in errors,
      // the REPL and the serialized snapshot.
      std::string out = "{";
      bool first = true;
      for (const auto& kv : *table) {
        if (!first) out += ", ";
        first = false;
        out += kv.first;
        out += " = ";
        out += kv.second.ToDisplay();
      }
      out += "}";
      return out;
    }
  }
  return "";
}

bool ScriptValue::operator==(const ScriptValue& o) const {
  if (type != o.type) {
    // Numbers and bools compare numerically; everything else is unequal.
    if ((type == Type::Number && o.type == Type::Bool) ||
        (type == Type::Bool && o.type == Type::Number)) {
      return AsNumber() == o.AsNumber();
    }
    return false;
  }
  switch (type) {
    case Type::Nil: return true;
    case Type::Bool: return boolean == o.boolean;
    case Type::Number: return number == o.number;
    case Type::String: return string == o.string;
    // Reference equality, like Luau tables: comparing by content could recurse
    // forever on a self-referential table.
    case Type::Table: return table == o.table;
  }
  return false;
}

//----------------------------------------------------------------------------
// ScriptError
//----------------------------------------------------------------------------

ScriptError ScriptError::Make(std::string msg, std::string chunk, i32 line) {
  ScriptError e;
  e.occurred = true;
  e.message = std::move(msg);
  e.chunk = std::move(chunk);
  e.line = line;
  return e;
}

//----------------------------------------------------------------------------
// Status names
//----------------------------------------------------------------------------

const char* ScriptStatusName(ScriptStatus s) {
  switch (s) {
    case ScriptStatus::Ok: return "ok";
    case ScriptStatus::Running: return "running";
    case ScriptStatus::Yielded: return "yielded";
    case ScriptStatus::NotLoaded: return "not-loaded";
    case ScriptStatus::NoFunction: return "no-function";
    case ScriptStatus::Error: return "error";
    case ScriptStatus::BudgetExceeded: return "budget-exceeded";
    case ScriptStatus::MemoryExceeded: return "memory-exceeded";
    case ScriptStatus::Timeout: return "timeout";
    case ScriptStatus::Interrupted: return "interrupted";
    case ScriptStatus::CapabilityDenied: return "capability-denied";
    case ScriptStatus::Unloaded: return "unloaded";
  }
  return "unknown";
}

const char* ScriptHostStateName(ScriptHostState s) {
  switch (s) {
    case ScriptHostState::Idle: return "idle";
    case ScriptHostState::Loaded: return "loaded";
    case ScriptHostState::Faulted: return "faulted";
    case ScriptHostState::Unloaded: return "unloaded";
  }
  return "unknown";
}

//----------------------------------------------------------------------------
// ScriptBindings
//----------------------------------------------------------------------------

void ScriptBindings::Register(const std::string& name, const Capability& required,
                              ScriptNativeFn fn) {
  for (auto& e : entries_) {
    if (e.first == name) {
      e.second.required = required;
      e.second.fn = std::move(fn);
      return;
    }
  }
  entries_.push_back({name, Entry{required, std::move(fn)}});
}

bool ScriptBindings::Has(const std::string& name) const {
  for (const auto& e : entries_) {
    if (e.first == name) return true;
  }
  return false;
}

Capability ScriptBindings::Required(const std::string& name) const {
  for (const auto& e : entries_) {
    if (e.first == name) return e.second.required;
  }
  return Capability{};   // Unknown
}

ScriptValue ScriptBindings::Invoke(ScriptContext& ctx, const std::string& name,
                                   const std::vector<ScriptValue>& args, bool* ok,
                                   bool* denied) const {
  if (ok) *ok = false;
  if (denied) *denied = false;
  for (const auto& e : entries_) {
    if (e.first != name) continue;
    // Capability gate: a binding whose capability was not granted is denied,
    // never silently allowed.
    if (e.second.required.kind != CapKind::Unknown &&
        !ctx.CanUse(e.second.required)) {
      if (denied) *denied = true;
      return ScriptValue::Nil();
    }
    if (!e.second.fn) {
      if (ok) *ok = true;
      return ScriptValue::Nil();
    }
    if (ok) *ok = true;
    return e.second.fn(ctx, args);
  }
  return ScriptValue::Nil();
}

size_t ScriptBindings::Count() const { return entries_.size(); }

std::vector<std::string> ScriptBindings::Names() const {
  std::vector<std::string> names;
  names.reserve(entries_.size());
  for (const auto& e : entries_) names.push_back(e.first);
  return names;
}

void ScriptBindings::Clear() { entries_.clear(); }

//----------------------------------------------------------------------------
// ScriptContext
//----------------------------------------------------------------------------

void ScriptContext::SetScratch(const std::string& key, ScriptValue v) {
  for (auto& kv : scratch_) {
    if (kv.first == key) {
      kv.second = std::move(v);
      return;
    }
  }
  scratch_.emplace_back(key, std::move(v));
}

bool ScriptContext::GetScratch(const std::string& key, ScriptValue* out) const {
  for (const auto& kv : scratch_) {
    if (kv.first == key) {
      if (out) *out = kv.second;
      return true;
    }
  }
  return false;
}

//----------------------------------------------------------------------------
// IScriptVm convenience
//----------------------------------------------------------------------------

ScriptStatus IScriptVm::Call(const std::string& name,
                             const std::vector<ScriptValue>& args,
                             std::vector<ScriptValue>* ret, ScriptError* err) {
  ResetInstructionCount();
  ScriptStatus st = Start(name, args, err);
  if (st != ScriptStatus::Running && st != ScriptStatus::Ok) return st;
  // Drive to completion, respecting the VM's own budget as a safety ceiling so
  // a caller cannot spin forever on a runaway script.
  const u64 kStepBudget = 4096;
  u64 guard = 0;
  const u64 maxSteps = (Limits().instructionBudget / kStepBudget) + 16;
  while (inCall()) {
    st = Step(kStepBudget, err);
    if (st != ScriptStatus::Running) {
      if (ret) *ret = Result();
      return st;
    }
    if (++guard > maxSteps) {
      Abort();
      if (err) {
        *err = ScriptError::Make("instruction budget exceeded", "", 0);
      }
      return ScriptStatus::BudgetExceeded;
    }
  }
  if (ret) *ret = Result();
  return ScriptStatus::Ok;
}

//----------------------------------------------------------------------------
// ScopedScriptTimeslice
//----------------------------------------------------------------------------

ScopedScriptTimeslice::ScopedScriptTimeslice(IScriptVm* vm, u64 maxMs)
    : vm_(vm) {
  // Scripts run on the dedicated worker, never the render thread.
  ThreadModel::Instance().AssertScripts();
  startNs_ = NowNs();
  deadlineNs_ = startNs_ + maxMs * 1000000ULL;
}

ScopedScriptTimeslice::~ScopedScriptTimeslice() {
  // Leaving the scope must not leave a stale interrupt armed on the VM.
  if (vm_) vm_->ClearInterrupt();
}

bool ScopedScriptTimeslice::expired() const {
  if (NowNs() < deadlineNs_) return false;
  // Past the deadline: arm the VM interrupt so the next instruction boundary
  // stops the script instead of trusting it to yield voluntarily.
  if (vm_) vm_->RequestInterrupt();
  return true;
}

u64 ScopedScriptTimeslice::elapsedMs() const {
  return (NowNs() - startNs_) / 1000000ULL;
}

}  // namespace Berry
