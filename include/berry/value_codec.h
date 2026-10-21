#pragma once
// Deterministic serialization of ScriptValue trees.
//
// Factored out of the reference VM so every VM behind IScriptVm serializes hot
// reload state the same way: a byte-stable, length-prefixed framing that
// round-trips nested tables (and strings that contain newlines or colons)
// exactly. The host may hand a snapshot written by one VM to a RestoreState on
// another, so the encoding is a property of the value layer, not of a VM.

#include "berry/export.h"
#include "berry/script.h"

#include <string>

namespace Berry {
namespace ValueCodec {

/// Encodes a value into the length-prefixed canonical form.
BERRY_API std::string Encode(const ScriptValue& v);

/// Decodes one value from the front of `s`, advancing `pos` past the consumed
/// bytes. Returns false on malformed input (never trusts the framing).
BERRY_API bool DecodeAt(const std::string& s, size_t* pos, ScriptValue* out);

/// Decodes one value from the front of `s`.
BERRY_API bool Decode(const std::string& s, ScriptValue* out);

/// Approximate resident size of a value tree, used by VM memory accounting.
BERRY_API size_t Bytes(const ScriptValue& v);

}  // namespace ValueCodec
}  // namespace Berry
