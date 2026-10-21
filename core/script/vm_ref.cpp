// Reference scripting VM.
//
// This is the host-test backend behind IScriptVm. It is intentionally small
// and deterministic: a stack machine over a line-oriented assembler dialect,
// with a hard instruction budget, a memory cap and a cooperative interrupt.
// The point is to exercise the runtime contract (budgets, capability-gated
// bindings, error isolation, state serialization) with no third-party engine
// and no game binary, exactly like ShimBackend does for hooking.
//
// Dialect (one instruction per line; '#' starts a comment):
//
//   func <name>            begin a function body
//   end                    end the current function
//   push <literal>         push nil | true | false | number | "string"
//   pop                    discard TOS
//   dup                    duplicate TOS
//   load <name>            push the value of a global
//   store <name>           pop TOS into a global
//   add sub mul div mod    numeric binary ops
//   neg not                unary ops
//   eq ne lt le gt ge      comparisons (push true/false)
//   and or                 logical ops (short-circuit at runtime)
//   jmp <label>            unconditional jump
//   jz <label>             jump when TOS is falsy
//   jnz <label>            jump when TOS is truthy
//   label:                 jump target
//   call <name> <argc>     call a script function (pushes its return value)
//   native <name> <argc>   call a capability-gated native binding
//   ret [<argc>]           return; optional number of values
//   abort <message>        raise an error
//
// Globals are the serialized state. Budget enforcement counts every executed
// instruction and checks the interrupt flag at the same boundary.

#include "berry/script.h"
#include "berry/logger.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <unordered_map>

namespace Berry {
namespace {

struct Insn {
  enum class Op {
    PushNil, PushBool, PushNum, PushStr,
    Pop, Dup, Load, Store,
    Add, Sub, Mul, Div, Mod, Neg, Not,
    Eq, Ne, Lt, Le, Gt, Ge, And, Or,
    Jmp, Jz, Jnz,
    Call, Native, Ret, Abort,
  };
  Op op = Op::Pop;
  ScriptValue literal;          // for Push*
  std::string name;             // for Load/Store/Call/Native/Abort
  std::string label;            // for jumps
  i32 argc = 0;                 // for Call/Native/Ret
  i32 line = 0;                 // source line for diagnostics
};

struct Function {
  std::string name;
  size_t entry = 0;             // index into program
  size_t end = 0;
  i32 paramCount = 0;           // declared parameters (for arity checks)
  std::vector<std::string> params;
};

class RefVm final : public IScriptVm {
 public:
  const char* Name() const override { return "berry-ref"; }

  bool Load(const std::string& source, const std::string& chunkName,
            ScriptError* err) override {
    Unload();
    chunk_ = chunkName;
    if (!Assemble(source, err)) {
      Unload();
      return false;
    }
    loaded_ = true;
    return true;
  }

  void Unload() override {
    program_.clear();
    functions_.clear();
    funcIndex_.clear();
    globals_.clear();
    stack_.clear();
    result_.clear();
    loaded_ = false;
    inCall_ = false;
    callDepth_ = 0;
    instructions_ = 0;
    interrupt_ = false;
  }

  bool loaded() const override { return loaded_; }

  bool HasFunction(const std::string& name) const override {
    return funcIndex_.find(name) != funcIndex_.end();
  }

  ScriptStatus Start(const std::string& name,
                     const std::vector<ScriptValue>& args,
                     ScriptError* err) override {
    if (!loaded_) return ScriptStatus::NotLoaded;
    auto it = funcIndex_.find(name);
    if (it == funcIndex_.end()) {
      if (err) *err = ScriptError::Make("no such function: " + name, chunk_, 0);
      return ScriptStatus::NoFunction;
    }
    stack_.clear();
    result_.clear();
    resultValues_.clear();
    frames_.clear();
    inCall_ = true;
    callDepth_ = 0;
    // Seed the argument frame; the callee reads params from globals so state
    // stays serializable (arguments are not part of persisted state).
    const Function& fn = functions_[it->second];
    for (size_t i = 0; i < fn.params.size(); ++i) {
      globals_[fn.params[i]] = i < args.size() ? args[i] : ScriptValue::Nil();
    }
    frames_.push_back(Frame{it->second, fn.entry, fn.end});
    pc_ = fn.entry;
    return ScriptStatus::Running;
  }

  ScriptStatus Step(u64 budget, ScriptError* err) override {
    if (!loaded_) return ScriptStatus::NotLoaded;
    if (!inCall_) return ScriptStatus::Ok;
    if (interrupt_) {
      Abort();
      if (err) *err = ScriptError::Make("interrupted", chunk_, CurrentLine());
      return ScriptStatus::Interrupted;
    }
    u64 executed = 0;
    while (inCall_) {
      if (executed >= budget) return ScriptStatus::Running;
      if (interrupt_) {
        Abort();
        if (err) *err = ScriptError::Make("interrupted", chunk_, CurrentLine());
        return ScriptStatus::Interrupted;
      }
      if (instructions_ >= limits_.instructionBudget) {
        Abort();
        if (err) {
          *err = ScriptError::Make("instruction budget exceeded", chunk_,
                                   CurrentLine());
        }
        return ScriptStatus::BudgetExceeded;
      }
      if (MemoryUsed() > limits_.memoryLimitBytes) {
        Abort();
        if (err) *err = ScriptError::Make("memory limit exceeded", chunk_,
                                          CurrentLine());
        return ScriptStatus::MemoryExceeded;
      }
      ScriptStatus st = ExecOne(err);
      ++executed;
      ++instructions_;
      if (st != ScriptStatus::Running) {
        // A terminal step must publish the call's result values before the
        // caller reads them back through Result().
        if (st == ScriptStatus::Ok) {
          result_ = resultValues_;
          resultValues_.clear();
        }
        return st;
      }
    }
    // Reached only when ExecOne ended the call without reporting a terminal
    // status; publish whatever the frame produced.
    result_ = resultValues_;
    resultValues_.clear();
    return ScriptStatus::Ok;
  }

  bool inCall() const override { return inCall_; }
  const std::vector<ScriptValue>& Result() const override { return result_; }

  void Abort() override {
    inCall_ = false;
    frames_.clear();
    stack_.clear();
    resultValues_.clear();
  }

  void SetLimits(const ScriptLimits& limits) override { limits_ = limits; }
  const ScriptLimits& Limits() const override { return limits_; }

  void SetBindings(const ScriptBindings* bindings) override { bindings_ = bindings; }
  void SetContext(ScriptContext* ctx) override { ctx_ = ctx; }

  u64 InstructionsExecuted() const override { return instructions_; }

  size_t MemoryUsed() const override {
    size_t bytes = 0;
    for (const auto& kv : globals_) {
      bytes += kv.first.size() + kv.second.string.size() + sizeof(ScriptValue);
    }
    return bytes;
  }

  void ResetInstructionCount() override { instructions_ = 0; }

  void RequestInterrupt() override { interrupt_ = true; }
  bool interrupted() const override { return interrupt_; }
  void ClearInterrupt() override { interrupt_ = false; }

  std::string SerializeState() const override {
    // Deterministic key order (std::map) so the snapshot is byte-stable.
    std::ostringstream os;
    os << "berry-ref-state-v1\n";
    std::map<std::string, ScriptValue> ordered(globals_.begin(), globals_.end());
    for (const auto& kv : ordered) {
      os << kv.first << "=" << EncodeValue(kv.second) << "\n";
    }
    return os.str();
  }

  bool RestoreState(const std::string& state, ScriptError* err) override {
    std::istringstream is(state);
    std::string header;
    if (!std::getline(is, header) || header != "berry-ref-state-v1") {
      if (err) *err = ScriptError::Make("bad state header", chunk_, 0);
      return false;
    }
    std::unordered_map<std::string, ScriptValue> restored;
    std::string line;
    while (std::getline(is, line)) {
      if (line.empty()) continue;
      size_t eq = line.find('=');
      if (eq == std::string::npos) {
        if (err) *err = ScriptError::Make("bad state line: " + line, chunk_, 0);
        return false;
      }
      ScriptValue v;
      if (!DecodeValue(line.substr(eq + 1), &v)) {
        if (err) *err = ScriptError::Make("bad state value: " + line, chunk_, 0);
        return false;
      }
      restored[line.substr(0, eq)] = v;
    }
    globals_ = std::move(restored);
    return true;
  }

 private:
  struct Frame {
    size_t funcIndex;
    size_t returnPc;
    size_t end;
  };

  i32 CurrentLine() const {
    if (pc_ < program_.size()) return program_[pc_].line;
    return 0;
  }

  static std::string EncodeValue(const ScriptValue& v) {
    switch (v.type) {
      case ScriptValue::Type::Nil: return "nil";
      case ScriptValue::Type::Bool: return v.boolean ? "true" : "false";
      case ScriptValue::Type::Number: return "n:" + v.ToDisplay();
      case ScriptValue::Type::String: {
        // Length-prefixed so embedded newlines/commas survive round-trip.
        return "s:" + std::to_string(v.string.size()) + ":" + v.string;
      }
    }
    return "nil";
  }

  static bool DecodeValue(const std::string& s, ScriptValue* out) {
    if (s == "nil") { *out = ScriptValue::Nil(); return true; }
    if (s == "true") { *out = ScriptValue::Bool(true); return true; }
    if (s == "false") { *out = ScriptValue::Bool(false); return true; }
    if (s.rfind("n:", 0) == 0) {
      *out = ScriptValue::Num(std::strtod(s.c_str() + 2, nullptr));
      return true;
    }
    if (s.rfind("s:", 0) == 0) {
      size_t colon = s.find(':', 2);
      if (colon == std::string::npos) return false;
      size_t len = (size_t)std::strtoull(s.c_str() + 2, nullptr, 10);
      std::string body = s.substr(colon + 1);
      if (body.size() != len) return false;
      *out = ScriptValue::Str(body);
      return true;
    }
    return false;
  }

  //----------------------------------------------------------------------------
  // Assembler
  //----------------------------------------------------------------------------

  static std::vector<std::string> Tokenize(const std::string& line) {
    std::vector<std::string> toks;
    size_t i = 0;
    while (i < line.size()) {
      char c = line[i];
      if (std::isspace((unsigned char)c)) { ++i; continue; }
      if (c == '#') break;
      std::string tok;
      if (c == '"') {
        tok.push_back('"');
        ++i;
        while (i < line.size() && line[i] != '"') {
          if (line[i] == '\\' && i + 1 < line.size()) {
            tok.push_back(line[i]);
            tok.push_back(line[i + 1]);
            i += 2;
            continue;
          }
          tok.push_back(line[i]);
          ++i;
        }
        if (i < line.size()) { tok.push_back('"'); ++i; }
      } else {
        while (i < line.size() && !std::isspace((unsigned char)line[i]) &&
               line[i] != '#') {
          tok.push_back(line[i]);
          ++i;
        }
      }
      toks.push_back(std::move(tok));
    }
    return toks;
  }

  static bool ParseStringLiteral(const std::string& tok, std::string* out) {
    if (tok.size() < 2 || tok.front() != '"' || tok.back() != '"') return false;
    out->clear();
    for (size_t i = 1; i + 1 < tok.size(); ++i) {
      if (tok[i] == '\\' && i + 2 < tok.size()) {
        char e = tok[++i];
        switch (e) {
          case 'n': out->push_back('\n'); break;
          case 't': out->push_back('\t'); break;
          case 'r': out->push_back('\r'); break;
          case '"': out->push_back('"'); break;
          case '\\': out->push_back('\\'); break;
          default: out->push_back(e); break;
        }
        continue;
      }
      out->push_back(tok[i]);
    }
    return true;
  }

  bool Assemble(const std::string& source, ScriptError* err) {
    std::istringstream is(source);
    std::string raw;
    i32 lineNo = 0;
    Function* current = nullptr;
    std::map<std::string, size_t> labels;
    struct PendingJump { size_t insnIndex; std::string label; i32 line; };
    std::vector<PendingJump> jumps;

    while (std::getline(is, raw)) {
      ++lineNo;
      std::vector<std::string> toks = Tokenize(raw);
      if (toks.empty()) continue;
      std::string& op = toks[0];

      // label:
      if (!op.empty() && op.back() == ':') {
        std::string label = op.substr(0, op.size() - 1);
        if (label.empty()) {
          if (err) *err = ScriptError::Make("empty label", chunk_, lineNo);
          return false;
        }
        if (labels.count(label)) {
          if (err) *err = ScriptError::Make("duplicate label: " + label, chunk_,
                                            lineNo);
          return false;
        }
        labels[label] = program_.size();
        continue;
      }

      auto need = [&](size_t n, const char* what) {
        if (toks.size() < n) {
          if (err) {
            *err = ScriptError::Make(std::string("expected ") + what + " after '" +
                                         op + "'", chunk_, lineNo);
          }
          return false;
        }
        return true;
      };

      if (op == "func") {
        if (!need(2, "name")) return false;
        if (current) {
          if (err) *err = ScriptError::Make("nested func not allowed", chunk_, lineNo);
          return false;
        }
        Function fn;
        fn.name = toks[1];
        fn.entry = program_.size();
        // Parameters may be declared as "func name a b c".
        for (size_t i = 2; i < toks.size(); ++i) fn.params.push_back(toks[i]);
        fn.paramCount = (i32)fn.params.size();
        if (funcIndex_.count(fn.name)) {
          if (err) *err = ScriptError::Make("duplicate function: " + fn.name,
                                            chunk_, lineNo);
          return false;
        }
        functions_.push_back(fn);
        funcIndex_[fn.name] = functions_.size() - 1;
        current = &functions_.back();
        continue;
      }
      if (op == "end") {
        if (!current) {
          if (err) *err = ScriptError::Make("'end' outside func", chunk_, lineNo);
          return false;
        }
        current->end = program_.size();
        // Implicit return at the end of every function.
        Insn ret;
        ret.op = Insn::Op::Ret;
        ret.argc = 0;
        ret.line = lineNo;
        program_.push_back(ret);
        current = nullptr;
        continue;
      }

      Insn insn;
      insn.line = lineNo;
      bool ok = true;
      if (op == "push") {
        if (!need(2, "literal")) return false;
        if (toks.size() != 2) {
          if (err) *err = ScriptError::Make("push takes exactly one literal",
                                            chunk_, lineNo);
          return false;
        }
        const std::string& lit = toks[1];
        if (lit == "nil") { insn.op = Insn::Op::PushNil; }
        else if (lit == "true") { insn.op = Insn::Op::PushBool; insn.literal = ScriptValue::Bool(true); }
        else if (lit == "false") { insn.op = Insn::Op::PushBool; insn.literal = ScriptValue::Bool(false); }
        else if (lit.size() >= 2 && lit.front() == '"') {
          std::string s;
          if (!ParseStringLiteral(lit, &s)) {
            if (err) *err = ScriptError::Make("bad string literal", chunk_, lineNo);
            return false;
          }
          insn.op = Insn::Op::PushStr;
          insn.literal = ScriptValue::Str(s);
        } else {
          char* endp = nullptr;
          double v = std::strtod(lit.c_str(), &endp);
          if (endp == lit.c_str() || *endp != '\0') {
            if (err) *err = ScriptError::Make("bad number: " + lit, chunk_, lineNo);
            return false;
          }
          insn.op = Insn::Op::PushNum;
          insn.literal = ScriptValue::Num(v);
        }
      } else if (op == "pop") { insn.op = Insn::Op::Pop; }
      else if (op == "dup") { insn.op = Insn::Op::Dup; }
      else if (op == "load") { if (!need(2, "name")) return false; insn.op = Insn::Op::Load; insn.name = toks[1]; }
      else if (op == "store") { if (!need(2, "name")) return false; insn.op = Insn::Op::Store; insn.name = toks[1]; }
      else if (op == "add") { insn.op = Insn::Op::Add; }
      else if (op == "sub") { insn.op = Insn::Op::Sub; }
      else if (op == "mul") { insn.op = Insn::Op::Mul; }
      else if (op == "div") { insn.op = Insn::Op::Div; }
      else if (op == "mod") { insn.op = Insn::Op::Mod; }
      else if (op == "neg") { insn.op = Insn::Op::Neg; }
      else if (op == "not") { insn.op = Insn::Op::Not; }
      else if (op == "eq") { insn.op = Insn::Op::Eq; }
      else if (op == "ne") { insn.op = Insn::Op::Ne; }
      else if (op == "lt") { insn.op = Insn::Op::Lt; }
      else if (op == "le") { insn.op = Insn::Op::Le; }
      else if (op == "gt") { insn.op = Insn::Op::Gt; }
      else if (op == "ge") { insn.op = Insn::Op::Ge; }
      else if (op == "and") { insn.op = Insn::Op::And; }
      else if (op == "or") { insn.op = Insn::Op::Or; }
      else if (op == "jmp") { if (!need(2, "label")) return false; insn.op = Insn::Op::Jmp; insn.label = toks[1]; }
      else if (op == "jz") { if (!need(2, "label")) return false; insn.op = Insn::Op::Jz; insn.label = toks[1]; }
      else if (op == "jnz") { if (!need(2, "label")) return false; insn.op = Insn::Op::Jnz; insn.label = toks[1]; }
      else if (op == "call") {
        if (!need(3, "name and argc")) return false;
        insn.op = Insn::Op::Call;
        insn.name = toks[1];
        insn.argc = (i32)std::strtol(toks[2].c_str(), nullptr, 10);
      } else if (op == "native") {
        if (!need(3, "name and argc")) return false;
        insn.op = Insn::Op::Native;
        insn.name = toks[1];
        insn.argc = (i32)std::strtol(toks[2].c_str(), nullptr, 10);
      } else if (op == "ret") {
        insn.op = Insn::Op::Ret;
        insn.argc = toks.size() >= 2 ? (i32)std::strtol(toks[1].c_str(), nullptr, 10) : 0;
      } else if (op == "abort") {
        if (!need(2, "message")) return false;
        std::string msg = toks[1];
        if (!msg.empty() && msg.front() == '"') {
          // Parse into a distinct buffer: ParseStringLiteral clears its output
          // before reading, so passing `msg` as both would truncate it.
          std::string parsed;
          if (!ParseStringLiteral(msg, &parsed)) {
            if (err) *err = ScriptError::Make("bad abort message", chunk_, lineNo);
            return false;
          }
          msg = std::move(parsed);
        }
        insn.op = Insn::Op::Abort;
        insn.name = msg;
      } else {
        if (err) *err = ScriptError::Make("unknown opcode: " + op, chunk_, lineNo);
        return false;
      }

      if (!ok) return false;
      program_.push_back(insn);
      if (insn.op == Insn::Op::Jmp || insn.op == Insn::Op::Jz ||
          insn.op == Insn::Op::Jnz) {
        jumps.push_back({program_.size() - 1, insn.label, lineNo});
      }
    }

    if (current) {
      if (err) *err = ScriptError::Make("unterminated function: " + current->name,
                                        chunk_, 0);
      return false;
    }

    // Resolve jumps.
    for (const auto& j : jumps) {
      auto it = labels.find(j.label);
      if (it == labels.end()) {
        if (err) {
          *err = ScriptError::Make("undefined label: " + j.label, chunk_, j.line);
        }
        return false;
      }
      program_[j.insnIndex].argc = (i32)it->second;   // reuse argc as target
    }
    return true;
  }

  //----------------------------------------------------------------------------
  // Interpreter
  //----------------------------------------------------------------------------

  ScriptStatus ExecOne(ScriptError* err) {
    if (pc_ >= program_.size()) {
      inCall_ = false;
      return ScriptStatus::Ok;
    }
    const Insn& in = program_[pc_];
    switch (in.op) {
      case Insn::Op::PushNil: stack_.push_back(ScriptValue::Nil()); ++pc_; break;
      case Insn::Op::PushBool: stack_.push_back(in.literal); ++pc_; break;
      case Insn::Op::PushNum: stack_.push_back(in.literal); ++pc_; break;
      case Insn::Op::PushStr: stack_.push_back(in.literal); ++pc_; break;
      case Insn::Op::Pop: if (!Pop()) return Underflow(err); ++pc_; break;
      case Insn::Op::Dup: {
        if (stack_.empty()) return Underflow(err);
        stack_.push_back(stack_.back());
        ++pc_;
        break;
      }
      case Insn::Op::Load: {
        auto it = globals_.find(in.name);
        stack_.push_back(it == globals_.end() ? ScriptValue::Nil() : it->second);
        ++pc_;
        break;
      }
      case Insn::Op::Store: {
        if (stack_.empty()) return Underflow(err);
        globals_[in.name] = stack_.back();
        stack_.pop_back();
        ++pc_;
        break;
      }
      case Insn::Op::Add: case Insn::Op::Sub: case Insn::Op::Mul:
      case Insn::Op::Div: case Insn::Op::Mod: {
        ScriptValue b, a;
        if (!Pop2(&a, &b)) return Underflow(err);
        // '+' concatenates when either operand is a string (Luau/JS-ish).
        if (in.op == Insn::Op::Add && (a.IsString() || b.IsString())) {
          std::string lhs = a.IsString() ? a.string : a.ToDisplay();
          std::string rhs = b.IsString() ? b.string : b.ToDisplay();
          stack_.push_back(ScriptValue::Str(lhs + rhs));
          ++pc_;
          break;
        }
        f64 x = a.AsNumber(), y = b.AsNumber();
        f64 r = 0;
        switch (in.op) {
          case Insn::Op::Add: r = x + y; break;
          case Insn::Op::Sub: r = x - y; break;
          case Insn::Op::Mul: r = x * y; break;
          case Insn::Op::Div:
            if (y == 0) {
              if (err) *err = ScriptError::Make("division by zero", chunk_, in.line);
              Abort();
              return ScriptStatus::Error;
            }
            r = x / y;
            break;
          case Insn::Op::Mod:
            if (y == 0) {
              if (err) *err = ScriptError::Make("modulo by zero", chunk_, in.line);
              Abort();
              return ScriptStatus::Error;
            }
            r = std::fmod(x, y);
            break;
          default: break;
        }
        stack_.push_back(ScriptValue::Num(r));
        ++pc_;
        break;
      }
      case Insn::Op::Neg: {
        if (stack_.empty()) return Underflow(err);
        stack_.back() = ScriptValue::Num(-stack_.back().AsNumber());
        ++pc_;
        break;
      }
      case Insn::Op::Not: {
        if (stack_.empty()) return Underflow(err);
        stack_.back() = ScriptValue::Bool(!stack_.back().Truthy());
        ++pc_;
        break;
      }
      case Insn::Op::Eq: case Insn::Op::Ne: case Insn::Op::Lt:
      case Insn::Op::Le: case Insn::Op::Gt: case Insn::Op::Ge: {
        ScriptValue b, a;
        if (!Pop2(&a, &b)) return Underflow(err);
        bool r = false;
        switch (in.op) {
          case Insn::Op::Eq: r = (a == b); break;
          case Insn::Op::Ne: r = !(a == b); break;
          case Insn::Op::Lt: r = a.AsNumber() < b.AsNumber(); break;
          case Insn::Op::Le: r = a.AsNumber() <= b.AsNumber(); break;
          case Insn::Op::Gt: r = a.AsNumber() > b.AsNumber(); break;
          case Insn::Op::Ge: r = a.AsNumber() >= b.AsNumber(); break;
          default: break;
        }
        stack_.push_back(ScriptValue::Bool(r));
        ++pc_;
        break;
      }
      case Insn::Op::And: case Insn::Op::Or: {
        ScriptValue b, a;
        if (!Pop2(&a, &b)) return Underflow(err);
        bool r = in.op == Insn::Op::And ? (a.Truthy() && b.Truthy())
                                        : (a.Truthy() || b.Truthy());
        stack_.push_back(ScriptValue::Bool(r));
        ++pc_;
        break;
      }
      case Insn::Op::Jmp: pc_ = (size_t)in.argc; break;
      case Insn::Op::Jz: {
        if (stack_.empty()) return Underflow(err);
        bool falsy = !stack_.back().Truthy();
        stack_.pop_back();
        pc_ = falsy ? (size_t)in.argc : pc_ + 1;
        break;
      }
      case Insn::Op::Jnz: {
        if (stack_.empty()) return Underflow(err);
        bool truthy = stack_.back().Truthy();
        stack_.pop_back();
        pc_ = truthy ? (size_t)in.argc : pc_ + 1;
        break;
      }
      case Insn::Op::Call: return DoCall(in, err);
      case Insn::Op::Native: return DoNative(in, err);
      case Insn::Op::Ret: return DoReturn(in);
      case Insn::Op::Abort: {
        if (err) *err = ScriptError::Make(in.name.empty() ? "aborted" : in.name,
                                          chunk_, in.line);
        Abort();
        return ScriptStatus::Error;
      }
    }
    return ScriptStatus::Running;
  }

  ScriptStatus Underflow(ScriptError* err) {
    if (err) *err = ScriptError::Make("stack underflow", chunk_, CurrentLine());
    Abort();
    return ScriptStatus::Error;
  }

  bool Pop() {
    if (stack_.empty()) return false;
    stack_.pop_back();
    return true;
  }

  bool Pop2(ScriptValue* a, ScriptValue* b) {
    if (stack_.size() < 2) return false;
    *b = stack_.back();
    stack_.pop_back();
    *a = stack_.back();
    stack_.pop_back();
    return true;
  }

  ScriptStatus DoCall(const Insn& in, ScriptError* err) {
    auto it = funcIndex_.find(in.name);
    if (it == funcIndex_.end()) {
      if (err) *err = ScriptError::Make("call to undefined function: " + in.name,
                                        chunk_, in.line);
      Abort();
      return ScriptStatus::Error;
    }
    if (callDepth_ + 1 > kMaxCallDepth) {
      if (err) *err = ScriptError::Make("call depth exceeded", chunk_, in.line);
      Abort();
      return ScriptStatus::Error;
    }
    if (in.argc < 0 || (size_t)in.argc > stack_.size()) return Underflow(err);
    std::vector<ScriptValue> args((size_t)in.argc);
    for (i32 i = in.argc - 1; i >= 0; --i) {
      args[(size_t)i] = stack_.back();
      stack_.pop_back();
    }
    const Function& fn = functions_[it->second];
    // Bind parameters positionally; extra args dropped, missing = nil.
    for (size_t i = 0; i < fn.params.size(); ++i) {
      globals_[fn.params[i]] = i < args.size() ? args[i] : ScriptValue::Nil();
    }
    frames_.push_back(Frame{it->second, pc_ + 1, fn.end});
    ++callDepth_;
    pc_ = fn.entry;
    return ScriptStatus::Running;
  }

  ScriptStatus DoNative(const Insn& in, ScriptError* err) {
    if (!bindings_ || !ctx_) {
      if (err) *err = ScriptError::Make("no bindings available", chunk_, in.line);
      Abort();
      return ScriptStatus::Error;
    }
    if (in.argc < 0 || (size_t)in.argc > stack_.size()) return Underflow(err);
    std::vector<ScriptValue> args((size_t)in.argc);
    for (i32 i = in.argc - 1; i >= 0; --i) {
      args[(size_t)i] = stack_.back();
      stack_.pop_back();
    }
    bool ok = false, denied = false;
    ScriptValue r = bindings_->Invoke(*ctx_, in.name, args, &ok, &denied);
    if (!ok) {
      if (err) {
        *err = ScriptError::Make(
            denied ? ("capability denied for binding: " + in.name)
                   : ("no such binding: " + in.name),
            chunk_, in.line);
      }
      Abort();
      return denied ? ScriptStatus::CapabilityDenied : ScriptStatus::Error;
    }
    stack_.push_back(r);
    if (ctx_->yieldRequested()) {
      // The async layer asked to suspend: stop the call cleanly at this
      // boundary. State stays intact for a resume.
      inCall_ = false;
      return ScriptStatus::Yielded;
    }
    ++pc_;
    return ScriptStatus::Running;
  }

  ScriptStatus DoReturn(const Insn& in) {
    if (frames_.empty()) {
      inCall_ = false;
      return ScriptStatus::Ok;
    }
    // Collect return values (in declaration order).
    std::vector<ScriptValue> values;
    for (i32 i = 0; i < in.argc; ++i) {
      if (stack_.empty()) break;
      values.push_back(stack_.back());
      stack_.pop_back();
    }
    std::reverse(values.begin(), values.end());

    Frame fr = frames_.back();
    frames_.pop_back();
    pc_ = fr.returnPc;

    if (frames_.empty()) {
      // Top-level return: publish the result.
      resultValues_ = values;
      inCall_ = false;
      return ScriptStatus::Ok;
    }
    --callDepth_;
    // A call expression yields exactly one value.
    stack_.push_back(values.empty() ? ScriptValue::Nil() : values.front());
    return ScriptStatus::Running;
  }

  static constexpr size_t kMaxCallDepth = 64;

  std::string chunk_;
  std::vector<Insn> program_;
  std::vector<Function> functions_;
  std::unordered_map<std::string, size_t> funcIndex_;
  std::unordered_map<std::string, ScriptValue> globals_;
  std::vector<ScriptValue> stack_;
  std::vector<Frame> frames_;
  std::vector<ScriptValue> resultValues_;
  std::vector<ScriptValue> result_;
  ScriptLimits limits_;
  const ScriptBindings* bindings_ = nullptr;
  ScriptContext* ctx_ = nullptr;
  size_t pc_ = 0;
  size_t callDepth_ = 0;
  u64 instructions_ = 0;
  bool loaded_ = false;
  bool inCall_ = false;
  bool interrupt_ = false;
};

}  // namespace

IScriptVm* CreateReferenceVm() { return new RefVm(); }

}  // namespace Berry
