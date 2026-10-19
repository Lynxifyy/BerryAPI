#pragma once
#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/event.h"

//----------------------------------------------------------------------------
// Berry::Render — on-device GLES/EGL overlay adapter.
//
// The Renderer module owns an abstract draw batch that flows through a
// RenderSeam. On the device this adapter owns the display surface:
//
//   * Loads egl* / gl* symbols at runtime via dlsym (RTLD_DEFAULT) so the
//     injected .so never links against the game's EGL/GLES libraries at
//     build time — they may not exist on the host nor under the same SONAME
//     on every device.
//   * Wraps eglSwapBuffers: when the seam is the on-device renderer, the
//     eglSwapBuffers hook calls BeginFrame/EndFrame on the Renderer right
//     before the real swap, so a debug overlay is composited under the UI.
//
// The implementation is thin: IRenderBackend owns the GL resources and the
// frame callback. The Renderer itself stays backend-agnostic.
//----------------------------------------------------------------------------

namespace Berry {

/// Abstract GL/EGL backend. A null backend (host tests) is a no-op.
class BERRY_API IRenderBackend {
 public:
  virtual ~IRenderBackend() = default;

  /// Loads the runtime egl/gl symbols. Returns false when GL is unavailable
  /// (headless host, or dlsym lookup failed).
  virtual bool Init() = 0;

  /// Installs the eglSwapBuffers hook and enables per-frame Present.
  virtual bool Start() = 0;
  virtual void Stop() = 0;

  virtual bool running() const = 0;
  virtual const char* name() const = 0;
};

/// Instantiates the platform render backend. On-device this is the GLES
/// overlay; on the host it is a null stub (name() == "null").
BERRY_API IRenderBackend* CreateRenderBackend();

}  // namespace Berry