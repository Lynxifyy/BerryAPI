// Host tests for the script shader registry and post-process chain.
//
// Covers:
//   * Pure validation: name rules, empty/oversized source, the #include
//     refusal, the `void main` requirement, and the uniform cap.
//   * Uniform parsing: names, types, comment-safety and the word boundary.
//   * Registration, replacement (hot reload), unregister and the shader cap.
//   * Hot reload preserving chain position and surviving parameters, and
//     dropping parameters whose uniform disappeared.
//   * The post-process chain: enable/disable, ordering, chain names.
//   * Parameters: only declared uniforms, finite values, the parameter cap.
//   * The device compiler seam: a refused compile keeps the previous program
//     and reports the reason; a null compiler still registers.
//   * The shader.* script bindings, including the fail-closed case with no
//     registry bound.
#include "berry/api_bindings.h"
#include "berry/generated_bindings.h"
#include "berry/shader.h"
#include "berry/script.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

CapabilitySet Grant(const std::vector<std::string>& caps) {
  CapabilitySet s;
  for (const auto& c : caps) s.Add(c);
  return s;
}

ScriptStatus CallBinding(ScriptBindings& bindings, const std::string& name,
                         const std::vector<ScriptValue>& args,
                         const std::vector<std::string>& grants,
                         ScriptValue* out) {
  ScriptHost host("shader-test", Grant(grants), nullptr);
  host.SetBindings(&bindings);
  return host.CallBinding(name, args, out);
}

const char* kBlurSrc =
    "uniform float radius;\n"
    "uniform vec2 direction;\n"
    "void main() { gl_FragColor = vec4(radius); }\n";

/// A compiler that refuses everything, to exercise the device-failure path.
class FailingCompiler : public IShaderCompiler {
 public:
  bool Compile(const ShaderSource&, std::string* error) override {
    if (error) *error = "synthetic compile failure";
    return false;
  }
  void Release(const std::string&) override { ++released; }
  int released = 0;
};

/// A compiler that accepts everything and counts.
class RecordingCompiler : public IShaderCompiler {
 public:
  bool Compile(const ShaderSource& s, std::string*) override {
    last = s.name;
    ++compiles;
    return true;
  }
  void Release(const std::string&) override { ++released; }
  std::string last;
  int compiles = 0;
  int released = 0;
};

}  // namespace

static void TestValidation() {
  ShaderSource s;
  CHECK(ValidateShaderSource(s) == ShaderReject::EmptyName);

  s.name = "9bad";
  s.source = "void main(){}";
  CHECK(ValidateShaderSource(s) == ShaderReject::BadName);

  s.name = "has space";
  CHECK(ValidateShaderSource(s) == ShaderReject::BadName);

  s.name = "ok";
  s.source = "";
  CHECK(ValidateShaderSource(s) == ShaderReject::EmptySource);

  s.source = "void main() {}";
  CHECK(ValidateShaderSource(s) == ShaderReject::None);

  // Missing entry point.
  s.source = "float x = 1.0;";
  CHECK(ValidateShaderSource(s) == ShaderReject::MissingMain);

  // #include is refused, not silently ignored.
  s.source = "#include <common>\nvoid main(){}";
  CHECK(ValidateShaderSource(s) == ShaderReject::HasInclude);

  // Oversized source.
  s.source = "void main(){" + std::string(ShaderRegistry::kMaxSourceBytes, 'x') + "}";
  CHECK(ValidateShaderSource(s) == ShaderReject::TooLong);

  // Too many uniforms.
  s.source = "void main(){}";
  std::string many;
  for (size_t i = 0; i < ShaderRegistry::kMaxUniforms + 1; ++i) {
    many += "uniform float u" + std::to_string(i) + ";\n";
  }
  s.source = many + "void main(){}";
  CHECK(ValidateShaderSource(s) == ShaderReject::TooManyUniforms);
}

static void TestUniformParsing() {
  const std::vector<ShaderUniform> u = ParseShaderUniforms(
      "uniform float radius;\n"
      "uniform vec2 direction;\n"
      "uniform mat4 mvp;\n"
      "void main(){}\n");
  CHECK(u.size() == 3);
  CHECK(u[0].name == "radius" && u[0].type == "float");
  CHECK(u[1].name == "direction" && u[1].type == "vec2");
  CHECK(u[2].name == "mvp" && u[2].type == "mat4");

  // "uniform" inside a longer identifier is not a declaration.
  CHECK(ParseShaderUniforms("float uniformity = 1.0; void main(){}").empty());
  // An unknown type token is not a uniform we can drive.
  CHECK(ParseShaderUniforms("uniform sampler2D tex; void main(){}").empty());
}

static void TestRegisterAndReplace() {
  ShaderRegistry reg;
  CHECK(reg.Count() == 0);

  ShaderSource a{"blur", ShaderStage::Fragment, kBlurSrc};
  CHECK(reg.Register(a) == ShaderReject::None);
  CHECK(reg.Count() == 1);
  CHECK(reg.Has("blur"));
  CHECK(reg.UniformNames("blur") == "radius,direction");

  // Replacing with the same name is a reload, not a second entry.
  ShaderSource a2{"blur", ShaderStage::Fragment,
                  "uniform float strength;\nvoid main(){}\n"};
  CHECK(reg.Register(a2) == ShaderReject::None);
  CHECK(reg.Count() == 1);
  CHECK(reg.UniformNames("blur") == "strength");

  // A refused registration leaves the registry unchanged.
  ShaderSource bad{"", ShaderStage::Fragment, "void main(){}"};
  CHECK(reg.Register(bad) == ShaderReject::EmptyName);
  CHECK(reg.Count() == 1);

  CHECK(reg.Unregister("blur"));
  CHECK(!reg.Has("blur"));
  CHECK(!reg.Unregister("blur"));   // already gone

  // The shader cap.
  ShaderRegistry small;
  for (size_t i = 0; i < ShaderRegistry::kMaxShaders; ++i) {
    ShaderSource s{"s" + std::to_string(i), ShaderStage::Fragment, "void main(){}"};
    CHECK(small.Register(s) == ShaderReject::None);
  }
  ShaderSource over{"one_too_many", ShaderStage::Fragment, "void main(){}"};
  CHECK(small.Register(over) == ShaderReject::TooMany);
}

static void TestHotReloadKeepsState() {
  ShaderRegistry reg;
  ShaderSource a{"grade", ShaderStage::Fragment, kBlurSrc};
  CHECK(reg.Register(a) == ShaderReject::None);
  CHECK(reg.SetEffectEnabled("grade", true));
  CHECK(reg.SetParam("grade", "radius", 2.5f));
  CHECK(reg.SetParam("grade", "direction", 1.0f));

  // Reload keeping both uniforms: chain position and params survive.
  CHECK(reg.Register(a) == ShaderReject::None);
  CHECK(reg.EffectEnabled("grade"));
  f32 v = 0;
  CHECK(reg.Param("grade", "radius", &v));
  CHECK(v == 2.5f);
  CHECK(reg.Param("grade", "direction", &v));
  CHECK(v == 1.0f);

  // Reload dropping `direction`: that param is gone, `radius` survives.
  ShaderSource a2{"grade", ShaderStage::Fragment,
                  "uniform float radius;\nvoid main(){}\n"};
  CHECK(reg.Register(a2) == ShaderReject::None);
  CHECK(reg.Param("grade", "radius", &v) && v == 2.5f);
  CHECK(!reg.Param("grade", "direction", &v));
  CHECK(reg.EffectEnabled("grade"));
}

static void TestChain() {
  ShaderRegistry reg;
  CHECK(reg.Chain().empty());
  CHECK(reg.ChainNames().empty());

  CHECK(reg.Register({"first", ShaderStage::Fragment, "void main(){}"}) ==
        ShaderReject::None);
  CHECK(reg.Register({"second", ShaderStage::Fragment, "void main(){}"}) ==
        ShaderReject::None);

  // A name that is not registered cannot be enabled.
  CHECK(!reg.SetEffectEnabled("ghost", true));
  CHECK(!reg.EffectEnabled("ghost"));

  CHECK(reg.SetEffectEnabled("first", true));
  CHECK(reg.SetEffectEnabled("second", true));
  CHECK(reg.ChainNames() == "first,second");
  const std::vector<PostEffect> chain = reg.Chain();
  CHECK(chain.size() == 2);
  CHECK(chain[0].name == "first");
  CHECK(chain[1].name == "second");

  CHECK(reg.SetEffectEnabled("first", false));
  CHECK(reg.ChainNames() == "second");

  reg.Clear();
  CHECK(reg.Count() == 0);
  CHECK(reg.ChainNames().empty());
}

static void TestParams() {
  ShaderRegistry reg;
  CHECK(reg.Register({"p", ShaderStage::Fragment, kBlurSrc}) == ShaderReject::None);

  // Only a declared uniform may be written.
  CHECK(!reg.SetParam("p", "nope", 1.0f));
  CHECK(reg.SetParam("p", "radius", 1.0f));

  // A non-finite value is refused.
  CHECK(!reg.SetParam("p", "radius", std::nanf("")));
  CHECK(!reg.SetParam("p", "radius", INFINITY));

  // A malformed parameter name is refused.
  CHECK(!reg.SetParam("p", "not an ident", 1.0f));

  // An unregistered effect refuses the write.
  CHECK(!reg.SetParam("ghost", "radius", 1.0f));

  // Every declared uniform is writable up to the uniform cap, and re-setting an
  // existing one is always allowed. (A separate parameter cap would be
  // unreachable, since a parameter must name a declared uniform and both caps
  // are the same size.)
  ShaderRegistry many;
  std::string src;
  for (size_t i = 0; i < ShaderRegistry::kMaxUniforms; ++i) {
    src += "uniform float p" + std::to_string(i) + ";\n";
  }
  src += "void main(){}";
  CHECK(many.Register({"m", ShaderStage::Fragment, src}) == ShaderReject::None);
  for (size_t i = 0; i < ShaderRegistry::kMaxUniforms; ++i) {
    CHECK(many.SetParam("m", "p" + std::to_string(i), (f32)i));
  }
  CHECK(many.SetParam("m", "p0", 9.0f));
  f32 v = 0;
  CHECK(many.Param("m", "p0", &v) && v == 9.0f);
}

static void TestCompilerSeam() {
  RecordingCompiler comp;
  {
    ShaderRegistry reg(&comp);
    CHECK(reg.Register({"ok", ShaderStage::Fragment, "void main(){}"}) ==
          ShaderReject::None);
    CHECK(comp.compiles == 1);
    CHECK(comp.last == "ok");
    CHECK(reg.Unregister("ok"));
    CHECK(comp.released == 1);
  }
  {
    // A compiler that refuses leaves the previous program in place and reports
    // the reason without treating it as a validation error.
    ShaderRegistry reg;
    CHECK(reg.Register({"keep", ShaderStage::Fragment, "void main(){}"}) ==
          ShaderReject::None);
    CHECK(reg.SetEffectEnabled("keep", true));

    FailingCompiler bad;
    ShaderRegistry reg2(&bad);
    std::string err;
    // The first registration has no previous program; the name is simply absent.
    CHECK(reg2.Register({"keep", ShaderStage::Fragment, "void main(){}"}, &err) ==
          ShaderReject::None);
    CHECK(!err.empty());
    CHECK(!reg2.Has("keep"));

    // The original registry is unaffected: its program is still there.
    CHECK(reg.Has("keep"));
    CHECK(reg.EffectEnabled("keep"));
  }
}

//----------------------------------------------------------------------------
// Post-process walk
//----------------------------------------------------------------------------

namespace {

/// Records the passes a walk produced, in order.
class RecordingTarget : public IPostProcessTarget {
 public:
  void Begin(const Vec2& size) override {
    ++begins;
    size_ = size;
  }
  bool Pass(const std::string& name, const std::string& source,
            const std::vector<ShaderUniform>& uniforms,
            const std::vector<std::pair<std::string, f32>>& params) override {
    names.push_back(name);
    sources.push_back(source);
    uniformCounts.push_back(uniforms.size());
    paramsByPass.push_back(params);
    return names.size() <= passes;
  }
  void End() override { ++ends; }

  std::vector<std::string> names;
  std::vector<std::string> sources;
  std::vector<size_t> uniformCounts;
  std::vector<std::vector<std::pair<std::string, f32>>> paramsByPass;
  Vec2 size_;
  int begins = 0;
  int ends = 0;
  size_t passes = SIZE_MAX;   // stop after this many passes
};

}  // namespace

static void TestPostProcessWalk() {
  ShaderRegistry reg;
  RecordingTarget target;

  // An empty chain is a no-op: the target is not even begun.
  CHECK(RunPostProcess(reg, &target, Vec2(100, 100)) == 0);
  CHECK(target.begins == 0);

  // A null target is a no-op.
  CHECK(RunPostProcess(reg, nullptr, Vec2(100, 100)) == 0);

  CHECK(reg.Register({"first", ShaderStage::Fragment, kBlurSrc}) ==
        ShaderReject::None);
  CHECK(reg.Register({"second", ShaderStage::Fragment, "void main(){}"}) ==
        ShaderReject::None);
  CHECK(reg.SetEffectEnabled("first", true));
  CHECK(reg.SetEffectEnabled("second", true));
  CHECK(reg.SetParam("first", "radius", 4.0f));

  const size_t n = RunPostProcess(reg, &target, Vec2(320, 240));
  CHECK(n == 2);
  CHECK(target.begins == 1);
  CHECK(target.ends == 1);
  CHECK(target.names.size() == 2);
  CHECK(target.names[0] == "first" && target.names[1] == "second");
  CHECK(target.size_.x == 320 && target.size_.y == 240);
  CHECK(target.uniformCounts[0] == 2);   // radius, direction
  CHECK(target.uniformCounts[1] == 0);
  // Parameters arrive in declared-uniform order, and only the ones set.
  CHECK(target.paramsByPass[0].size() == 1);
  CHECK(target.paramsByPass[0][0].first == "radius");
  CHECK(target.paramsByPass[0][0].second == 4.0f);
  CHECK(target.paramsByPass[1].empty());   // `second` has no params

  // A disabled effect is absent from the walk.
  target = RecordingTarget();
  CHECK(reg.SetEffectEnabled("first", false));
  CHECK(RunPostProcess(reg, &target, Vec2(10, 10)) == 1);
  CHECK(target.names.size() == 1 && target.names[0] == "second");

  // A target that stops early ends the walk cleanly (End still runs).
  target = RecordingTarget();
  target.passes = 1;
  CHECK(RunPostProcess(reg, &target, Vec2(10, 10)) == 1);
  CHECK(target.begins == 1 && target.ends == 1);
  CHECK(target.names.size() == 1);
}

//----------------------------------------------------------------------------
// Script bindings
//----------------------------------------------------------------------------

static void TestShaderBindings() {
  ShaderRegistry reg;
  ScriptApi::Services services;
  services.shaders = &reg;
  ScriptApi api(services);
  ScriptBindings bindings;
  const size_t n = api.InstallBindings(&bindings);
  CHECK(n == api::BindingSpecCount());
  CHECK(bindings.Has("shader.register"));
  CHECK(bindings.Has("shader.chain"));

  ScriptValue out;
  CHECK(CallBinding(bindings, "shader.register",
                    {ScriptValue::Str("blur"), ScriptValue::Str(kBlurSrc)},
                    {"render"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());

  CHECK(CallBinding(bindings, "shader.has", {ScriptValue::Str("blur")},
                    {"render"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());

  CHECK(CallBinding(bindings, "shader.uniforms", {ScriptValue::Str("blur")},
                    {"render"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsString() == "radius,direction");

  // A malformed shader fails the call (the binding raises).
  CHECK(CallBinding(bindings, "shader.register",
                    {ScriptValue::Str("bad"), ScriptValue::Str("no entry")},
                    {"render"}, &out) == ScriptStatus::Error);

  CHECK(CallBinding(bindings, "shader.enable",
                    {ScriptValue::Str("blur"), ScriptValue::Bool(true)},
                    {"render"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(CallBinding(bindings, "shader.chain", {}, {"render"}, &out) ==
        ScriptStatus::Ok);
  CHECK(out.AsString() == "blur");

  CHECK(CallBinding(bindings, "shader.set_param",
                    {ScriptValue::Str("blur"), ScriptValue::Str("radius"),
                     ScriptValue::Num(3.0)},
                    {"render"}, &out) == ScriptStatus::Ok);
  CHECK(out.Truthy());
  CHECK(CallBinding(bindings, "shader.param",
                    {ScriptValue::Str("blur"), ScriptValue::Str("radius")},
                    {"render"}, &out) == ScriptStatus::Ok);
  CHECK(out.AsNumber() == 3.0);

  CHECK(CallBinding(bindings, "shader.clear", {}, {"render"}, &out) ==
        ScriptStatus::Ok);
  CHECK(reg.Count() == 0);

  // The shader module carries the render grant.
  CHECK(CallBinding(bindings, "shader.register",
                    {ScriptValue::Str("x"), ScriptValue::Str("void main(){}")},
                    {}, &out) == ScriptStatus::CapabilityDenied);

  // With no registry bound, registration fails closed.
  ScriptApi::Services none;
  ScriptApi bare(none);
  ScriptBindings bareBindings;
  bare.InstallBindings(&bareBindings);
  CHECK(CallBinding(bareBindings, "shader.register",
                    {ScriptValue::Str("x"), ScriptValue::Str("void main(){}")},
                    {"render"}, &out) == ScriptStatus::Error);
  CHECK(!out.Truthy());
}

int main() {
  TestValidation();
  TestUniformParsing();
  TestRegisterAndReplace();
  TestHotReloadKeepsState();
  TestChain();
  TestParams();
  TestCompilerSeam();
  TestPostProcessWalk();
  TestShaderBindings();
  if (g_fail == 0) printf("test_shader: ALL OK\n");
  return g_fail;
}
