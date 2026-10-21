#pragma once
// Internal minimal JSON parser for BerryAPI's data-driven subsystems
// (script manifests, signature packs, package manifests).
//
// Deliberately small and dependency-free. Fail closed: a malformed document,
// a nesting depth over the cap, or a duplicate object key is rejected rather
// than silently coerced. Object key order is preserved so generated output is
// deterministic.
//
// This header is internal (not part of the public SDK surface) and therefore
// lives under core/ rather than include/berry/.

#include "berry/types.h"

#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace Berry {
namespace detail {
namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
  Type type = Type::Null;
  bool boolean = false;
  f64 number = 0;
  std::string str;
  std::vector<Value> array;
  std::vector<std::pair<std::string, Value>> object;  // order-preserving

  bool IsNull() const { return type == Type::Null; }
  bool IsBool() const { return type == Type::Bool; }
  bool IsNumber() const { return type == Type::Number; }
  bool IsString() const { return type == Type::String; }
  bool IsArray() const { return type == Type::Array; }
  bool IsObject() const { return type == Type::Object; }

  /// Object member lookup; returns nullptr when absent or not an object.
  const Value* Get(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& kv : object) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }

  size_t Size() const {
    if (type == Type::Array) return array.size();
    if (type == Type::Object) return object.size();
    return 0;
  }
};

namespace detail_impl {

inline void SkipWs(const char*& p, const char* end) {
  while (p < end) {
    char c = *p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++p;
      continue;
    }
    break;
  }
}

inline bool ParseString(const char*& p, const char* end, std::string* out,
                        std::string* error) {
  if (p >= end || *p != '"') {
    if (error) *error = "expected string";
    return false;
  }
  ++p;
  out->clear();
  while (p < end) {
    char c = *p++;
    if (c == '"') return true;
    if (c == '\\') {
      if (p >= end) break;
      char e = *p++;
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          if (end - p < 4) {
            if (error) *error = "truncated \\u escape";
            return false;
          }
          u32 cp = 0;
          for (int i = 0; i < 4; ++i) {
            char h = *p++;
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= (u32)(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= (u32)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= (u32)(h - 'A' + 10);
            else {
              if (error) *error = "bad hex in \\u escape";
              return false;
            }
          }
          // Encode as UTF-8 (BMP only; surrogate pairs are left as-is).
          if (cp < 0x80) {
            out->push_back((char)cp);
          } else if (cp < 0x800) {
            out->push_back((char)(0xC0 | (cp >> 6)));
            out->push_back((char)(0x80 | (cp & 0x3F)));
          } else {
            out->push_back((char)(0xE0 | (cp >> 12)));
            out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back((char)(0x80 | (cp & 0x3F)));
          }
          break;
        }
        default:
          if (error) *error = "invalid escape";
          return false;
      }
      continue;
    }
    out->push_back(c);
  }
  if (error) *error = "unterminated string";
  return false;
}

inline bool ParseValue(const char*& p, const char* end, int depth, Value* out,
                       std::string* error);

inline bool ParseNumber(const char*& p, const char* end, f64* out,
                        std::string* error) {
  const char* start = p;
  while (p < end) {
    char c = *p;
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
        c == 'e' || c == 'E') {
      ++p;
      continue;
    }
    break;
  }
  if (p == start) {
    if (error) *error = "invalid number";
    return false;
  }
  std::string tok(start, (size_t)(p - start));
  char* endp = nullptr;
  double v = std::strtod(tok.c_str(), &endp);
  if (endp != tok.c_str() + tok.size()) {
    if (error) *error = "invalid number literal";
    return false;
  }
  *out = (f64)v;
  return true;
}

inline bool ParseObject(const char*& p, const char* end, int depth, Value* out,
                        std::string* error) {
  ++p;  // consume '{'
  out->type = Type::Object;
  SkipWs(p, end);
  if (p < end && *p == '}') {
    ++p;
    return true;
  }
  for (;;) {
    SkipWs(p, end);
    std::string key;
    if (!ParseString(p, end, &key, error)) return false;
    for (const auto& kv : out->object) {
      if (kv.first == key) {
        if (error) *error = "duplicate object key: " + key;
        return false;
      }
    }
    SkipWs(p, end);
    if (p >= end || *p != ':') {
      if (error) *error = "expected ':'";
      return false;
    }
    ++p;
    SkipWs(p, end);
    Value child;
    if (!ParseValue(p, end, depth + 1, &child, error)) return false;
    out->object.emplace_back(std::move(key), std::move(child));
    SkipWs(p, end);
    if (p >= end) {
      if (error) *error = "unterminated object";
      return false;
    }
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p == '}') {
      ++p;
      return true;
    }
    if (error) *error = "expected ',' or '}'";
    return false;
  }
}

inline bool ParseArray(const char*& p, const char* end, int depth, Value* out,
                       std::string* error) {
  ++p;  // consume '['
  out->type = Type::Array;
  SkipWs(p, end);
  if (p < end && *p == ']') {
    ++p;
    return true;
  }
  for (;;) {
    SkipWs(p, end);
    Value child;
    if (!ParseValue(p, end, depth + 1, &child, error)) return false;
    out->array.push_back(std::move(child));
    SkipWs(p, end);
    if (p >= end) {
      if (error) *error = "unterminated array";
      return false;
    }
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p == ']') {
      ++p;
      return true;
    }
    if (error) *error = "expected ',' or ']'";
    return false;
  }
}

inline bool ParseValue(const char*& p, const char* end, int depth, Value* out,
                       std::string* error) {
  if (depth > 64) {
    if (error) *error = "nesting too deep";
    return false;
  }
  SkipWs(p, end);
  if (p >= end) {
    if (error) *error = "unexpected end of input";
    return false;
  }
  char c = *p;
  switch (c) {
    case '{':
      return ParseObject(p, end, depth, out, error);
    case '[':
      return ParseArray(p, end, depth, out, error);
    case '"':
      out->type = Type::String;
      return ParseString(p, end, &out->str, error);
    case 't':
      if (end - p >= 4 && std::string(p, p + 4) == "true") {
        p += 4;
        out->type = Type::Bool;
        out->boolean = true;
        return true;
      }
      if (error) *error = "invalid literal";
      return false;
    case 'f':
      if (end - p >= 5 && std::string(p, p + 5) == "false") {
        p += 5;
        out->type = Type::Bool;
        out->boolean = false;
        return true;
      }
      if (error) *error = "invalid literal";
      return false;
    case 'n':
      if (end - p >= 4 && std::string(p, p + 4) == "null") {
        p += 4;
        out->type = Type::Null;
        return true;
      }
      if (error) *error = "invalid literal";
      return false;
    default:
      out->type = Type::Number;
      return ParseNumber(p, end, &out->number, error);
  }
}

}  // namespace detail_impl

/// Parses a complete JSON document. Trailing non-whitespace is an error, so a
/// truncated or concatenated document is rejected (fail closed).
inline bool Parse(const char* text, size_t len, Value* out,
                  std::string* error = nullptr) {
  if (!text || !out) {
    if (error) *error = "null input";
    return false;
  }
  const char* p = text;
  const char* end = text + len;
  if (!detail_impl::ParseValue(p, end, 0, out, error)) return false;
  detail_impl::SkipWs(p, end);
  if (p != end) {
    if (error) *error = "trailing data after JSON value";
    return false;
  }
  return true;
}

inline bool Parse(const std::string& text, Value* out,
                  std::string* error = nullptr) {
  return Parse(text.data(), text.size(), out, error);
}

}  // namespace json
}  // namespace detail
}  // namespace Berry
