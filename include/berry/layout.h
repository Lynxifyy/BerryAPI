#pragma once
// Struct layouts described as data.
//
// A hooked function hands us a pointer to a game object, and reading a field of
// it means knowing an offset — which changes whenever the game is rebuilt. This
// lets that offset live in a JSON table next to a signature, so a new client
// build is a data update rather than a recompile.
//
// The table is per-version and gated by a build fingerprint, exactly like the
// signature table: if the running build is not the one the layout was written
// against, every field read fails closed rather than reading a stale offset out
// of a foreign struct. Reading the wrong offset is worse than reading nothing.
//
// Decoding is deliberately narrow and explicit. A field declares its type, so a
// read is a bounded, checked copy — never a blind reinterpret of memory as
// whatever the caller hoped was there:
//
//   * every read is bounds-checked against the struct's declared size
//   * a pointer is only dereferenced by an explicit follow, one hop at a time
//   * a string is copied with a cap, and a missing terminator is an error
//   * nothing here writes to game memory
//
// Pure data + arithmetic, no game pointers, so the whole thing runs on the host
// under ASan with a byte buffer standing in for a struct.

#include "berry/export.h"
#include "berry/math.h"
#include "berry/types.h"

#include <mutex>
#include <string>
#include <vector>

namespace Berry {

//----------------------------------------------------------------------------
// Field types
//----------------------------------------------------------------------------

/// The scalar kinds a field may declare. Anything else is refused: a layout
/// cannot describe an unaligned or otherwise ambiguous read.
enum class FieldType : u32 {
  I8 = 0,
  U8,
  I16,
  U16,
  I32,
  U32,
  I64,
  U64,
  F32,
  F64,
  Bool,      // one byte, 0 = false, anything else = true
  Vec3,      // three f32
  Pointer,   // a target-width pointer, read as an integer
  EntityId,  // a 64-bit entity id
};

const char* FieldTypeName(FieldType t);
/// Parses a type name ("u32", "vec3", ...). Returns false for an unknown name.
bool ParseFieldType(const std::string& name, FieldType* out);
/// The width in bytes, or 0 for an unknown type.
size_t FieldTypeSize(FieldType t);

//----------------------------------------------------------------------------
// Layout
//----------------------------------------------------------------------------

enum class LayoutError : u32 {
  Ok = 0,
  BadJson,
  MissingName,
  BadName,
  MissingVersion,
  MissingSize,
  BadSize,
  NoFields,
  TooManyFields,
  BadField,          // a field is not an object, or a required member is absent
  BadOffset,
  BadFieldType,
  Overlap,           // two fields occupy the same bytes
  OutOfRange,        // a field extends past the declared struct size
  BadBuild,          // the `build` fingerprint is absent or malformed
  DuplicateField,
};

const char* LayoutErrorName(LayoutError e);

/// One field of a struct: where it starts and what it is.
struct BERRY_API LayoutField {
  std::string name;
  FieldType type = FieldType::U32;
  u32 offset = 0;
};

/// A struct layout: a name, the build it was written against, the struct's
/// size, and its fields.
struct BERRY_API StructLayout {
  std::string name;
  std::string version;     // the game version this layout targets
  std::string build;       // the build fingerprint (see BuildLibrary)
  u32 size = 0;            // total struct size in bytes
  std::vector<LayoutField> fields;

  /// Parses and fully validates a layout document. Rejects an unknown key, a
  /// duplicate field, an overlapping field, a field that runs past `size`, a
  /// bad type and a bad offset — the whole document, or nothing.
  static bool Parse(const std::string& json, StructLayout* out,
                    LayoutError* outError = nullptr, std::string* error = nullptr);

  /// True for a struct/field name: a bounded identifier.
  static bool ValidName(const std::string& name);

  const LayoutField* Field(const std::string& name) const;
};

//----------------------------------------------------------------------------
// Decoding
//----------------------------------------------------------------------------

/// Why a read failed. Every one of these leaves the caller's output untouched.
enum class ReadError : u32 {
  Ok = 0,
  NoLayout,       // the field is not in the layout
  NullBase,       // a null struct pointer
  OutOfBounds,    // the field would read past the supplied buffer
  NotAnInteger,   // the field is not an integer type
  NotAFloat,      // the field is not a float type
  NotAPointer,    // the field is not a pointer type
  NotAVec3,       // the field is not a vec3
  BadString,      // no terminator within the cap
};

const char* ReadErrorName(ReadError e);

/// A bounds-checked reader over a byte buffer that stands in for a struct.
///
/// The reader never owns the bytes and never writes to them. `base` is the
/// struct's first byte; `available` is how many bytes may be read. A field read
/// is checked against both the layout's declared size and `available`, so a
/// short or truncated buffer fails rather than reading past it.
class BERRY_API StructReader {
 public:
  /// The most bytes a string field will copy before reporting `BadString`.
  static constexpr size_t kMaxStringBytes = 256;

  StructReader(const StructLayout* layout, const u8* base, size_t available);

  bool valid() const { return layout_ != nullptr && base_ != nullptr; }
  const StructLayout* layout() const { return layout_; }

  bool ReadI64(const std::string& field, i64* out) const;
  bool ReadU64(const std::string& field, u64* out) const;
  bool ReadF64(const std::string& field, f64* out) const;
  bool ReadVec3(const std::string& field, Vec3* out) const;
  bool ReadBool(const std::string& field, bool* out) const;
  /// Reads a field as a pointer-width integer. Does NOT dereference.
  bool ReadPointer(const std::string& field, uptr* out) const;
  /// Copies a NUL-terminated string with a cap. Fails when no terminator is
  /// found within `kMaxStringBytes` or the field's declared width.
  bool ReadString(const std::string& field, std::string* out) const;

  /// The error from the last failed read, or Ok.
  ReadError lastError() const { return last_; }

 private:
  const u8* FieldBytes(const std::string& field, size_t width,
                       ReadError* err) const;

  const StructLayout* layout_ = nullptr;
  const u8* base_ = nullptr;
  size_t available_ = 0;
  mutable ReadError last_ = ReadError::Ok;
};

/// One pointer hop: reads a pointer field from `base` and returns the address
/// it holds. A caller chains hops explicitly, so a layout can never describe a
/// dereference the caller did not ask for.
bool FollowPointer(const StructReader& reader, const std::string& field,
                   uptr* out);

//----------------------------------------------------------------------------
// Registry
//----------------------------------------------------------------------------

/// The layout registry. Holds every parsed layout and resolves a field by
/// (struct, build) — a layout whose `build` does not match the active
/// fingerprint is not used. Thread-safe.
class BERRY_API LayoutRegistry {
 public:
  static constexpr size_t kMaxLayouts = 256;
  static constexpr size_t kMaxFields = 128;

  /// Registers a layout. Refused for a duplicate `name@build` or an invalid
  /// layout. The same struct at different builds is expected.
  bool Add(const StructLayout& layout, std::string* error = nullptr);

  /// Parses and registers. Returns the parse error on failure.
  LayoutError AddJson(const std::string& json, std::string* error = nullptr);

  /// Sets the active build fingerprint. A layout only resolves when its
  /// `build` equals this. An empty fingerprint disables every layout.
  void SetActiveBuild(std::string build);
  std::string activeBuild() const;

  bool Has(const std::string& name) const;
  /// Looks up `name` at the active build into `out`. False when there is no
  /// active fingerprint or no layout for it. This is the only lookup that
  /// yields a usable layout, so a caller cannot accidentally read a layout
  /// written for a different build. Copies, so the result stays valid across a
  /// later Add().
  bool Find(const std::string& name, StructLayout* out) const;
  /// The layout for an explicit build, for tooling and tests.
  bool FindForBuild(const std::string& name, const std::string& build,
                    StructLayout* out) const;
  std::vector<std::string> Names() const;
  size_t Count() const;

  void Clear();

 private:
  mutable std::mutex mutex_;
  std::string activeBuild_;
  std::vector<StructLayout> layouts_;
};

}  // namespace Berry
