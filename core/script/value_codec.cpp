// Deterministic ScriptValue serialization. See include/berry/value_codec.h.
#include "berry/value_codec.h"

#include <cstdio>
#include <cstdlib>

namespace Berry {
namespace ValueCodec {

size_t Bytes(const ScriptValue& v) {
  size_t bytes = v.string.size();
  if (v.type == ScriptValue::Type::Table && v.table) {
    for (const auto& kv : *v.table) {
      bytes += kv.first.size() + sizeof(ScriptValue) + Bytes(kv.second);
    }
  }
  return bytes;
}

std::string Encode(const ScriptValue& v) {
  switch (v.type) {
    case ScriptValue::Type::Nil: return "nil";
    case ScriptValue::Type::Bool: return v.boolean ? "true" : "false";
    case ScriptValue::Type::Number: {
      // Length-prefixed like strings/tables so a number nested inside a
      // table decodes without relying on a line terminator.
      const std::string d = v.ToDisplay();
      return "n:" + std::to_string(d.size()) + ":" + d;
    }
    case ScriptValue::Type::String: {
      // Length-prefixed so embedded newlines/commas survive round-trip.
      return "s:" + std::to_string(v.string.size()) + ":" + v.string;
    }
    case ScriptValue::Type::Table: {
      // Length-prefixed recursive framing: "<klen>:<key><encvalue>".
      // A table's encoded length is written first so a nested value that
      // itself contains colons still decodes unambiguously.
      std::string body;
      if (v.table) {
        for (const auto& kv : *v.table) {
          const std::string ev = Encode(kv.second);
          body += std::to_string(kv.first.size()) + ":" + kv.first + ev;
        }
      }
      return "t:" + std::to_string(body.size()) + ":" + body;
    }
  }
  return "nil";
}

/// Decodes one value starting at `pos`, advancing it past the consumed
/// bytes. Framing is length-prefixed throughout so nested tables round-trip
/// byte-for-byte.
bool DecodeAt(const std::string& s, size_t* pos, ScriptValue* out) {
  size_t& p = *pos;
  if (p >= s.size()) return false;
  if (s.compare(p, 3, "nil") == 0) { *out = ScriptValue::Nil(); p += 3; return true; }
  if (s.compare(p, 4, "true") == 0) { *out = ScriptValue::Bool(true); p += 4; return true; }
  if (s.compare(p, 5, "false") == 0) { *out = ScriptValue::Bool(false); p += 5; return true; }
  if (s.compare(p, 2, "n:") == 0) {
    size_t len = 0;
    int adv = 0;
    if (std::sscanf(s.c_str() + p + 2, "%zu:%n", &len, &adv) != 1) return false;
    const size_t bodyStart = p + 2 + (size_t)adv;
    if (bodyStart + len > s.size()) return false;
    // Parse from a NUL-terminated copy: strtod would otherwise read on past
    // the length-prefixed slice into the following key.
    *out = ScriptValue::Num(std::strtod(s.substr(bodyStart, len).c_str(), nullptr));
    p = bodyStart + len;
    return true;
  }
  if (s.compare(p, 2, "s:") == 0) {
    size_t len = 0;
    int adv = 0;
    if (std::sscanf(s.c_str() + p + 2, "%zu:%n", &len, &adv) != 1) return false;
    const size_t bodyStart = p + 2 + (size_t)adv;
    if (bodyStart + len > s.size()) return false;
    *out = ScriptValue::Str(s.substr(bodyStart, len));
    p = bodyStart + len;
    return true;
  }
  if (s.compare(p, 2, "t:") == 0) {
    size_t len = 0;
    int adv = 0;
    if (std::sscanf(s.c_str() + p + 2, "%zu:%n", &len, &adv) != 1) return false;
    const size_t bodyStart = p + 2 + (size_t)adv;
    if (bodyStart + len > s.size()) return false;
    ScriptTable tbl;
    size_t q = bodyStart;
    while (q < bodyStart + len) {
      size_t klen = 0;
      int kadv = 0;
      if (std::sscanf(s.c_str() + q, "%zu:%n", &klen, &kadv) != 1) return false;
      const size_t keyStart = q + (size_t)kadv;
      if (keyStart + klen > bodyStart + len) return false;
      const std::string key = s.substr(keyStart, klen);
      q = keyStart + klen;
      ScriptValue child;
      if (!DecodeAt(s, &q, &child)) return false;
      tbl[key] = std::move(child);
    }
    *out = ScriptValue::Tbl(std::move(tbl));
    p = bodyStart + len;
    return true;
  }
  return false;
}

bool Decode(const std::string& s, ScriptValue* out) {
  size_t pos = 0;
  return DecodeAt(s, &pos, out);
}

}  // namespace ValueCodec
}  // namespace Berry
