#pragma once
#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/event.h"
#include "berry/render.h"
#include "berry/pipeline.h"

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
// The geometry path is the same on the host and the device: `CreateBackendSeam`
// wraps a backend in a RenderSeam that coalesces the frame's primitives into a
// vertex batch (`BatchBuilder`) and hands it to the backend in two draw calls.
// The host harness therefore exercises the real batch conversion, and the
// device backend only has to upload vertices.
//
// The implementation is thin: IRenderBackend owns the GL resources. The
// Renderer itself stays backend-agnostic.
//----------------------------------------------------------------------------

namespace Berry {

/// Abstract GL/EGL backend. A null backend (host tests) records nothing.
class BERRY_API IRenderBackend {
 public:
  virtual ~IRenderBackend() = default;

  /// Loads the runtime egl/gl symbols. Returns false when GL is unavailable
  /// (headless host, or dlsym lookup failed).
  virtual bool Init() = 0;

  /// Installs the eglSwapBuffers hook and enables per-frame Present.
  virtual bool Start() = 0;
  virtual void Stop() = 0;

  /// Uploads and draws one frame's coalesced geometry. `screenSize` is in
  /// physical pixels.
  virtual void PresentBatch(const DrawBatch& batch, const Vec2& screenSize) = 0;

  /// The dynamic-resolution scale to render at: 1.0 native, below 1.0 a
  /// fraction. Called only when the value changes. The default is a no-op, so a
  /// backend that cannot rescale is unaffected.
  virtual void SetRenderScale(f32 scale) { (void)scale; }

  /// The current presentation surface size in physical pixels, or (0,0) when
  /// unknown (headless, or no current surface). The client reads this each
  /// frame to size the overlay; a zero size makes the overlays skip the frame
  /// rather than draw at a bogus scale. The default reports unknown, so a
  /// backend that cannot answer fails closed.
  virtual Vec2 SurfaceSize() const { return Vec2(0, 0); }

  virtual bool running() const = 0;
  virtual const char* name() const = 0;
};

/// Instantiates the platform render backend. On-device this is the GLES
/// overlay; on the host it is a null stub (name() == "null").
BERRY_API IRenderBackend* CreateRenderBackend();

/// Wraps `backend` in a RenderSeam that builds a `DrawBatch` from each frame and
/// forwards it to `PresentBatch`. The returned seam is owned by the caller and
/// must not outlive `backend`. Returns nullptr when `backend` is null.
BERRY_API RenderSeam* CreateBackendSeam(IRenderBackend* backend);

}  // namespace Berry
