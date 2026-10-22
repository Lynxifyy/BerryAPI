#pragma once
// Internal minimal JSON writer.
//
// The project has a JSON parser (json_min.h) but no writer. Two subsystems now
// need to serialise structured data (the record tape, the local RPC server), so
// the writer lives here rather than being duplicated.
//
// Deliberately small and dependency-free, and deterministic: callers write
// members in a fixed order and numbers use a fixed precision, so the same value
// always produces the same bytes. This header is internal (not part of the
// public SDK surface) and therefore lives under core/.

#include "berry/types.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "json_min.h"

namespace Berry {
namespace detail {
namespace jsonw {

/// Appends `s` as a quoted, escaped JSON string.
inline void String(std::string* out, const std::string& s) {
  out->push_back('"');
  for (char c : s) {
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      case '\b': *out += "\\b"; break;
      case '\f': *out += "\\f"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
          *out += buf;
        } else {
          out->push_back(c);
        }
    }
  }
  out->push_back('"');
}

/// Appends a finite number. A non-finite value is written as 0, because JSON has
/// no representation for it and a caller that produced one has a bug.
inline void Number(std::string* out, f64 v) {
  if (!std::isfinite(v)) { *out += "0"; return; }
  char buf[64];
  // %.9g keeps a float round-trippable and drops trailing zeros, so a whole
  // number prints as "1" and not "1.000000000".
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  *out += buf;
}

inline void Bool(std::string* out, bool v) { *out += v ? "true" : "false"; }
inline void Int(std::string* out, i64 v) { *out += std::to_string(v); }
inline void Uint(std::string* out, u64 v) { *out += std::to_string(v); }
inline void Null(std::string* out) { *out += "null"; }

/// A writer that tracks the separators, so a caller does not have to. Writes a
/// JSON object or array without building an intermediate tree.
class Writer {
 public:
  explicit Writer(std::string* out) : out_(out) {}

  void BeginObject() { Sep(); out_->push_back('{'); first_.push_back(true); }
  void BeginArray() { Sep(); out_->push_back('['); first_.push_back(true); }
  void EndObject() { out_->push_back('}'); first_.pop_back(); }
  void EndArray() { out_->push_back(']'); first_.pop_back(); }

  void Key(const std::string& k) {
    Sep();
    jsonw::String(out_, k);
    out_->push_back(':');
    pendingKey_ = true;
  }

  void String(const std::string& v) { Sep(); jsonw::String(out_, v); }
  void Number(f64 v) { Sep(); jsonw::Number(out_, v); }
  void Int(i64 v) { Sep(); jsonw::Int(out_, v); }
  void Uint(u64 v) { Sep(); jsonw::Uint(out_, v); }
  void Bool(bool v) { Sep(); jsonw::Bool(out_, v); }
  void Null() { Sep(); jsonw::Null(out_); }

  /// Emits a key and a value in one call.
  void Member(const std::string& k, const std::string& v) { Key(k); String(v); }
  void Member(const std::string& k, f64 v) { Key(k); Number(v); }
  void Member(const std::string& k, i64 v) { Key(k); Int(v); }
  void Member(const std::string& k, u64 v) { Key(k); Uint(v); }
  void Member(const std::string& k, bool v) { Key(k); Bool(v); }

  /// Appends pre-serialised JSON verbatim. Used for a params blob the server
  /// does not need to understand.
  void Raw(const std::string& json) { Sep(); *out_ += json; }

  size_t depth() const { return first_.size(); }

 private:
  // A value that follows a key is written with no separator; every other value
  // needs a comma unless it is the first in its container.
  void Sep() {
    if (pendingKey_) { pendingKey_ = false; return; }
    if (first_.empty()) return;   // a top-level value
    if (first_.back()) { first_.back() = false; return; }
    out_->push_back(',');
  }

  std::string* out_;
  std::vector<bool> first_;
  bool pendingKey_ = false;
};

namespace impl {
inline void DumpValue(std::string* out, const ::Berry::detail::json::Value& v) {
  using ::Berry::detail::json::Type;
  switch (v.type) {
    case Type::Null: Null(out); break;
    case Type::Bool: Bool(out, v.boolean); break;
    case Type::Number: Number(out, v.number); break;
    case Type::String: String(out, v.str); break;
    case Type::Array: {
      out->push_back('[');
      for (size_t i = 0; i < v.array.size(); ++i) {
        if (i) out->push_back(',');
        DumpValue(out, v.array[i]);
      }
      out->push_back(']');
      break;
    }
    case Type::Object: {
      out->push_back('{');
      for (size_t i = 0; i < v.object.size(); ++i) {
        if (i) out->push_back(',');
        String(out, v.object[i].first);
        out->push_back(':');
        DumpValue(out, v.object[i].second);
      }
      out->push_back('}');
      break;
    }
  }
}
}  // namespace impl

/// Re-serialises a parsed tree. The parser preserves object key order, so this
/// round-trips a document's ordering; it does not preserve the original
/// whitespace, which no consumer here depends on.
inline std::string Dump(const ::Berry::detail::json::Value& v) {
  std::string out;
  impl::DumpValue(&out, v);
  return out;
}

}  // namespace jsonw
}  // namespace detail
}  // namespace Berry
