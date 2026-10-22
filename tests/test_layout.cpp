// Host tests for struct layouts described as data.
//
// Covers:
//   * Field types: name parsing, widths.
//   * Layout parsing: required members, unknown keys, duplicate and overlapping
//     fields, a field past the struct end, and bad types/offsets.
//   * The reader: every scalar type, vec3, pointer, string, and the
//     bounds/type failures — including a truncated buffer and a string with no
//     terminator.
//   * The build gate: a layout only resolves at its own build fingerprint, and
//     an empty fingerprint disables everything.
//   * The registry: duplicate name@build, copy-on-find, names and counts.
#include "berry/layout.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

StructLayout Layout(const std::string& json) {
  StructLayout l;
  LayoutError le = LayoutError::Ok;
  std::string err;
  CHECK(StructLayout::Parse(json, &l, &le, &err));
  return l;
}

/// An actor-like layout: an id, a health float, a position, a name pointer and
/// a name string.
const char* kActorJson =
    "{"
    "\"name\":\"Actor\","
    "\"version\":\"1.21.0\","
    "\"build\":\"abc123\","
    "\"size\":64,"
    "\"fields\":["
    "  {\"name\":\"id\",\"type\":\"entityid\",\"offset\":0},"
    "  {\"name\":\"health\",\"type\":\"f32\",\"offset\":8},"
    "  {\"name\":\"alive\",\"type\":\"bool\",\"offset\":12},"
    "  {\"name\":\"flags\",\"type\":\"u8\",\"offset\":13},"
    "  {\"name\":\"pos\",\"type\":\"vec3\",\"offset\":16},"
    "  {\"name\":\"parent\",\"type\":\"ptr\",\"offset\":32},"
    "  {\"name\":\"name\",\"type\":\"u8\",\"offset\":48}"
    "]"
    "}";

}  // namespace

//----------------------------------------------------------------------------
// Types
//----------------------------------------------------------------------------

static void TestFieldTypes() {
  FieldType t;
  CHECK(ParseFieldType("u32", &t) && t == FieldType::U32);
  CHECK(ParseFieldType("f32", &t) && t == FieldType::F32);
  CHECK(ParseFieldType("vec3", &t) && t == FieldType::Vec3);
  CHECK(ParseFieldType("ptr", &t) && t == FieldType::Pointer);
  CHECK(ParseFieldType("entityid", &t) && t == FieldType::EntityId);
  CHECK(!ParseFieldType("u24", &t));
  CHECK(!ParseFieldType("", &t));

  CHECK(FieldTypeSize(FieldType::U8) == 1);
  CHECK(FieldTypeSize(FieldType::U16) == 2);
  CHECK(FieldTypeSize(FieldType::U32) == 4);
  CHECK(FieldTypeSize(FieldType::U64) == 8);
  CHECK(FieldTypeSize(FieldType::Pointer) == 8);
  CHECK(FieldTypeSize(FieldType::Vec3) == 12);
  CHECK(FieldTypeSize(FieldType::Bool) == 1);
}

//----------------------------------------------------------------------------
// Parsing
//----------------------------------------------------------------------------

static void TestLayoutParse() {
  StructLayout l;
  LayoutError le = LayoutError::Ok;
  std::string err;

  CHECK(StructLayout::Parse(kActorJson, &l, &le, &err));
  CHECK(l.name == "Actor" && l.version == "1.21.0" && l.build == "abc123");
  CHECK(l.size == 64);
  CHECK(l.fields.size() == 7);
  CHECK(l.Field("health") != nullptr);
  CHECK(l.Field("health")->offset == 8);
  CHECK(l.Field("health")->type == FieldType::F32);
  CHECK(l.Field("nope") == nullptr);

  // Missing members.
  CHECK(!StructLayout::Parse(
      "{\"version\":\"1\",\"build\":\"b\",\"size\":8,\"fields\":[]}",
      &l, &le, &err));
  CHECK(le == LayoutError::MissingName);
  CHECK(!StructLayout::Parse(
      "{\"name\":\"A\",\"build\":\"b\",\"size\":8,\"fields\":[{\"name\":\"x\",\"type\":\"u8\",\"offset\":0}]}",
      &l, &le, &err));
  CHECK(le == LayoutError::MissingVersion);
  CHECK(!StructLayout::Parse(
      "{\"name\":\"A\",\"version\":\"1\",\"size\":8,\"fields\":[{\"name\":\"x\",\"type\":\"u8\",\"offset\":0}]}",
      &l, &le, &err));
  CHECK(le == LayoutError::BadBuild);
  CHECK(!StructLayout::Parse(
      "{\"name\":\"A\",\"version\":\"1\",\"build\":\"b\",\"fields\":[{\"name\":\"x\",\"type\":\"u8\",\"offset\":0}]}",
      &l, &le, &err));
  CHECK(le == LayoutError::MissingSize);
  CHECK(!StructLayout::Parse(
      "{\"name\":\"A\",\"version\":\"1\",\"build\":\"b\",\"size\":0,\"fields\":[]}",
      &l, &le, &err));
  CHECK(le == LayoutError::BadSize);
  CHECK(!StructLayout::Parse(
      "{\"name\":\"A\",\"version\":\"1\",\"build\":\"b\",\"size\":8}",
      &l, &le, &err));
  CHECK(le == LayoutError::NoFields);
  CHECK(!StructLayout::Parse(
      "{\"name\":\"A\",\"version\":\"1\",\"build\":\"b\",\"size\":8,\"fields\":[]}",
      &l, &le, &err));
  CHECK(le == LayoutError::NoFields);

  // A field past the struct end.
  CHECK(!StructLayout::Parse(
      "{\"name\":\"A\",\"version\":\"1\",\"build\":\"b\",\"size\":4,\"fields\":["
      "{\"name\":\"x\",\"type\":\"u32\",\"offset\":2}]}",
      &l, &le, &err));
  CHECK(le == LayoutError::OutOfRange);

  // Overlapping fields.
  CHECK(!StructLayout::Parse(
      "{\"name\":\"A\",\"version\":\"1\",\"build\":\"b\",\"size\":8,\"fields\":["
      "{\"name\":\"x\",\"type\":\"u32\",\"offset\":0},"
      "{\"name\":\"y\",\"type\":\"u16\",\"offset\":2}]}",
      &l, &le, &err));
  CHECK(le == LayoutError::Overlap);

  // Duplicate field names.
  CHECK(!StructLayout::Parse(
      "{\"name\":\"A\",\"version\":\"1\",\"build\":\"b\",\"size\":8,\"fields\":["
      "{\"name\":\"x\",\"type\":\"u8\",\"offset\":0},"
      "{\"name\":\"x\",\"type\":\"u8\",\"offset\":1}]}",
      &l, &le, &err));
  CHECK(le == LayoutError::DuplicateField);

  // Bad type and bad offset.
  CHECK(!StructLayout::Parse(
      "{\"name\":\"A\",\"version\":\"1\",\"build\":\"b\",\"size\":8,\"fields\":["
      "{\"name\":\"x\",\"type\":\"u24\",\"offset\":0}]}",
      &l, &le, &err));
  CHECK(le == LayoutError::BadFieldType);
  CHECK(!StructLayout::Parse(
      "{\"name\":\"A\",\"version\":\"1\",\"build\":\"b\",\"size\":8,\"fields\":["
      "{\"name\":\"x\",\"type\":\"u8\",\"offset\":-1}]}",
      &l, &le, &err));
  CHECK(le == LayoutError::BadOffset);

  // Unknown keys, at both levels.
  CHECK(!StructLayout::Parse(
      "{\"name\":\"A\",\"version\":\"1\",\"build\":\"b\",\"size\":8,\"nope\":1,"
      "\"fields\":[{\"name\":\"x\",\"type\":\"u8\",\"offset\":0}]}",
      &l, &le, &err));
  CHECK(le == LayoutError::BadJson);
  CHECK(!StructLayout::Parse(
      "{\"name\":\"A\",\"version\":\"1\",\"build\":\"b\",\"size\":8,\"fields\":["
      "{\"name\":\"x\",\"type\":\"u8\",\"offset\":0,\"extra\":1}]}",
      &l, &le, &err));
  CHECK(le == LayoutError::BadField);

  // Adjacent, non-overlapping fields are fine.
  CHECK(StructLayout::Parse(
      "{\"name\":\"A\",\"version\":\"1\",\"build\":\"b\",\"size\":8,\"fields\":["
      "{\"name\":\"x\",\"type\":\"u32\",\"offset\":0},"
      "{\"name\":\"y\",\"type\":\"u32\",\"offset\":4}]}",
      &l, &le, &err));
}

//----------------------------------------------------------------------------
// Reader
//----------------------------------------------------------------------------

static void TestReaderScalars() {
  const StructLayout l = Layout(kActorJson);
  std::vector<u8> buf(l.size, 0);

  // id = 0x1122334455667788
  const u64 id = 0x1122334455667788ull;
  std::memcpy(buf.data() + 0, &id, 8);
  const f32 hp = 19.5f;
  std::memcpy(buf.data() + 8, &hp, 4);
  buf[12] = 1;    // alive
  buf[13] = 7;    // flags
  const f32 xyz[3] = {1.5f, -2.0f, 3.25f};
  std::memcpy(buf.data() + 16, xyz, 12);
  const uptr parent = 0xDEADBEEF;
  std::memcpy(buf.data() + 32, &parent, sizeof(uptr));
  const char* nm = "Steve";
  std::memcpy(buf.data() + 48, nm, 6);   // includes the NUL

  StructReader r(&l, buf.data(), buf.size());
  CHECK(r.valid());

  u64 v = 0;
  CHECK(r.ReadU64("id", &v) && v == id);
  i64 iv = 0;
  CHECK(r.ReadI64("id", &iv) && (u64)iv == id);

  f64 d = 0;
  CHECK(r.ReadF64("health", &d) && d > 19.4 && d < 19.6);

  bool b = false;
  CHECK(r.ReadBool("alive", &b) && b);
  CHECK(r.ReadBool("flags", &b) && b);   // 7 != 0
  buf[13] = 0;
  CHECK(r.ReadBool("flags", &b) && !b);

  Vec3 p;
  CHECK(r.ReadVec3("pos", &p));
  CHECK(p.x == 1.5f && p.y == -2.0f && p.z == 3.25f);

  uptr ptr = 0;
  CHECK(r.ReadPointer("parent", &ptr) && ptr == parent);

  std::string s;
  CHECK(r.ReadString("name", &s) && s == "Steve");
}

static void TestReaderErrors() {
  const StructLayout l = Layout(kActorJson);
  std::vector<u8> buf(l.size, 0);
  buf[12] = 1;
  // No terminator anywhere in the string field's bytes.
  std::memset(buf.data() + 48, 'A', l.size - 48);

  StructReader r(&l, buf.data(), buf.size());

  // Type mismatches.
  f64 d = 0;
  CHECK(!r.ReadF64("id", &d));
  CHECK(r.lastError() == ReadError::NotAFloat);
  i64 v = 0;
  CHECK(!r.ReadI64("health", &v));
  CHECK(r.lastError() == ReadError::NotAnInteger);
  uptr ptr = 0;
  CHECK(!r.ReadPointer("health", &ptr));
  CHECK(r.lastError() == ReadError::NotAPointer);
  Vec3 p;
  CHECK(!r.ReadVec3("health", &p));
  CHECK(r.lastError() == ReadError::NotAVec3);

  // Unknown field.
  u64 uv = 0;
  CHECK(!r.ReadU64("ghost", &uv));
  CHECK(r.lastError() == ReadError::NoLayout);

  // A string with no terminator is refused, not silently truncated.
  std::string s;
  CHECK(!r.ReadString("name", &s));
  CHECK(r.lastError() == ReadError::BadString);

  // A truncated buffer: the layout says 64 bytes, the reader was given 20.
  StructReader short_r(&l, buf.data(), 20);
  CHECK(!short_r.ReadPointer("parent", &ptr));   // offset 32, past 20
  CHECK(short_r.lastError() == ReadError::OutOfBounds);
  // ...but a field fully inside the supplied bytes still reads.
  CHECK(short_r.ReadF64("health", &d));

  // A null base or null layout fails closed.
  StructReader null_base(&l, nullptr, 64);
  CHECK(!null_base.valid());
  CHECK(!null_base.ReadU64("id", &uv));
  CHECK(null_base.lastError() == ReadError::NullBase);
  StructReader null_layout(nullptr, buf.data(), buf.size());
  CHECK(!null_layout.valid());
  CHECK(!null_layout.ReadU64("id", &uv));

  // FollowPointer is one hop: it reads the address but does not dereference it.
  const uptr parent = 0x1234;
  std::memcpy(buf.data() + 32, &parent, sizeof(uptr));
  uptr hop = 0;
  CHECK(FollowPointer(r, "parent", &hop) && hop == parent);
  CHECK(!FollowPointer(r, "health", &hop));
}

//----------------------------------------------------------------------------
// Registry + build gate
//----------------------------------------------------------------------------

static void TestRegistryBuildGate() {
  LayoutRegistry reg;
  CHECK(reg.Add(Layout(kActorJson)));
  // A second build of the same struct is fine; the same build is a duplicate.
  std::string other = kActorJson;
  const size_t pos = other.find("abc123");
  other.replace(pos, 6, "def456");
  CHECK(reg.Add(Layout(other)));
  std::string err;
  CHECK(!reg.Add(Layout(kActorJson), &err));
  CHECK(!err.empty());
  CHECK(reg.Count() == 2);
  CHECK(reg.Has("Actor"));

  // No active build: every lookup fails closed.
  StructLayout found;
  CHECK(!reg.Find("Actor", &found));
  CHECK(reg.activeBuild().empty());

  reg.SetActiveBuild("abc123");
  CHECK(reg.activeBuild() == "abc123");
  CHECK(reg.Find("Actor", &found));
  CHECK(found.build == "abc123");

  // A build with no matching layout fails closed.
  reg.SetActiveBuild("nope");
  CHECK(!reg.Find("Actor", &found));

  // Explicit-build lookup ignores the active build (for tooling).
  CHECK(reg.FindForBuild("Actor", "def456", &found));
  CHECK(found.build == "def456");
  CHECK(!reg.FindForBuild("Actor", "zzz", &found));

  // An empty fingerprint disables everything again.
  reg.SetActiveBuild("");
  CHECK(!reg.Find("Actor", &found));

  // AddJson round-trips and reports the parse error.
  LayoutRegistry reg2;
  CHECK(reg2.AddJson(kActorJson) == LayoutError::Ok);
  CHECK(reg2.AddJson("{}") == LayoutError::MissingName);
  CHECK(reg2.Names().size() == 1 && reg2.Names()[0] == "Actor");

  reg.Clear();
  CHECK(reg.Count() == 0);
}

int main() {
  TestFieldTypes();
  TestLayoutParse();
  TestReaderScalars();
  TestReaderErrors();
  TestRegistryBuildGate();
  if (g_fail == 0) printf("test_layout: ALL OK\n");
  return g_fail;
}
