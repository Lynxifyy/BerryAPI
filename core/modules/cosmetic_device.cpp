// The on-device cosmetic render path: bone source, player-model hook, and the
// render seam that binds resolved items to uploaded geometry.
// See include/berry/cosmetic_device.h for the design.
#include "berry/cosmetic_device.h"
#include "berry/logger.h"

namespace Berry {

namespace {
/// The hook the static detour forwards to. A backend patches one target with
/// one plain function pointer, so there is exactly one live player-model hook;
/// `Install` claims it and `Uninstall` releases it. A second install is refused
/// rather than silently sharing the target.
PlayerModelHook* g_playerModelHook = nullptr;
}  // namespace

//----------------------------------------------------------------------------
// PlayerModelSkeletonSource
//----------------------------------------------------------------------------

void PlayerModelSkeletonSource::SetReader(PlayerModelBoneReader reader) {
  std::lock_guard<std::mutex> lk(mutex_);
  reader_ = std::move(reader);
}

bool PlayerModelSkeletonSource::hasReader() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return reader_.valid();
}

bool PlayerModelSkeletonSource::BoneTransform(EntityId entity, Bone bone,
                                              CosmeticBoneTransform* out) const {
  // Copy the reader under the lock, then call it unlocked: the reader may run
  // game-side code that must not hold this mutex (and must not deadlock if it
  // re-enters the hook that installed it).
  PlayerModelBoneReader reader;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    reader = reader_;
  }
  if (!reader.valid() || !out) return false;
  return reader.boneTransform(entity, bone, out);
}

bool PlayerModelSkeletonSource::PoseRevision(EntityId entity, u64* out) const {
  PlayerModelBoneReader reader;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    reader = reader_;
  }
  if (!reader.valid() || !reader.poseRevision) return false;
  return reader.poseRevision(entity, out);
}

//----------------------------------------------------------------------------
// PlayerModelHook
//----------------------------------------------------------------------------

PlayerModelHook::~PlayerModelHook() { Uninstall(); }

void PlayerModelHook::SetReader(PlayerModelBoneReader reader) {
  std::lock_guard<std::mutex> lk(mutex_);
  reader_ = std::move(reader);
}

bool PlayerModelHook::Install(void* target, IHook* backend) {
  if (installed_) return false;
  if (!target || !backend || !backend->CanHook()) return false;
  // Fail closed: a hook with no reader would refresh nothing, so it is refused
  // rather than installed as a no-op.
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!reader_.valid()) return false;
  }
  // One detour target, one live hook: a second install would leave two objects
  // sharing the singleton, so it is refused.
  if (g_playerModelHook != nullptr) return false;
  backend_ = backend;
  hook_.SetBackend(backend);
  const HookStatus st = hook_.Install(target, &PlayerModelHook::Detour);
  if (st != HookStatus::Ok) {
    Log::Warn("PlayerModelHook: install refused (%d)", (int)st);
    backend_ = nullptr;
    return false;
  }
  g_playerModelHook = this;
  installed_ = true;
  return true;
}

void PlayerModelHook::Uninstall() {
  if (!installed_) return;
  if (g_playerModelHook == this) g_playerModelHook = nullptr;
  hook_.Reset();   // uninstalls through the backend and clears the Hook state
  installed_ = false;
  backend_ = nullptr;
}

EntityId PlayerModelHook::lastEntity() const {
  EntityId id;
  id.raw = lastRaw_;
  return id;
}

void PlayerModelHook::Handle(void* self, void* entity, void* ctx) {
  {
    std::lock_guard<std::mutex> lk(mutex_);
    ++renders_;
    lastRaw_ = reinterpret_cast<uptr>(entity);
  }
  // The reader refreshes its own bone cache for the entity being rendered. It is
  // copied under the lock so a concurrent SetReader cannot tear it; the call
  // runs unlocked so a reader that touches the game cannot deadlock here.
  PlayerModelBoneReader reader;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    reader = reader_;
  }
  if (reader.valid() && reader.boneTransform) {
    CosmeticBoneTransform bt;
    EntityId id;
    id.raw = reinterpret_cast<uptr>(entity);
    // Refresh the root transform; a device reader caches every bone it can read
    // while it has the model, so one call is enough.
    reader.boneTransform(id, Bone::Root, &bt);
  }
  // Call the original through the trampoline, so the game's own render runs.
  if (hook_.original()) hook_.original()(self, entity, ctx);
}

void PlayerModelHook::Detour(void* self, void* entity, void* ctx) {
  // The backend patches one target with one plain function pointer, so exactly
  // one hook is live at a time. The reader is stored on the hook, so the
  // singleton indirection is the hook itself.
  if (g_playerModelHook) g_playerModelHook->Handle(self, entity, ctx);
}

//----------------------------------------------------------------------------
// DeviceCosmeticRenderSeam
//----------------------------------------------------------------------------

void DeviceCosmeticRenderSeam::BeginFrame(const CameraState& cam,
                                          const Vec2& screenSize) {
  if (backend_) backend_->SetFrame(cam, screenSize);
}

void DeviceCosmeticRenderSeam::SubmitGeometry(const CosmeticDrawItem& item) {
  if (!backend_ || !backend_->ready()) {
    ++dropped_;
    return;
  }
  if (!store_ || item.geometry == 0) {
    ++dropped_;
    return;
  }
  MeshData mesh;
  if (!store_->Mesh(item.geometry, &mesh)) {
    ++dropped_;
    return;
  }
  TextureData tex;
  TextureData* texPtr = nullptr;
  if (item.texture != 0 && store_->Texture(item.texture, &tex)) {
    texPtr = &tex;
  }
  backend_->DrawMesh(item, mesh, texPtr);
  ++drawn_;
}

void DeviceCosmeticRenderSeam::Reset() {
  drawn_ = 0;
  dropped_ = 0;
  if (backend_) backend_->Reset();
}

}  // namespace Berry
