// Script shader registry + post-process chain. See include/berry/shader.h for
// the model: validation is pure, GL work is behind IShaderCompiler.
#include "berry/shader.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace Berry {

namespace {

constexpr size_t kMaxNameLen = ShaderRegistry::kMaxNameLen;
constexpr size_t kMaxUniforms = ShaderRegistry::kMaxUniforms;
constexpr size_t kMaxSourceBytes = ShaderRegistry::kMaxSourceBytes;
constexpr size_t kMaxShaders = ShaderRegistry::kMaxShaders;

bool IsIdentStart(char c) {
  return std::isalpha((unsigned char)c) || c == '_';
}
bool IsIdentChar(char c) {
  return std::isalnum((unsigned char)c) || c == '_';
}

/// A name that can appear in a URL-ish context and be logged unambiguously.
bool IsPlainIdentifier(const std::string& s) {
  if (s.empty() || s.size() > kMaxNameLen) return false;
  if (!IsIdentStart(s[0])) return false;
  for (char c : s) {
    if (!IsIdentChar(c)) return false;
  }
  return true;
}

/// True when `token` appears as a whole word (not part of a longer identifier).
bool ContainsWord(const std::string& hay, const std::string& word) {
  size_t pos = hay.find(word);
  while (pos != std::string::npos) {
    const bool leftOk = pos == 0 || !IsIdentChar(hay[pos - 1]);
    const size_t end = pos + word.size();
    const bool rightOk = end >= hay.size() || !IsIdentChar(hay[end]);
    if (leftOk && rightOk) return true;
    pos = hay.find(word, pos + 1);
  }
  return false;
}

/// The known GLSL types a uniform may be declared with. A declaration using an
/// unknown token is not a uniform we can drive, so it is skipped rather than
/// treated as a malformed declaration.
bool IsKnownUniformType(const std::string& t) {
  static const char* kTypes[] = {"float", "vec2", "vec3", "vec4", "int", "bool",
                                 "mat2", "mat3", "mat4"};
  for (const char* k : kTypes) {
    if (t == k) return true;
  }
  return false;
}

}  // namespace

const char* ShaderRejectName(ShaderReject r) {
  switch (r) {
    case ShaderReject::None: return "ok";
    case ShaderReject::EmptyName: return "empty name";
    case ShaderReject::BadName: return "name must be an identifier";
    case ShaderReject::EmptySource: return "empty source";
    case ShaderReject::TooLong: return "source too long";
    case ShaderReject::TooMany: return "too many shaders";
    case ShaderReject::MissingMain: return "missing void main";
    case ShaderReject::HasInclude: return "#include is not supported";
    case ShaderReject::TooManyUniforms: return "too many uniforms";
    case ShaderReject::BadUniform: return "bad uniform name";
  }
  return "unknown";
}

//----------------------------------------------------------------------------
// Pure validation + parsing
//----------------------------------------------------------------------------

ShaderReject ValidateShaderSource(const ShaderSource& s) {
  if (s.name.empty()) return ShaderReject::EmptyName;
  if (!IsPlainIdentifier(s.name)) return ShaderReject::BadName;
  if (s.source.empty()) return ShaderReject::EmptySource;
  if (s.source.size() > kMaxSourceBytes) return ShaderReject::TooLong;
  // A preprocessor include would pull in text we never validated; refusing it
  // is safer than silently ignoring it.
  if (ContainsWord(s.source, "include")) return ShaderReject::HasInclude;
  if (!ContainsWord(s.source, "main")) return ShaderReject::MissingMain;

  const std::vector<ShaderUniform> u = ParseShaderUniforms(s.source);
  if (u.size() > kMaxUniforms) return ShaderReject::TooManyUniforms;
  for (const auto& un : u) {
    if (!IsPlainIdentifier(un.name)) return ShaderReject::BadUniform;
  }
  return ShaderReject::None;
}

std::vector<ShaderUniform> ParseShaderUniforms(const std::string& source) {
  std::vector<ShaderUniform> out;
  const std::string needle = "uniform";
  size_t pos = 0;
  while ((pos = source.find(needle, pos)) != std::string::npos) {
    // Must be a whole word, and the next non-space char must start a type.
    const bool leftOk = pos == 0 || !IsIdentChar(source[pos - 1]);
    if (!leftOk) {
      pos += needle.size();
      continue;
    }
    size_t i = pos + needle.size();
    auto skip = [&](size_t* p) {
      while (*p < source.size() && std::isspace((unsigned char)source[*p])) ++(*p);
    };
    skip(&i);
    // Read the type token.
    const size_t typeStart = i;
    while (i < source.size() && IsIdentChar(source[i])) ++i;
    const std::string type = source.substr(typeStart, i - typeStart);
    if (!IsKnownUniformType(type)) {
      pos += needle.size();
      continue;
    }
    skip(&i);
    // Read the name token.
    const size_t nameStart = i;
    while (i < source.size() && IsIdentChar(source[i])) ++i;
    const std::string name = source.substr(nameStart, i - nameStart);
    if (!name.empty()) out.push_back(ShaderUniform{name, type});
    pos = i > pos ? i : pos + needle.size();
  }
  return out;
}

//----------------------------------------------------------------------------
// ShaderRegistry
//----------------------------------------------------------------------------

ShaderRegistry::ShaderRegistry(IShaderCompiler* compiler) : compiler_(compiler) {}

ShaderRegistry::Entry* ShaderRegistry::FindLocked(const std::string& name) {
  for (auto& kv : entries_) {
    if (kv.first == name) return &kv.second;
  }
  return nullptr;
}

ShaderReject ShaderRegistry::Register(const ShaderSource& s, std::string* error) {
  if (error) error->clear();

  const ShaderReject reason = ValidateShaderSource(s);
  if (reason != ShaderReject::None) return reason;

  const std::vector<ShaderUniform> uniforms = ParseShaderUniforms(s.source);

  std::lock_guard<std::mutex> lk(mutex_);
  Entry* existing = FindLocked(s.name);
  if (!existing && entries_.size() >= kMaxShaders) return ShaderReject::TooMany;

  // Compile before committing. A device that refuses the shader leaves the
  // previous program (and its chain position and params) untouched.
  if (compiler_) {
    std::string cerr;
    if (!compiler_->Compile(s, &cerr)) {
      if (error) *error = cerr;
      return ShaderReject::None;   // not a validation error; old state kept
    }
  }

  if (existing) {
    // Hot reload: keep enabled state, chain position and every parameter whose
    // uniform survived the edit.
    std::unordered_map<std::string, f32> kept;
    for (const auto& un : uniforms) {
      auto it = existing->params.find(un.name);
      if (it != existing->params.end()) kept.emplace(un.name, it->second);
    }
    existing->src = s;
    existing->uniforms = uniforms;
    existing->params = std::move(kept);
    return ShaderReject::None;
  }

  Entry e;
  e.src = s;
  e.uniforms = uniforms;
  entries_.emplace_back(s.name, std::move(e));
  return ShaderReject::None;
}

bool ShaderRegistry::Unregister(const std::string& name) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->first == name) {
      if (compiler_) compiler_->Release(name);
      entries_.erase(it);
      return true;
    }
  }
  return false;
}

bool ShaderRegistry::Has(const std::string& name) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& kv : entries_) {
    if (kv.first == name) return true;
  }
  return false;
}

std::vector<std::string> ShaderRegistry::Names() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<std::string> out;
  out.reserve(entries_.size());
  for (const auto& kv : entries_) out.push_back(kv.first);
  return out;
}

size_t ShaderRegistry::Count() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return entries_.size();
}

std::vector<ShaderUniform> ShaderRegistry::Uniforms(const std::string& name) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& kv : entries_) {
    if (kv.first == name) return kv.second.uniforms;
  }
  return {};
}

std::string ShaderRegistry::UniformNames(const std::string& name) const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::string out;
  for (const auto& kv : entries_) {
    if (kv.first != name) continue;
    for (const auto& u : kv.second.uniforms) {
      if (!out.empty()) out += ",";
      out += u.name;
    }
  }
  return out;
}

std::string ShaderRegistry::Source(const std::string& name) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& kv : entries_) {
    if (kv.first == name) return kv.second.src.source;
  }
  return {};
}

bool ShaderRegistry::SetEffectEnabled(const std::string& name, bool on) {
  std::lock_guard<std::mutex> lk(mutex_);
  Entry* e = FindLocked(name);
  if (!e) return false;
  e->enabled = on;
  return true;
}

bool ShaderRegistry::EffectEnabled(const std::string& name) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& kv : entries_) {
    if (kv.first == name) return kv.second.enabled;
  }
  return false;
}

bool ShaderRegistry::SetParam(const std::string& name, const std::string& param,
                              f32 value) {
  if (!std::isfinite(value)) return false;
  if (!IsPlainIdentifier(param)) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  Entry* e = FindLocked(name);
  if (!e) return false;
  // The parameter must be a uniform the source actually declares, or the write
  // would silently do nothing at draw time.
  bool declared = false;
  for (const auto& u : e->uniforms) {
    if (u.name == param) { declared = true; break; }
  }
  if (!declared) return false;
  e->params[param] = value;
  return true;
}

bool ShaderRegistry::Param(const std::string& name, const std::string& param,
                           f32* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& kv : entries_) {
    if (kv.first != name) continue;
    auto it = kv.second.params.find(param);
    if (it == kv.second.params.end()) return false;
    if (out) *out = it->second;
    return true;
  }
  return false;
}

std::vector<PostEffect> ShaderRegistry::Chain() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<PostEffect> out;
  for (const auto& kv : entries_) {
    if (!kv.second.enabled) continue;
    PostEffect e;
    e.name = kv.first;
    e.enabled = true;
    e.params = kv.second.params;
    out.push_back(std::move(e));
  }
  return out;
}

std::string ShaderRegistry::ChainNames() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::string out;
  for (const auto& kv : entries_) {
    if (!kv.second.enabled) continue;
    if (!out.empty()) out += ",";
    out += kv.first;
  }
  return out;
}

void ShaderRegistry::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (compiler_) {
    for (const auto& kv : entries_) compiler_->Release(kv.first);
  }
  entries_.clear();
}

//----------------------------------------------------------------------------
// Post-process walk
//----------------------------------------------------------------------------

size_t RunPostProcess(ShaderRegistry& registry, IPostProcessTarget* target,
                      const Vec2& screenSize) {
  if (!target) return 0;

  // Snapshot the enabled chain under the registry lock, then draw without it:
  // the target calls back into GL and must never run while the registry is
  // locked (a script reloading a shader mid-frame would otherwise deadlock).
  struct Pass {
    std::string name;
    std::string source;
    std::vector<ShaderUniform> uniforms;
    std::vector<std::pair<std::string, f32>> params;
  };
  std::vector<Pass> passes;
  {
    const std::vector<PostEffect> chain = registry.Chain();
    passes.reserve(chain.size());
    for (const PostEffect& e : chain) {
      Pass p;
      p.name = e.name;
      p.source = registry.Source(e.name);
      p.uniforms = registry.Uniforms(e.name);
      // Parameters in declared-uniform order, so the backend can bind them
      // positionally without a name lookup per frame.
      for (const ShaderUniform& u : p.uniforms) {
        auto it = e.params.find(u.name);
        if (it != e.params.end()) p.params.emplace_back(u.name, it->second);
      }
      passes.push_back(std::move(p));
    }
  }
  if (passes.empty()) return 0;

  target->Begin(screenSize);
  size_t done = 0;
  for (const Pass& p : passes) {
    if (!target->Pass(p.name, p.source, p.uniforms, p.params)) break;
    ++done;
  }
  target->End();
  return done;
}

}  // namespace Berry
