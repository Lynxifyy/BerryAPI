// Struct layouts described as data. See include/berry/layout.h for the model.
#include "berry/layout.h"
#include "json_min.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

namespace Berry {

namespace {

bool ReadInt(const detail::json::Value* v, u32* out) {
  if (!v || !v->IsNumber()) return false;
  if (v->number < 0) return false;
  const f64 n = v->number;
  if (n > 4294967295.0) return false;
  if (n != (f64)(u64)n) return false;   // reject a fractional value
  *out = (u32)(u64)n;
  return true;
}

}  // namespace

const char* FieldTypeName(FieldType t) {
  switch (t) {
    case FieldType::I8: return "i8";
    case FieldType::U8: return "u8";
    case FieldType::I16: return "i16";
    case FieldType::U16: return "u16";
    case FieldType::I32: return "i32";
    case FieldType::U32: return "u32";
    case FieldType::I64: return "i64";
    case FieldType::U64: return "u64";
    case FieldType::F32: return "f32";
    case FieldType::F64: return "f64";
    case FieldType::Bool: return "bool";
    case FieldType::Vec3: return "vec3";
    case FieldType::Pointer: return "ptr";
    case FieldType::EntityId: return "entityid";
  }
  return "?";
}

bool ParseFieldType(const std::string& name, FieldType* out) {
  static const struct { const char* n; FieldType t; } kTable[] = {
      {"i8", FieldType::I8},     {"u8", FieldType::U8},
      {"i16", FieldType::I16},   {"u16", FieldType::U16},
      {"i32", FieldType::I32},   {"u32", FieldType::U32},
      {"i64", FieldType::I64},   {"u64", FieldType::U64},
      {"f32", FieldType::F32},   {"f64", FieldType::F64},
      {"bool", FieldType::Bool}, {"vec3", FieldType::Vec3},
      {"ptr", FieldType::Pointer}, {"entityid", FieldType::EntityId},
  };
  for (const auto& e : kTable) {
    if (name == e.n) { *out = e.t; return true; }
  }
  return false;
}

size_t FieldTypeSize(FieldType t) {
  switch (t) {
    case FieldType::I8:
    case FieldType::U8:
    case FieldType::Bool: return 1;
    case FieldType::I16:
    case FieldType::U16: return 2;
    case FieldType::I32:
    case FieldType::U32:
    case FieldType::F32: return 4;
    case FieldType::I64:
    case FieldType::U64:
    case FieldType::F64:
    case FieldType::Pointer:
    case FieldType::EntityId: return 8;
    case FieldType::Vec3: return 12;
  }
  return 0;
}

const char* LayoutErrorName(LayoutError e) {
  switch (e) {
    case LayoutError::Ok: return "ok";
    case LayoutError::BadJson: return "bad json";
    case LayoutError::MissingName: return "missing name";
    case LayoutError::BadName: return "bad name";
    case LayoutError::MissingVersion: return "missing version";
    case LayoutError::MissingSize: return "missing size";
    case LayoutError::BadSize: return "bad size";
    case LayoutError::NoFields: return "no fields";
    case LayoutError::TooManyFields: return "too many fields";
    case LayoutError::BadField: return "bad field";
    case LayoutError::BadOffset: return "bad offset";
    case LayoutError::BadFieldType: return "bad field type";
    case LayoutError::Overlap: return "overlapping fields";
    case LayoutError::OutOfRange: return "field past struct end";
    case LayoutError::BadBuild: return "bad build fingerprint";
    case LayoutError::DuplicateField: return "duplicate field";
  }
  return "unknown";
}

bool StructLayout::ValidName(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  if (!std::isalpha((unsigned char)name[0]) && name[0] != '_') return false;
  for (char c : name) {
    const unsigned char u = (unsigned char)c;
    if (!std::isalnum(u) && c != '_' && c != '.') return false;
  }
  return true;
}

bool StructLayout::Parse(const std::string& json, StructLayout* out,
                         LayoutError* outError, std::string* error) {
  auto fail = [&](LayoutError e, const std::string& detail) {
    if (outError) *outError = e;
    if (error) *error = detail;
    return false;
  };

  detail::json::Value root;
  std::string jerr;
  if (!detail::json::Parse(json, &root, &jerr)) {
    return fail(LayoutError::BadJson, jerr);
  }
  if (!root.IsObject()) return fail(LayoutError::BadJson, "root is not an object");

  static const char* kKnown[] = {"name", "version", "build", "size", "fields"};
  for (const auto& kv : root.object) {
    bool known = false;
    for (const char* k : kKnown) {
      if (kv.first == k) { known = true; break; }
    }
    if (!known) return fail(LayoutError::BadJson, "unknown key '" + kv.first + "'");
  }

  const detail::json::Value* name = root.Get("name");
  if (!name || !name->IsString() || name->str.empty()) {
    return fail(LayoutError::MissingName, "missing or empty 'name'");
  }
  if (!ValidName(name->str)) {
    return fail(LayoutError::BadName, "bad name '" + name->str + "'");
  }
  const detail::json::Value* version = root.Get("version");
  if (!version || !version->IsString() || version->str.empty()) {
    return fail(LayoutError::MissingVersion, "missing or empty 'version'");
  }
  const detail::json::Value* build = root.Get("build");
  if (!build || !build->IsString() || build->str.empty()) {
    return fail(LayoutError::BadBuild, "missing or empty 'build'");
  }
  const detail::json::Value* size = root.Get("size");
  u32 structSize = 0;
  if (!size) return fail(LayoutError::MissingSize, "missing 'size'");
  if (!ReadInt(size, &structSize) || structSize == 0) {
    return fail(LayoutError::BadSize, "'size' is not a positive integer");
  }
  const detail::json::Value* fields = root.Get("fields");
  if (!fields || !fields->IsArray() || fields->array.empty()) {
    return fail(LayoutError::NoFields, "missing or empty 'fields'");
  }
  if (fields->array.size() > LayoutRegistry::kMaxFields) {
    return fail(LayoutError::TooManyFields, "too many fields");
  }

  StructLayout m;
  m.name = name->str;
  m.version = version->str;
  m.build = build->str;
  m.size = structSize;

  std::set<std::string> seen;
  // Occupancy map, so an overlap is caught even between differently sized
  // fields (a u8 at offset 0 and a u32 at offset 0 both touch byte 0).
  std::vector<bool> used(structSize, false);

  for (const auto& fv : fields->array) {
    if (!fv.IsObject()) return fail(LayoutError::BadField, "field is not an object");
    static const char* kFieldKnown[] = {"name", "type", "offset"};
    for (const auto& kv : fv.object) {
      bool known = false;
      for (const char* k : kFieldKnown) {
        if (kv.first == k) { known = true; break; }
      }
      if (!known) return fail(LayoutError::BadField, "unknown field key '" + kv.first + "'");
    }
    const detail::json::Value* fn = fv.Get("name");
    if (!fn || !fn->IsString() || fn->str.empty()) {
      return fail(LayoutError::BadField, "field is missing 'name'");
    }
    if (!ValidName(fn->str)) {
      return fail(LayoutError::BadField, "bad field name '" + fn->str + "'");
    }
    if (!seen.insert(fn->str).second) {
      return fail(LayoutError::DuplicateField, "duplicate field '" + fn->str + "'");
    }
    const detail::json::Value* ft = fv.Get("type");
    if (!ft || !ft->IsString()) {
      return fail(LayoutError::BadFieldType, "field '" + fn->str + "' is missing 'type'");
    }
    FieldType type = FieldType::U32;
    if (!ParseFieldType(ft->str, &type)) {
      return fail(LayoutError::BadFieldType,
                  "field '" + fn->str + "' has unknown type '" + ft->str + "'");
    }
    const detail::json::Value* fo = fv.Get("offset");
    u32 offset = 0;
    if (!fo) return fail(LayoutError::BadOffset, "field '" + fn->str + "' is missing 'offset'");
    if (!ReadInt(fo, &offset)) {
      return fail(LayoutError::BadOffset, "field '" + fn->str + "' has a bad offset");
    }
    const size_t width = FieldTypeSize(type);
    if (offset >= structSize || offset + width > structSize) {
      return fail(LayoutError::OutOfRange,
                  "field '" + fn->str + "' runs past the struct end");
    }
    for (size_t i = offset; i < offset + width; ++i) {
      if (used[i]) {
        return fail(LayoutError::Overlap, "field '" + fn->str + "' overlaps another");
      }
    }
    for (size_t i = offset; i < offset + width; ++i) used[i] = true;

    LayoutField lf;
    lf.name = fn->str;
    lf.type = type;
    lf.offset = offset;
    m.fields.push_back(std::move(lf));
  }

  *out = std::move(m);
  if (outError) *outError = LayoutError::Ok;
  if (error) error->clear();
  return true;
}

const LayoutField* StructLayout::Field(const std::string& name) const {
  for (const auto& f : fields) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

//----------------------------------------------------------------------------
// Reader
//----------------------------------------------------------------------------

const char* ReadErrorName(ReadError e) {
  switch (e) {
    case ReadError::Ok: return "ok";
    case ReadError::NoLayout: return "no such field";
    case ReadError::NullBase: return "null struct pointer";
    case ReadError::OutOfBounds: return "read past buffer";
    case ReadError::NotAnInteger: return "field is not an integer";
    case ReadError::NotAFloat: return "field is not a float";
    case ReadError::NotAPointer: return "field is not a pointer";
    case ReadError::NotAVec3: return "field is not a vec3";
    case ReadError::BadString: return "string has no terminator";
  }
  return "unknown";
}

StructReader::StructReader(const StructLayout* layout, const u8* base,
                           size_t available)
    : layout_(layout), base_(base), available_(available) {}

const u8* StructReader::FieldBytes(const std::string& field, size_t width,
                                   ReadError* err) const {
  last_ = ReadError::Ok;
  if (!layout_ || !base_) {
    last_ = ReadError::NullBase;
    if (err) *err = last_;
    return nullptr;
  }
  const LayoutField* f = layout_->Field(field);
  if (!f) {
    last_ = ReadError::NoLayout;
    if (err) *err = last_;
    return nullptr;
  }
  const size_t declared = FieldTypeSize(f->type);
  if (width > declared) {
    // The caller asked for more than the field is: a type mismatch, reported
    // as a bounds problem so a reader can never over-read a narrower field.
    last_ = ReadError::OutOfBounds;
    if (err) *err = last_;
    return nullptr;
  }
  // Check against both the declared struct size and the supplied buffer.
  if (f->offset + width > layout_->size || f->offset + width > available_) {
    last_ = ReadError::OutOfBounds;
    if (err) *err = last_;
    return nullptr;
  }
  return base_ + f->offset;
}

bool StructReader::ReadI64(const std::string& field, i64* out) const {
  ReadError err = ReadError::Ok;
  const LayoutField* f = layout_ ? layout_->Field(field) : nullptr;
  if (!f) {
    last_ = layout_ && base_ ? ReadError::NoLayout : ReadError::NullBase;
    return false;
  }
  const size_t width = FieldTypeSize(f->type);
  const u8* p = FieldBytes(field, width, &err);
  if (!p) return false;
  i64 v = 0;
  switch (f->type) {
    case FieldType::I8: { i8 t; std::memcpy(&t, p, 1); v = t; break; }
    case FieldType::U8: { u8 t; std::memcpy(&t, p, 1); v = t; break; }
    case FieldType::Bool: { u8 t; std::memcpy(&t, p, 1); v = t ? 1 : 0; break; }
    case FieldType::I16: { i16 t; std::memcpy(&t, p, 2); v = t; break; }
    case FieldType::U16: { u16 t; std::memcpy(&t, p, 2); v = t; break; }
    case FieldType::I32: { i32 t; std::memcpy(&t, p, 4); v = t; break; }
    case FieldType::U32: { u32 t; std::memcpy(&t, p, 4); v = t; break; }
    case FieldType::I64:
    case FieldType::U64:
    case FieldType::Pointer:
    case FieldType::EntityId: { i64 t; std::memcpy(&t, p, 8); v = t; break; }
    default:
      last_ = ReadError::NotAnInteger;
      return false;
  }
  *out = v;
  return true;
}

bool StructReader::ReadU64(const std::string& field, u64* out) const {
  i64 v = 0;
  if (!ReadI64(field, &v)) return false;
  *out = (u64)v;
  return true;
}

bool StructReader::ReadF64(const std::string& field, f64* out) const {
  ReadError err = ReadError::Ok;
  const LayoutField* f = layout_ ? layout_->Field(field) : nullptr;
  if (!f) {
    last_ = layout_ && base_ ? ReadError::NoLayout : ReadError::NullBase;
    return false;
  }
  if (f->type != FieldType::F32 && f->type != FieldType::F64) {
    last_ = ReadError::NotAFloat;
    return false;
  }
  const size_t width = FieldTypeSize(f->type);
  const u8* p = FieldBytes(field, width, &err);
  if (!p) return false;
  if (f->type == FieldType::F32) {
    f32 t; std::memcpy(&t, p, 4); *out = t;
  } else {
    f64 t; std::memcpy(&t, p, 8); *out = t;
  }
  return true;
}

bool StructReader::ReadVec3(const std::string& field, Vec3* out) const {
  ReadError err = ReadError::Ok;
  const LayoutField* f = layout_ ? layout_->Field(field) : nullptr;
  if (!f) {
    last_ = layout_ && base_ ? ReadError::NoLayout : ReadError::NullBase;
    return false;
  }
  if (f->type != FieldType::Vec3) {
    last_ = ReadError::NotAVec3;
    return false;
  }
  const u8* p = FieldBytes(field, 12, &err);
  if (!p) return false;
  f32 xyz[3];
  std::memcpy(xyz, p, 12);
  *out = Vec3(xyz[0], xyz[1], xyz[2]);
  return true;
}

bool StructReader::ReadBool(const std::string& field, bool* out) const {
  i64 v = 0;
  if (!ReadI64(field, &v)) return false;
  *out = (v != 0);
  return true;
}

bool StructReader::ReadPointer(const std::string& field, uptr* out) const {
  ReadError err = ReadError::Ok;
  const LayoutField* f = layout_ ? layout_->Field(field) : nullptr;
  if (!f) {
    last_ = layout_ && base_ ? ReadError::NoLayout : ReadError::NullBase;
    return false;
  }
  if (f->type != FieldType::Pointer) {
    last_ = ReadError::NotAPointer;
    return false;
  }
  const u8* p = FieldBytes(field, 8, &err);
  if (!p) return false;
  uptr v = 0;
  std::memcpy(&v, p, sizeof(uptr));
  *out = v;
  return true;
}

bool StructReader::ReadString(const std::string& field, std::string* out) const {
  last_ = ReadError::Ok;
  if (!layout_ || !base_) {
    last_ = ReadError::NullBase;
    return false;
  }
  const LayoutField* f = layout_->Field(field);
  if (!f) {
    last_ = ReadError::NoLayout;
    return false;
  }
  // A string field is a fixed byte region; copy up to the terminator, capped.
  const size_t declared = FieldTypeSize(f->type);
  if (declared != 1) {
    // Only byte-sized fields carry a string, so a string read cannot silently
    // walk across a neighbouring numeric field.
    last_ = ReadError::BadString;
    return false;
  }
  if (f->offset >= layout_->size || f->offset >= available_) {
    last_ = ReadError::OutOfBounds;
    return false;
  }
  const size_t limit = std::min<size_t>(layout_->size - f->offset,
                                        std::min<size_t>(available_ - f->offset,
                                                         kMaxStringBytes));
  const u8* p = base_ + f->offset;
  size_t n = 0;
  while (n < limit && p[n] != 0) ++n;
  if (n >= limit) {
    last_ = ReadError::BadString;
    return false;
  }
  out->assign(reinterpret_cast<const char*>(p), n);
  return true;
}

bool FollowPointer(const StructReader& reader, const std::string& field,
                   uptr* out) {
  return reader.ReadPointer(field, out);
}

//----------------------------------------------------------------------------
// Registry
//----------------------------------------------------------------------------

bool LayoutRegistry::Add(const StructLayout& layout, std::string* error) {
  if (!StructLayout::ValidName(layout.name)) {
    if (error) *error = "bad layout name '" + layout.name + "'";
    return false;
  }
  if (layout.build.empty()) {
    if (error) *error = "layout has no build fingerprint";
    return false;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  if (layouts_.size() >= kMaxLayouts) {
    if (error) *error = "layout registry full";
    return false;
  }
  for (const auto& l : layouts_) {
    if (l.name == layout.name && l.build == layout.build) {
      if (error) *error = "duplicate " + layout.name + "@" + layout.build;
      return false;
    }
  }
  layouts_.push_back(layout);
  return true;
}

LayoutError LayoutRegistry::AddJson(const std::string& json, std::string* error) {
  StructLayout l;
  LayoutError le = LayoutError::Ok;
  std::string detail;
  if (!StructLayout::Parse(json, &l, &le, &detail)) {
    if (error) *error = detail;
    return le;
  }
  if (!Add(l, error)) return LayoutError::BadName;
  return LayoutError::Ok;
}

void LayoutRegistry::SetActiveBuild(std::string build) {
  std::lock_guard<std::mutex> lk(mutex_);
  activeBuild_ = std::move(build);
}

std::string LayoutRegistry::activeBuild() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return activeBuild_;
}

bool LayoutRegistry::Has(const std::string& name) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& l : layouts_) {
    if (l.name == name) return true;
  }
  return false;
}

bool LayoutRegistry::Find(const std::string& name, StructLayout* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (activeBuild_.empty()) return false;   // no fingerprint: fail closed
  for (const auto& l : layouts_) {
    if (l.name == name && l.build == activeBuild_) {
      if (out) *out = l;
      return true;
    }
  }
  return false;
}

bool LayoutRegistry::FindForBuild(const std::string& name,
                                  const std::string& build,
                                  StructLayout* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& l : layouts_) {
    if (l.name == name && l.build == build) {
      if (out) *out = l;
      return true;
    }
  }
  return false;
}

std::vector<std::string> LayoutRegistry::Names() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::set<std::string> names;
  for (const auto& l : layouts_) names.insert(l.name);
  return std::vector<std::string>(names.begin(), names.end());
}

size_t LayoutRegistry::Count() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return layouts_.size();
}

void LayoutRegistry::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  layouts_.clear();
}

}  // namespace Berry
