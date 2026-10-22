#pragma once
// Script-registered GLES fragment shaders and the post-processing chain.
//
// A script may register a fragment shader (blur, colour grading, a HUD
// material) and put it on an ordered post-process chain. Two things make this
// safe to expose:
//
//   * Registration is validated by a PURE function before any GL call. The
//     checks that matter (bounded source, bounded count, a real entry point, no
//     preprocessor includes, a bounded and well-formed uniform set) do not need
//     a GL context, so they are testable on the host and cannot be skipped by a
//     device that fails to compile.
//   * The GL work happens behind `IShaderCompiler`. On the host that is null
//     and compilation is a no-op; on device it is the GLES adapter. A registry
//     therefore never needs a context to be constructed or tested.
//
// Hot reload is re-registration: `Register` with an existing name replaces the
// program atomically, keeping the chain position and the parameter values whose
// uniforms still exist. A reload that fails validation leaves the old program
// in place, so a broken edit cannot blank the screen.
//
// This is a client-side service, deliberately not a new strict Berry:: module:
// it draws through Berry::Render and holds no engine pointers of its own.

#include "berry/export.h"
#include "berry/math.h"
#include "berry/types.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Berry {

enum class ShaderStage : u32 {
  Fragment = 0,
  Vertex,
  Count,
};

/// Why a shader was refused. Every value except `None` is a rejection.
enum class ShaderReject : u32 {
  None = 0,
  EmptyName,
  BadName,        // not a plain identifier
  EmptySource,
  TooLong,        // source exceeds kMaxSourceBytes
  TooMany,        // registry already holds kMaxShaders
  MissingMain,    // no `void main`
  HasInclude,     // `#include` is not supported and must not be silently ignored
  TooManyUniforms,
  BadUniform,     // a declared uniform name is not an identifier
};

const char* ShaderRejectName(ShaderReject r);

/// A shader as a script declares it.
struct BERRY_API ShaderSource {
  std::string name;
  ShaderStage stage = ShaderStage::Fragment;
  std::string source;
};

/// A uniform the source declares: the parsed name and the type token.
struct BERRY_API ShaderUniform {
  std::string name;
  std::string type;
};

/// Validates a shader without a GL context. Returns the first reason it is
/// unusable, or `None`. Pure: no registry, no GL, no state.
ShaderReject ValidateShaderSource(const ShaderSource& s);

/// Extracts the uniforms a source declares, in order. Only meaningful after
/// `ValidateShaderSource` returns `None`.
std::vector<ShaderUniform> ParseShaderUniforms(const std::string& source);

/// The device-side compiler. The host has none (null), so registration still
/// works and the chain can be inspected; only the GL upload is skipped.
class BERRY_API IShaderCompiler {
 public:
  virtual ~IShaderCompiler() = default;

  /// Compiles and links a shader. Returns false and sets `*error` on failure.
  /// A registry that fails to compile keeps the previous program, if any.
  virtual bool Compile(const ShaderSource& s, std::string* error) = 0;

  /// Releases the compiled program for `name`. A name that is not compiled is
  /// a no-op.
  virtual void Release(const std::string& name) = 0;
};

/// One entry on the post-process chain.
struct BERRY_API PostEffect {
  std::string name;                        // the registered shader's name
  bool enabled = false;
  std::unordered_map<std::string, f32> params;
};

/// The registry of script shaders plus the ordered post-process chain.
///
/// Thread safety: registration and chain edits come from the script worker,
/// consumption from the render thread, so every accessor locks.
class BERRY_API ShaderRegistry {
 public:
  /// Comfortably more effects than a sane script uses, small enough that a
  /// runaway one cannot exhaust the device's shader slots.
  static constexpr size_t kMaxShaders = 64;
  /// A fragment shader larger than this is a mistake or an attack.
  static constexpr size_t kMaxSourceBytes = 64 * 1024;
  static constexpr size_t kMaxUniforms = 32;
  static constexpr size_t kMaxNameLen = 64;

  explicit ShaderRegistry(IShaderCompiler* compiler = nullptr);

  /// Registers a shader, or replaces one with the same name (hot reload). On
  /// replacement the chain position and any parameter whose uniform still
  /// exists are preserved. Returns `None` on success, otherwise the reason.
  ///
  /// A device compile failure is reported as `None` with `*error` set and the
  /// previous program left in place: the script asked for a shader and got one
  /// that the device refused, which is not a validation error.
  ShaderReject Register(const ShaderSource& s, std::string* error = nullptr);

  bool Unregister(const std::string& name);
  bool Has(const std::string& name) const;
  std::vector<std::string> Names() const;      // registration order
  size_t Count() const;
  std::vector<ShaderUniform> Uniforms(const std::string& name) const;
  std::string UniformNames(const std::string& name) const;   // "a,b"
  /// The registered source of a shader, or an empty string when it is absent.
  std::string Source(const std::string& name) const;

  //--- post-process chain ---------------------------------------------------
  /// Enables or disables an effect. Refused (false) for a name that is not
  /// registered: the chain only ever names real shaders.
  bool SetEffectEnabled(const std::string& name, bool on);
  bool EffectEnabled(const std::string& name) const;

  /// Sets a float parameter. Refused (false) for an effect that is not
  /// registered, a parameter that is not a declared uniform, or a non-finite
  /// value.
  bool SetParam(const std::string& name, const std::string& param, f32 value);
  bool Param(const std::string& name, const std::string& param, f32* out) const;

  /// The enabled effects, in registration order. This is what the renderer
  /// walks; a disabled effect is simply absent.
  std::vector<PostEffect> Chain() const;
  /// Comma-separated names of the enabled effects.
  std::string ChainNames() const;

  void Clear();

 private:
  struct Entry {
    ShaderSource src;
    std::vector<ShaderUniform> uniforms;
    bool enabled = false;
    std::unordered_map<std::string, f32> params;
  };

  Entry* FindLocked(const std::string& name);

  mutable std::mutex mutex_;
  IShaderCompiler* compiler_ = nullptr;
  std::vector<std::pair<std::string, Entry>> entries_;   // registration order
};

/// Receives the post-process chain for drawing, one fullscreen pass per effect.
/// The device implementation binds the framebuffer and swaps programs; the host
/// has none. This mirrors `RenderSeam`: the registry owns no GL state, it only
/// hands the backend what to draw.
class BERRY_API IPostProcessTarget {
 public:
  virtual ~IPostProcessTarget() = default;

  /// Called once before the first pass. `screenSize` is in physical pixels.
  virtual void Begin(const Vec2& screenSize) = 0;

  /// One effect. `params` are the uniform values the script set, in the order
  /// the shader declared them. Returns false to stop the chain early (e.g. the
  /// backend lost its context).
  virtual bool Pass(const std::string& name, const std::string& source,
                    const std::vector<ShaderUniform>& uniforms,
                    const std::vector<std::pair<std::string, f32>>& params) = 0;

  /// Called once after the last pass (or after an early stop).
  virtual void End() = 0;
};

/// Walks the enabled chain and drives `target`. A null target or an empty chain
/// is a no-op, so calling this every frame is free when nothing is registered.
/// Returns the number of passes the target accepted.
size_t RunPostProcess(ShaderRegistry& registry, IPostProcessTarget* target,
                      const Vec2& screenSize);

}  // namespace Berry
