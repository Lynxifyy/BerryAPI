#pragma once
// The on-device cosmetic render path: real bones, real geometry, real draws.
//
// `cosmetic.h` defines the two seams the core renders through — a
// `CosmeticSkeletonSource` (per-bone world transforms) and a
// `CosmeticRenderSeam` (submit resolved geometry). The core deliberately does
// not know where either comes from. This header is the device half:
//
//   * **`PlayerModelSkeletonSource`** answers bone transforms from the game's
//     own player model. It reads through a `PlayerModelBoneReader` — a per-build
//     set of accessors filled from the resolved player-render function. With no
//     reader bound it fails closed (no bone resolves), so a build whose layout
//     is unknown renders nothing rather than a wrong rig.
//   * **`PlayerModelHook`** installs the detour on the game's player-render
//     function through the pluggable `IHook` backend. Each time the game
//     renders an entity it calls the reader, which caches that entity's bone
//     transforms. That cache is what the skeleton source answers from, so the
//     cosmetics attach to the *rendered* pose, not a guessed one.
//   * **`ICosmeticMeshBackend`** is the geometry sink: given a resolved draw
//     item and the loaded `MeshData`/`TextureData`, it uploads (once) and draws.
//     The GLES implementation is `cosmetic_gles.cpp`; a null backend is used
//     headless, so the seam is exercised without GL.
//   * **`DeviceCosmeticRenderSeam`** is the `CosmeticRenderSeam` the client
//     installs on the service. It resolves each item's geometry/texture handles
//     against `CosmeticAssetStore` and hands the pair to the mesh backend.
//
// Fail closed throughout: an unresolved handle, a missing reader, a null mesh
// backend, or a hook the backend refuses all leave the frame undrawn rather
// than drawing the wrong thing. Nothing here writes game memory: the hook reads
// the transform the game already computed.

#include "berry/cosmetic.h"
#include "berry/cosmetic_assets.h"
#include "berry/export.h"
#include "berry/game.h"
#include "berry/hooks.h"
#include "berry/types.h"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace Berry {

//----------------------------------------------------------------------------
// Per-build bone accessors.
//----------------------------------------------------------------------------

/// The accessors a device adapter fills from the resolved player-render code.
/// Both are per build; an empty `boneTransform` makes the source fail closed.
struct BERRY_API PlayerModelBoneReader {
  /// Fills the world transform for `bone` on `entity`. Returns false when the
  /// entity is not resolvable or the build's layout does not describe it.
  std::function<bool(EntityId entity, Bone bone,
                     CosmeticBoneTransform* out)> boneTransform;
  /// A revision of `entity`'s pose, bumped whenever a bone moves, so the
  /// cosmetics offset cache can reuse a stationary resolve. Empty means "no
  /// revision", which disables the cache (correctness over speed).
  std::function<bool(EntityId entity, u64* out)> poseRevision;

  bool valid() const { return static_cast<bool>(boneTransform); }
};

/// Answers bone transforms from a `PlayerModelBoneReader`. Thread-safe: the
/// render hook writes a reader on the game thread while the cosmetic resolve
/// reads on the frame thread, so the reader is swapped under a lock and copied
/// before use.
class BERRY_API PlayerModelSkeletonSource final : public CosmeticSkeletonSource {
 public:
  /// Installs the per-build reader. An empty reader clears it, so the source
  /// then resolves nothing (fail closed).
  void SetReader(PlayerModelBoneReader reader);
  bool hasReader() const;

  bool BoneTransform(EntityId entity, Bone bone,
                     CosmeticBoneTransform* out) const override;
  bool PoseRevision(EntityId entity, u64* out) const override;

 private:
  mutable std::mutex mutex_;
  PlayerModelBoneReader reader_;
};

//----------------------------------------------------------------------------
// Player-model hook.
//----------------------------------------------------------------------------

/// Installs a detour on the game's player-render function and, on each call,
/// asks the bound reader to refresh the entity's bone cache. The detour ABI is
/// `void(void* self, void* entity, void* renderContext)` — the shape every
/// Bedrock actor-render entry point shares — and the caller supplies the
/// per-build target address (resolved by signature scan). Fail closed: a null
/// target, a backend that cannot hook, or no reader leaves no hook behind.
///
/// This is the mechanism that makes bones available: the cosmetic renderer
/// resolves a bone from the *rendered* pose because the hook refreshed it this
/// frame. With no hook installed the source resolves nothing.
class BERRY_API PlayerModelHook {
 public:
  using RenderFn = void (*)(void*, void*, void*);

  PlayerModelHook() = default;
  ~PlayerModelHook();

  PlayerModelHook(const PlayerModelHook&) = delete;
  PlayerModelHook& operator=(const PlayerModelHook&) = delete;

  /// Binds the reader the detour refreshes. Safe to call before or after
  /// `Install`.
  void SetReader(PlayerModelBoneReader reader);

  /// Installs the detour. `backend` is the hook engine. Returns false on a
  /// refusal; on success `installed()` is true.
  bool Install(void* target, IHook* backend);

  /// Removes the detour. Idempotent.
  void Uninstall();

  bool installed() const { return installed_; }
  /// How many times the game rendered an entity through this hook.
  u64 renders() const { return renders_; }
  /// The last entity the hook saw, or 0.
  EntityId lastEntity() const;

 private:
  void Handle(void* self, void* entity, void* ctx);
  static void Detour(void* self, void* entity, void* ctx);

  IHook* backend_ = nullptr;
  Hook<RenderFn> hook_;
  PlayerModelBoneReader reader_;
  mutable std::mutex mutex_;
  bool installed_ = false;
  u64 renders_ = 0;
  u64 lastRaw_ = 0;
};

//----------------------------------------------------------------------------
// Mesh backend + render seam.
//----------------------------------------------------------------------------

/// The geometry sink. Given a resolved draw item and the loaded mesh/texture
/// data, it uploads on first sight and draws. Implemented for GLES on device;
/// a null implementation records nothing (host/headless).
class BERRY_API ICosmeticMeshBackend {
 public:
  virtual ~ICosmeticMeshBackend() = default;

  /// Loads the runtime GL symbols. Returns false when GL is unavailable, so the
  /// seam draws nothing rather than calling into a missing library.
  virtual bool Init() = 0;
  virtual bool ready() const = 0;
  virtual const char* name() const = 0;

  /// Draws one resolved item. `mesh`/`texture` are borrowed and outlive the
  /// call; `texture` is null when the item names no texture. The backend
  /// uploads the pair on first sight (keyed by the item's handles) and reuses
  /// the upload while the handles are unchanged.
  virtual void DrawMesh(const CosmeticDrawItem& item, const MeshData& mesh,
                        const TextureData* texture) = 0;

  /// Adopts the frame's camera and surface size, so a mesh projects through the
  /// same camera the overlay uses. Called once per present before any
  /// `DrawMesh`. The default is a no-op (a backend that only records).
  virtual void SetFrame(const CameraState& cam, const Vec2& screenSize) {
    (void)cam;
    (void)screenSize;
  }

  /// Drops every uploaded GPU resource (handles are then re-uploaded on next
  /// use). Called when the asset store is cleared.
  virtual void Reset() {}
};

/// Instantiates the platform mesh backend. On device this is the GLES mesh
/// path; on the host it is a null stub (name() == "null").
BERRY_API ICosmeticMeshBackend* CreateCosmeticMeshBackend();

/// The `CosmeticRenderSeam` the client installs on the service. It resolves
/// each item's geometry/texture handles against `store` and hands the pair to
/// `backend`. A null backend or an unresolved handle draws nothing (fail
/// closed). The store and backend are borrowed and must outlive the seam.
class BERRY_API DeviceCosmeticRenderSeam final : public CosmeticRenderSeam {
 public:
  DeviceCosmeticRenderSeam(CosmeticAssetStore* store, ICosmeticMeshBackend* backend)
      : store_(store), backend_(backend) {}

  void SubmitGeometry(const CosmeticDrawItem& item) override;

  /// Adopts the frame camera into the mesh backend before any mesh is drawn, so
  /// a cosmetic projects through the frame's own camera. A null backend ignores
  /// it; an invalid camera makes the backend draw nothing (fail closed).
  void BeginFrame(const CameraState& cam, const Vec2& screenSize) override;
  void EndFrame() override {}

  /// How many items were drawn / dropped since construction. A drop is an
  /// unresolved geometry handle or a backend that is not ready.
  u64 drawn() const { return drawn_; }
  u64 dropped() const { return dropped_; }

  void Reset();

 private:
  CosmeticAssetStore* store_ = nullptr;
  ICosmeticMeshBackend* backend_ = nullptr;
  u64 drawn_ = 0;
  u64 dropped_ = 0;
};

}  // namespace Berry
