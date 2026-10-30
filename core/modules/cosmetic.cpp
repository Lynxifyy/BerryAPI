// Cosmetics data model: registry, loadout/wardrobe and ownership.
// See include/berry/cosmetic.h for the rules.
#include "berry/cosmetic.h"
#include "berry/cosmetic_assets.h"
#include "berry/logger.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Berry {

//----------------------------------------------------------------------------
// Slots / bones
//----------------------------------------------------------------------------

namespace {
struct SlotRow { const char* name; CosmeticSlot slot; };
const SlotRow kSlots[] = {
    {"head", CosmeticSlot::Head},       {"back", CosmeticSlot::Back},
    {"shoulders", CosmeticSlot::Shoulders}, {"body", CosmeticSlot::Body},
    {"trail", CosmeticSlot::Trail},     {"effect", CosmeticSlot::Effect},
    {"emote", CosmeticSlot::Emote},
};

struct BoneRow { const char* name; Bone bone; };
const BoneRow kBones[] = {
    {"root", Bone::Root},         {"head", Bone::Head},
    {"torso", Bone::Torso},       {"left_arm", Bone::LeftArm},
    {"right_arm", Bone::RightArm}, {"left_leg", Bone::LeftLeg},
    {"right_leg", Bone::RightLeg}, {"back", Bone::Back},
};
}  // namespace

const char* CosmeticSlotName(CosmeticSlot s) {
  for (const auto& row : kSlots) {
    if (row.slot == s) return row.name;
  }
  return "none";
}

CosmeticSlot CosmeticSlotFromName(const std::string& name) {
  for (const auto& row : kSlots) {
    if (name == row.name) return row.slot;
  }
  return CosmeticSlot::None;
}

const char* BoneName(Bone b) {
  for (const auto& row : kBones) {
    if (row.bone == b) return row.name;
  }
  return "none";
}

Bone BoneFromName(const std::string& name) {
  for (const auto& row : kBones) {
    if (name == row.name) return row.bone;
  }
  return Bone::None;
}

Bone DefaultBoneFor(CosmeticSlot slot) {
  switch (slot) {
    case CosmeticSlot::Head: return Bone::Head;
    case CosmeticSlot::Back: return Bone::Back;
    case CosmeticSlot::Shoulders: return Bone::Torso;
    case CosmeticSlot::Body: return Bone::Torso;
    case CosmeticSlot::Trail: return Bone::Root;
    case CosmeticSlot::Effect: return Bone::Root;
    case CosmeticSlot::Emote: return Bone::Root;
    default: return Bone::None;
  }
}

//----------------------------------------------------------------------------
// Errors
//----------------------------------------------------------------------------

const char* CosmeticErrorName(CosmeticError e) {
  switch (e) {
    case CosmeticError::Ok: return "ok";
    case CosmeticError::EmptyId: return "empty asset id";
    case CosmeticError::DuplicateId: return "duplicate asset id";
    case CosmeticError::UnknownAsset: return "unknown asset";
    case CosmeticError::UnknownSlot: return "unknown slot";
    case CosmeticError::NotOwned: return "asset not owned";
    case CosmeticError::BadTransform: return "non-finite transform";
    case CosmeticError::BadScale: return "scale must be positive";
    case CosmeticError::BadAnimation: return "bad animation name or speed";
    case CosmeticError::SlotOccupied: return "slot already occupied";
    case CosmeticError::TooManyAssets: return "registry full";
  }
  return "unknown";
}

bool ValidCosmeticTransform(const CosmeticAsset& a, CosmeticError* out) {
  auto finite3 = [](const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
  };
  if (!finite3(a.offset) || !finite3(a.rotation) || !finite3(a.tint)) {
    if (out) *out = CosmeticError::BadTransform;
    return false;
  }
  if (!std::isfinite(a.scale) || a.scale <= 0.0f) {
    if (out) *out = CosmeticError::BadScale;
    return false;
  }
  if (!std::isfinite(a.trail) || a.trail < 0.0f || a.trail > 1.0f) {
    if (out) *out = CosmeticError::BadTransform;
    return false;
  }
  // A custom-animation name is bounded; a bad speed would make the clip jump
  // (non-finite) or run backwards (non-positive), so both are refused.
  if (a.animation.size() > CosmeticAsset::kMaxAnimationNameLen) {
    if (out) *out = CosmeticError::BadAnimation;
    return false;
  }
  if (!std::isfinite(a.animationSpeed) || a.animationSpeed <= 0.0f) {
    if (out) *out = CosmeticError::BadAnimation;
    return false;
  }
  if (out) *out = CosmeticError::Ok;
  return true;
}

//----------------------------------------------------------------------------
// Registry
//----------------------------------------------------------------------------

CosmeticRegistry& CosmeticRegistry::Instance() {
  static CosmeticRegistry instance;
  return instance;
}

bool CosmeticRegistry::Register(const CosmeticAsset& asset, CosmeticError* out) {
  if (asset.id.empty()) {
    if (out) *out = CosmeticError::EmptyId;
    return false;
  }
  if (asset.slot == CosmeticSlot::None) {
    if (out) *out = CosmeticError::UnknownSlot;
    return false;
  }
  if (!ValidCosmeticTransform(asset, out)) return false;

  std::lock_guard<std::mutex> lk(mutex_);
  if (index_.find(asset.id) != index_.end()) {
    if (out) *out = CosmeticError::DuplicateId;
    return false;
  }
  if (assets_.size() >= kMaxAssets) {
    if (out) *out = CosmeticError::TooManyAssets;
    return false;
  }
  index_[asset.id] = assets_.size();
  assets_.push_back(asset);
  if (out) *out = CosmeticError::Ok;
  return true;
}

bool CosmeticRegistry::RegisterAll(const std::vector<CosmeticAsset>& assets,
                                   CosmeticError* out) {
  // Validate the whole batch first, so a failure registers nothing. The batch
  // is also checked against itself, so a pack cannot smuggle a duplicate past
  // the per-asset rule by listing the id twice in one document.
  std::vector<std::string> seen;
  seen.reserve(assets.size());
  for (const auto& a : assets) {
    if (a.id.empty()) {
      if (out) *out = CosmeticError::EmptyId;
      return false;
    }
    if (a.slot == CosmeticSlot::None) {
      if (out) *out = CosmeticError::UnknownSlot;
      return false;
    }
    if (!ValidCosmeticTransform(a, out)) return false;
    for (const auto& id : seen) {
      if (id == a.id) {
        if (out) *out = CosmeticError::DuplicateId;
        return false;
      }
    }
    seen.push_back(a.id);
  }

  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& a : assets) {
    if (index_.find(a.id) != index_.end()) {
      if (out) *out = CosmeticError::DuplicateId;
      return false;
    }
  }
  if (assets_.size() + assets.size() > kMaxAssets) {
    if (out) *out = CosmeticError::TooManyAssets;
    return false;
  }
  for (const auto& a : assets) {
    index_[a.id] = assets_.size();
    assets_.push_back(a);
  }
  if (out) *out = CosmeticError::Ok;
  return true;
}

bool CosmeticRegistry::Contains(const std::string& id) const {
  std::lock_guard<std::mutex> lk(mutex_);
  return index_.find(id) != index_.end();
}

bool CosmeticRegistry::Find(const std::string& id, CosmeticAsset* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = index_.find(id);
  if (it == index_.end()) return false;
  if (out) *out = assets_[it->second];
  return true;
}

std::vector<CosmeticAsset> CosmeticRegistry::ForSlot(CosmeticSlot slot) const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<CosmeticAsset> out;
  for (const auto& a : assets_) {
    if (a.slot == slot) out.push_back(a);
  }
  return out;
}

std::vector<CosmeticAsset> CosmeticRegistry::All() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return assets_;
}

size_t CosmeticRegistry::Count() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return assets_.size();
}

void CosmeticRegistry::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  assets_.clear();
  index_.clear();
}

size_t CosmeticRegistry::RegisterBuiltins() {
  // A small starter set so a fresh client shows something. Idempotent: the
  // duplicate-id rule makes a second call a no-op for each asset. Capes and
  // wings carry a trail factor so they sway with motion; rigid pieces (a hat,
  // an aura) do not.
  //
  // Each built-in gets REAL geometry and a solid-colour texture from the asset
  // store's procedural builders, so a built-in draws on a fresh client instead
  // of resolving to a null handle and rendering nothing. The handles are cached
  // per shape/colour, so the four built-ins share at most three meshes and four
  // textures.
  CosmeticAssetStore& store = CosmeticAssetStore::Instance();
  const GeometryId plane = store.BuiltinGeometry(BuiltinShape::Plane);
  const GeometryId box = store.BuiltinGeometry(BuiltinShape::Box);
  const GeometryId quad = store.BuiltinGeometry(BuiltinShape::Quad);
  const TextureId red = store.BuiltinTexture(0xd23a3aff);
  const TextureId gold = store.BuiltinTexture(0xf0d060ff);
  const TextureId white = store.BuiltinTexture(0xffffffff);
  const TextureId violet = store.BuiltinTexture(0x8a5cf0ff);

  // Named-field construction rather than a positional aggregate: the asset
  // struct grows (it gained `animation`/`animationSpeed`), and a positional
  // initializer silently mis-assigns when a field is inserted. Each built-in
  // names a clip the client registers (`builtin.cape.flap`, `builtin.aura.spin`)
  // so it animates out of the box; on a host with no clips registered the name
  // resolves to nothing and the asset is placed rigidly (fail closed).
  auto make = [](const char* id, CosmeticSlot slot, Bone bone, GeometryId geo,
                 TextureId tex, const Vec3& off, f32 trail, const char* anim,
                 f32 animSpeed, std::vector<std::string> tags) {
    CosmeticAsset a;
    a.id = id;
    a.slot = slot;
    a.bone = bone;
    a.geometry = geo;
    a.texture = tex;
    a.offset = off;
    a.rotation = Vec3(0, 0, 0);
    a.scale = 1.0f;
    a.tint = Vec3(1, 1, 1);
    a.trail = trail;
    a.pack = "builtin";
    a.author = "BerryAPI";
    a.animation = anim ? anim : "";
    a.animationSpeed = animSpeed;
    a.tags = std::move(tags);
    return a;
  };

  const CosmeticAsset builtins[] = {
      make("builtin.cape.red", CosmeticSlot::Back, Bone::Back, plane, red,
           Vec3(0, 0, -0.1f), 0.8f, "builtin.cape.flap", 1.0f, {"cape"}),
      make("builtin.wings.angel", CosmeticSlot::Shoulders, Bone::Torso, plane,
           white, Vec3(0, 0.4f, -0.1f), 0.6f, "builtin.cape.flap", 0.8f,
           {"wings"}),
      make("builtin.hat.top", CosmeticSlot::Head, Bone::Head, box, gold,
           Vec3(0, 0.25f, 0), 0.0f, nullptr, 1.0f, {"hat"}),
      make("builtin.aura.spark", CosmeticSlot::Body, Bone::Torso, quad, violet,
           Vec3(0, 0.5f, 0), 0.2f, "builtin.aura.spin", 1.0f, {"aura"}),
  };
  size_t added = 0;
  for (const auto& a : builtins) {
    CosmeticError err = CosmeticError::Ok;
    if (Register(a, &err)) ++added;
  }
  return added;
}

//----------------------------------------------------------------------------
// Loadout
//----------------------------------------------------------------------------

bool CosmeticLoadout::Equip(const std::string& assetId, CosmeticError* out) {
  CosmeticAsset asset;
  if (!CosmeticRegistry::Instance().Find(assetId, &asset)) {
    if (out) *out = CosmeticError::UnknownAsset;
    return false;
  }
  return Equip(asset, out);
}

bool CosmeticLoadout::Equip(const CosmeticAsset& asset, CosmeticError* out) {
  if (!asset.valid()) {
    if (out) *out = asset.id.empty() ? CosmeticError::EmptyId
                                     : CosmeticError::UnknownSlot;
    return false;
  }
  if (!ValidCosmeticTransform(asset, out)) return false;
  slots_[asset.slot] = asset.id;   // exclusive: replaces whatever was there
  if (out) *out = CosmeticError::Ok;
  return true;
}

bool CosmeticLoadout::Unequip(CosmeticSlot slot) {
  auto it = slots_.find(slot);
  if (it == slots_.end()) return false;
  slots_.erase(it);
  return true;
}

std::string CosmeticLoadout::Equipped(CosmeticSlot slot) const {
  auto it = slots_.find(slot);
  return it == slots_.end() ? std::string() : it->second;
}

bool CosmeticLoadout::IsEquipped(const std::string& assetId) const {
  if (assetId.empty()) return false;
  for (const auto& kv : slots_) {
    if (kv.second == assetId) return true;
  }
  return false;
}

std::vector<CosmeticAsset> CosmeticLoadout::Resolved() const {
  std::vector<CosmeticAsset> out;
  auto& reg = CosmeticRegistry::Instance();
  for (const auto& kv : slots_) {
    CosmeticAsset a;
    if (reg.Find(kv.second, &a)) out.push_back(a);
  }
  return out;
}

std::string CosmeticLoadout::Canonical() const {
  // Slot order is the map's (kind) order, so the same loadout always frames
  // the same bytes regardless of the order it was built in.
  std::ostringstream os;
  for (const auto& kv : slots_) {
    os << CosmeticSlotName(kv.first) << '=' << kv.second << '\n';
  }
  return os.str();
}

bool CosmeticLoadout::FromCanonical(const std::string& text) {
  std::map<CosmeticSlot, std::string> parsed;
  std::istringstream is(text);
  std::string line;
  while (std::getline(is, line)) {
    if (line.empty()) continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return false;   // malformed: fail closed
    const std::string slotName = line.substr(0, eq);
    const std::string assetId = line.substr(eq + 1);
    const CosmeticSlot slot = CosmeticSlotFromName(slotName);
    if (slot == CosmeticSlot::None) return false;
    if (assetId.empty()) return false;
    parsed[slot] = assetId;
  }
  slots_ = std::move(parsed);
  return true;
}

//----------------------------------------------------------------------------
// Wardrobe
//----------------------------------------------------------------------------

bool CosmeticWardrobe::Save(const std::string& name, const CosmeticLoadout& loadout) {
  if (name.empty()) return false;
  auto it = presets_.find(name);
  if (it != presets_.end()) {
    it->second = loadout;
    return true;
  }
  if (presets_.size() >= kMaxPresets) return false;
  presets_[name] = loadout;
  return true;
}

bool CosmeticWardrobe::Load(const std::string& name, CosmeticLoadout* out) const {
  auto it = presets_.find(name);
  if (it == presets_.end()) return false;
  if (out) *out = it->second;
  return true;
}

bool CosmeticWardrobe::Remove(const std::string& name) {
  return presets_.erase(name) > 0;
}

bool CosmeticWardrobe::Has(const std::string& name) const {
  return presets_.find(name) != presets_.end();
}

std::vector<std::string> CosmeticWardrobe::Names() const {
  std::vector<std::string> out;
  out.reserve(presets_.size());
  for (const auto& kv : presets_) out.push_back(kv.first);
  return out;
}

size_t CosmeticWardrobe::Count() const { return presets_.size(); }

void CosmeticWardrobe::Clear() { presets_.clear(); }

//----------------------------------------------------------------------------
// Trailing sway
//----------------------------------------------------------------------------

f32 CosmeticTrailFullSpeed() { return 6.0f; }
f32 CosmeticTrailMaxLeanDeg() { return 45.0f; }
f32 CosmeticTrailMaxLag() { return 0.15f; }

CosmeticTrailPose CosmeticTrailingPose(f32 trail, const Vec3& velocity,
                                       const Vec3& boneRotation) {
  CosmeticTrailPose pose;
  (void)boneRotation;   // the lag is applied in world space, so yaw is implicit
  // A rigid asset, a non-positive trail, or a motion we cannot trust yields no
  // sway at all: the asset stays exactly where the bone puts it.
  if (!(trail > 0.0f)) return pose;
  if (trail > 1.0f) trail = 1.0f;
  if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y) ||
      !std::isfinite(velocity.z)) {
    return pose;
  }

  // Speed drives the sway. Vertical motion counts too, so a falling player's
  // cape flares the same way a sprinting one's trails.
  const f32 speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y +
                              velocity.z * velocity.z);
  if (!(speed > 0.0f)) return pose;
  const f32 k = Mathf::Clamp(speed / CosmeticTrailFullSpeed(), 0.0f, 1.0f);
  const f32 t = trail * k;

  // Lean back by an extra pitch, scaled by how slack and how fast.
  pose.rotation = Vec3(CosmeticTrailMaxLeanDeg() * t, 0, 0);

  // Lag opposite the horizontal motion. A purely vertical motion has no
  // horizontal component, so it leans without dragging sideways.
  const f32 hx = velocity.x, hz = velocity.z;
  const f32 hs = std::sqrt(hx * hx + hz * hz);
  if (hs > 1e-4f) {
    const f32 lag = CosmeticTrailMaxLag() * t;
    pose.offset = Vec3(-(hx / hs) * lag, 0, -(hz / hs) * lag);
  }
  pose.velocity = velocity;
  return pose;
}

//----------------------------------------------------------------------------
// Attach composition
//----------------------------------------------------------------------------

CosmeticDrawItem ComposeAttach(const CosmeticAsset& asset,
                               const CosmeticBoneTransform& bone,
                               EntityId entity) {
  CosmeticAnimPose none;
  return ComposeAttach(asset, bone, none, entity);
}

CosmeticDrawItem ComposeAttach(const CosmeticAsset& asset,
                               const CosmeticBoneTransform& bone,
                               const CosmeticAnimPose& anim,
                               EntityId entity) {
  CosmeticDrawItem item;
  item.assetId = asset.id;
  item.slot = asset.slot;
  item.bone = asset.ResolvedBone();
  item.geometry = asset.geometry;
  item.texture = asset.texture;
  item.entity = entity;

  // The bone transform places the attachment point in the world; the asset's
  // offset is scaled by the bone's scale and rotated by the bone's yaw, so a
  // cape stays behind the back as the player turns.
  const f32 yaw = bone.rotation.y * Mathf::DEG2RAD;
  const f32 c = std::cos(yaw), s = std::sin(yaw);

  // The custom-animation offset, when one applies, is a bone-local displacement:
  // scale it by the bone's scale and rotate it by the bone's yaw exactly like
  // the static offset, so a clip authored for a bone composes the same way the
  // asset does. Its rotation and scale add to the static values.
  Vec3 staticOffset = asset.offset;
  Vec3 animRotation;
  f32 animScale = 1.0f;
  if (anim.valid) {
    staticOffset = staticOffset + anim.offset;
    animRotation = anim.rotation;
    animScale = anim.scale;
  }
  const Vec3 off = staticOffset * bone.scale;
  const Vec3 rotated(off.x * c + off.z * s, off.y, -off.x * s + off.z * c);

  // Slack assets lag and lean with the player's motion, so a cape is not welded
  // stiffly to the back while sprinting or falling. A rigid asset (trail == 0)
  // or a still player contributes nothing.
  const CosmeticTrailPose trail =
      CosmeticTrailingPose(asset.trail, bone.velocity, bone.rotation);

  item.position = bone.position + rotated + trail.offset;
  item.rotation = Vec3(bone.rotation.x + asset.rotation.x + animRotation.x +
                           trail.rotation.x,
                       bone.rotation.y + asset.rotation.y + animRotation.y +
                           trail.rotation.y,
                       bone.rotation.z + asset.rotation.z + animRotation.z +
                           trail.rotation.z);
  item.scale = bone.scale * asset.scale * animScale;
  item.tint = asset.tint;
  item.velocity = trail.velocity;
  if (anim.valid) item.animation = asset.animation;
  return item;
}

//----------------------------------------------------------------------------
// CosmeticService
//----------------------------------------------------------------------------

namespace {

/// Opens the seam's frame with the current camera, or a no-op when no camera
/// source is bound or it reports none. Factored out so every present path
/// brackets its submissions identically.
void OpenSeamFrame(CosmeticRenderSeam* seam, CosmeticCameraSource* camera,
                   bool* opened) {
  *opened = false;
  if (!seam) return;
  CameraState cam;
  if (camera && camera->Camera(&cam)) {
    // The surface size comes from the camera snapshot (the same projection the
    // overlay uses), so a seam never has to ask the renderer separately.
    seam->BeginFrame(cam, Vec2(cam.width, cam.height));
  } else {
    // No camera: open with an invalid one so the seam knows this is a real
    // present that simply cannot project, rather than skipping the frame.
    CameraState none;
    seam->BeginFrame(none, Vec2(0, 0));
  }
  *opened = true;
}

}  // namespace

CosmeticLoadout CosmeticService::localLoadout() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return local_;
}

bool CosmeticService::Equip(const std::string& assetId, CosmeticError* out) {
  // Resolve outside the lock: the registry has its own.
  CosmeticAsset asset;
  if (!CosmeticRegistry::Instance().Find(assetId, &asset)) {
    if (out) *out = CosmeticError::UnknownAsset;
    return false;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  return local_.Equip(asset, out);
}

bool CosmeticService::Unequip(CosmeticSlot slot) {
  std::lock_guard<std::mutex> lk(mutex_);
  return local_.Unequip(slot);
}

std::string CosmeticService::Equipped(CosmeticSlot slot) const {
  std::lock_guard<std::mutex> lk(mutex_);
  return local_.Equipped(slot);
}

bool CosmeticService::SavePreset(const std::string& name) {
  std::lock_guard<std::mutex> lk(mutex_);
  return wardrobe_.Save(name, local_);
}

bool CosmeticService::ApplyPreset(const std::string& name) {
  std::lock_guard<std::mutex> lk(mutex_);
  CosmeticLoadout loaded;
  if (!wardrobe_.Load(name, &loaded)) return false;
  local_ = loaded;
  return true;
}

bool CosmeticService::HasPreset(const std::string& name) const {
  std::lock_guard<std::mutex> lk(mutex_);
  return wardrobe_.Has(name);
}

std::vector<std::string> CosmeticService::PresetNames() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return wardrobe_.Names();
}

size_t CosmeticService::SetRemoteLoadout(const std::string& playerId,
                                         const CosmeticLoadout& loadout) {
  if (playerId.empty()) return 0;
  // Ownership has its own lock; check it before taking ours so the two are
  // never held at once.
  auto& ownership = CosmeticOwnership::Instance();
  CosmeticLoadout filtered;
  u64 notOwned = 0;
  for (const auto& kv : loadout.slots()) {
    // Only assets the player provably owns survive; a forged loadout renders
    // nothing it does not own.
    if (!ownership.Owns(playerId, kv.second)) {
      ++notOwned;
      continue;
    }
    CosmeticAsset asset;
    if (CosmeticRegistry::Instance().Find(kv.second, &asset)) {
      filtered.Equip(asset);
    }
  }
  std::lock_guard<std::mutex> lk(mutex_);
  remote_[playerId] = filtered;
  stats_.skippedNotOwned += notOwned;
  return filtered.Count();
}

bool CosmeticService::ClearRemoteLoadout(const std::string& playerId) {
  std::lock_guard<std::mutex> lk(mutex_);
  return remote_.erase(playerId) > 0;
}

std::vector<std::string> CosmeticService::RemotePlayers() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<std::string> out;
  out.reserve(remote_.size());
  for (const auto& kv : remote_) out.push_back(kv.first);
  return out;
}

std::vector<CosmeticDrawItem> CosmeticService::ResolveLocked(
    EntityId entity, const CosmeticLoadout& loadout,
    const CosmeticSkeletonSource* skel, u64* skippedNoBone) const {
  std::vector<CosmeticDrawItem> items;
  if (!skel) return items;
  // The velocity source is read once per resolve and applied to any bone that
  // did not report its own motion, so a skeleton seam that only knows bone
  // positions still produces trailing sway.
  const Vec3 velocity = LocalVelocity();
  // The custom-animation clock, read once: every animated asset on this entity
  // samples against the same instant, so a cape and its aura stay in phase.
  const f32 animTime = animClock_.load();
  const CosmeticAnimationSource* anim = animationSource_;
  for (const auto& asset : loadout.Resolved()) {
    CosmeticBoneTransform bone;
    if (!skel->BoneTransform(entity, asset.ResolvedBone(), &bone) ||
        !bone.valid) {
      if (skippedNoBone) ++*skippedNoBone;
      continue;
    }
    // A bone that reports no motion of its own inherits the local player's.
    if (bone.velocity.x == 0 && bone.velocity.y == 0 &&
        bone.velocity.z == 0) {
      bone.velocity = velocity;
    }
    // A named clip, when a source can sample it, animates the asset on its own
    // bone: a flapping cape or a spinning aura. An unknown clip (or no source)
    // yields an invalid pose, and the asset is placed rigidly — fail closed.
    CosmeticAnimPose pose;
    if (anim && !asset.animation.empty()) {
      const f32 t = animTime * asset.animationSpeed;
      anim->Sample(asset.animation, asset.ResolvedBone(), t, &pose);
    }
    CosmeticDrawItem item = ComposeAttach(asset, bone, pose, entity);
    item.animationTime = pose.valid ? animTime * asset.animationSpeed : 0.0f;
    items.push_back(std::move(item));
  }
  return items;
}

std::vector<CosmeticDrawItem> CosmeticService::Resolve(
    EntityId entity, const CosmeticLoadout& loadout) const {
  u64 skipped = 0;
  bool hit = false;
  auto items = ResolveWithCache(entity, loadout, skeleton_, &skipped, &hit);
  std::lock_guard<std::mutex> lk(mutex_);
  stats_.skippedNoBone += skipped;
  if (hit) ++stats_.cacheHits; else ++stats_.cacheMisses;
  return items;
}

void CosmeticService::AdvanceAnimations(f32 dtSeconds) {
  if (!std::isfinite(dtSeconds) || dtSeconds < 0.0f) return;
  animClock_.store(animClock_.load() + dtSeconds);
}

bool CosmeticService::AnimationAppliesTo(const CosmeticLoadout& loadout) const {
  if (!animationSource_) return false;
  // A loadout's resolved assets are the registry's own rows; only an asset that
  // names a clip can move on its own. The registry is immutable during a
  // resolve, so this reads no shared mutable state.
  for (const auto& asset : loadout.Resolved()) {
    if (!asset.animation.empty()) return true;
  }
  return false;
}

void CosmeticService::EvictLruLocked() const {
  // The LRU list is ordered most-recent-first, so the victim is the tail. Both
  // the list node and the map entry are removed in O(1); no scan is needed.
  if (lru_.empty()) return;
  const ResolveCacheKey victim = lru_.back();
  lru_.pop_back();
  resolveCache_.erase(victim);
}

void CosmeticService::ClearResolveCache() {
  std::lock_guard<std::mutex> lk(cacheMutex_);
  resolveCache_.clear();
  lru_.clear();
}

size_t CosmeticService::resolveCacheSize() const {
  std::lock_guard<std::mutex> lk(cacheMutex_);
  return resolveCache_.size();
}

std::vector<CosmeticDrawItem> CosmeticService::ResolveWithCache(
    EntityId entity, const CosmeticLoadout& loadout,
    const CosmeticSkeletonSource* skel, u64* skippedNoBone, bool* hit) const {
  if (hit) *hit = false;
  std::vector<CosmeticDrawItem> items;
  if (!skel) return items;

  // The loadout is part of the cache's identity: equipping or unequipping
  // anything changes it, so the entry is invalidated. Canonical() is the same
  // stable form the sync channel frames on.
  const std::string loadoutKey = loadout.Canonical();

  // A hit additionally requires the skeleton seam to PROVE the pose has not
  // moved since the cached resolve. A seam that cannot report a revision always
  // misses, so a stale attach transform is never reused — the cache can only
  // make a correct resolve cheaper, never a wrong one appear.
  const std::pair<u64, u16> key{entity.raw, entity.runtime};
  // The injected local velocity feeds the trailing sway for any bone that does
  // not report its own motion, so it is part of the cache identity: if the
  // player sped up or slowed down, the sway changed and the entry is stale.
  const Vec3 velocity = LocalVelocity();
  // A custom-animation clock advances every frame, so an entity whose loadout
  // carries ANY animated asset produces a different pose each resolve. Such a
  // resolve is never cached: the revision would be unchanged while the pose
  // moved, and reusing it would freeze the animation. Only an all-rigid loadout
  // is cacheable.
  const bool animated = AnimationAppliesTo(loadout);
  u64 revision = 0;
  const bool haveRevision = !animated && skel->PoseRevision(entity, &revision);
  if (haveRevision) {
    std::lock_guard<std::mutex> lk(cacheMutex_);
    auto it = resolveCache_.find(key);
    if (it != resolveCache_.end() && it->second.poseRevision == revision &&
        it->second.loadoutKey == loadoutKey &&
        it->second.localVelocity == velocity) {
      // Promote to most-recently-used. Splice the existing node to the front:
      // O(1), no allocation, and the iterator stored in the entry stays valid.
      lru_.splice(lru_.begin(), lru_, it->second.lruIt);
      if (hit) *hit = true;
      return it->second.items;
    }
  }

  // Miss: resolve for real, then store the result under the pose revision so
  // the next frame can reuse it.
  items = ResolveLocked(entity, loadout, skel, skippedNoBone);
  if (haveRevision) {
    std::lock_guard<std::mutex> lk(cacheMutex_);
    // Replacing a stale entry (same key, changed revision/loadout/velocity) must
    // drop its old LRU node first, or the list would accumulate a duplicate key
    // and evict a key that is still live in the map.
    auto existing = resolveCache_.find(key);
    if (existing != resolveCache_.end()) {
      lru_.erase(existing->second.lruIt);
      resolveCache_.erase(existing);
    } else if (resolveCache_.size() >= kMaxResolveCacheEntries) {
      EvictLruLocked();
    }
    ResolveCacheEntry entry;
    entry.poseRevision = revision;
    entry.localVelocity = velocity;
    entry.loadoutKey = loadoutKey;
    entry.items = items;
    lru_.push_front(key);
    entry.lruIt = lru_.begin();
    resolveCache_[key] = std::move(entry);
  }
  return items;
}

size_t CosmeticService::PresentLocal(EntityId localPlayer) {
  // Copy the loadout under the lock, then resolve and submit unlocked: the
  // seam callback must not run while the script worker waits on this mutex.
  CosmeticSkeletonSource* skel = skeleton_;
  CosmeticRenderSeam* seam = seam_;
  CosmeticCameraSource* camera = cameraSource_;
  if (!seam || !skel) return 0;
  CosmeticLoadout snapshot;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    ++stats_.frames;
    snapshot = local_;
  }
  u64 skipped = 0;
  bool hit = false;
  const auto items = ResolveWithCache(localPlayer, snapshot, skel, &skipped, &hit);
  bool opened = false;
  OpenSeamFrame(seam, camera, &opened);
  for (const auto& item : items) seam->SubmitGeometry(item);
  if (opened) seam->EndFrame();
  {
    std::lock_guard<std::mutex> lk(mutex_);
    stats_.submitted += items.size();
    stats_.skippedNoBone += skipped;
    if (hit) ++stats_.cacheHits; else ++stats_.cacheMisses;
  }
  return items.size();
}

size_t CosmeticService::PresentRemote(const std::string& playerId,
                                      EntityId entity) {
  CosmeticSkeletonSource* skel = skeleton_;
  CosmeticRenderSeam* seam = seam_;
  CosmeticCameraSource* camera = cameraSource_;
  if (!seam || !skel) return 0;
  CosmeticLoadout snapshot;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (localOnly_.load()) return 0;
    auto it = remote_.find(playerId);
    if (it == remote_.end()) return 0;
    snapshot = it->second;
  }
  u64 skipped = 0;
  bool hit = false;
  const auto items = ResolveWithCache(entity, snapshot, skel, &skipped, &hit);
  bool opened = false;
  OpenSeamFrame(seam, camera, &opened);
  for (const auto& item : items) seam->SubmitGeometry(item);
  if (opened) seam->EndFrame();
  {
    std::lock_guard<std::mutex> lk(mutex_);
    stats_.submitted += items.size();
    stats_.skippedNoBone += skipped;
    if (hit) ++stats_.cacheHits; else ++stats_.cacheMisses;
  }
  return items.size();
}

CosmeticStats CosmeticService::stats() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return stats_;
}

//----------------------------------------------------------------------------
// Draw through Berry::Render
//----------------------------------------------------------------------------

bool CosmeticService::DrawItem(Renderer& r, const CosmeticDrawItem& item) {
  // An asset's draw item carries a world position, a scale and a tint. The
  // renderer needs a world size: a cosmetic asset is modelled at roughly one
  // block, so the quad is one block scaled by the item. A non-positive or
  // non-finite scale would make the quad vanish or invert, so it is refused
  // rather than drawn wrong.
  if (!std::isfinite(item.scale) || item.scale <= 0.0f) return false;
  Billboard3D bb;
  bb.position = item.position;
  bb.size = Vec2(item.scale, item.scale);
  bb.rollDeg = item.rotation.z;   // roll about the view axis, from the item
  const f32 cr = Mathf::Clamp(item.tint.x, 0.0f, 1.0f);
  const f32 cg = Mathf::Clamp(item.tint.y, 0.0f, 1.0f);
  const f32 cb = Mathf::Clamp(item.tint.z, 0.0f, 1.0f);
  bb.tint = Rgba((u8)(cr * 255.0f), (u8)(cg * 255.0f), (u8)(cb * 255.0f), 255);
  bb.geometry = item.geometry;
  bb.texture = item.texture;
  bb.entity = item.entity;
  return r.DrawBillboard(bb);
}

size_t CosmeticService::DrawLocal(Renderer& r, EntityId localPlayer) {
  CosmeticSkeletonSource* skel = skeleton_;
  if (!skel) return 0;
  CosmeticLoadout snapshot;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    ++stats_.frames;
    snapshot = local_;
  }
  u64 skipped = 0;
  bool hit = false;
  const auto items = ResolveWithCache(localPlayer, snapshot, skel, &skipped, &hit);
  size_t drawn = 0;
  for (const auto& item : items) {
    if (DrawItem(r, item)) ++drawn;
  }
  {
    std::lock_guard<std::mutex> lk(mutex_);
    stats_.submitted += drawn;
    stats_.skippedNoBone += skipped;
    if (hit) ++stats_.cacheHits; else ++stats_.cacheMisses;
  }
  return drawn;
}

size_t CosmeticService::DrawRemote(Renderer& r, const std::string& playerId,
                                   EntityId entity) {
  CosmeticSkeletonSource* skel = skeleton_;
  if (!skel) return 0;
  CosmeticLoadout snapshot;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (localOnly_.load()) return 0;
    auto it = remote_.find(playerId);
    if (it == remote_.end()) return 0;
    snapshot = it->second;
  }
  u64 skipped = 0;
  bool hit = false;
  const auto items = ResolveWithCache(entity, snapshot, skel, &skipped, &hit);
  size_t drawn = 0;
  for (const auto& item : items) {
    if (DrawItem(r, item)) ++drawn;
  }
  {
    std::lock_guard<std::mutex> lk(mutex_);
    stats_.submitted += drawn;
    stats_.skippedNoBone += skipped;
    if (hit) ++stats_.cacheHits; else ++stats_.cacheMisses;
  }
  return drawn;
}

//----------------------------------------------------------------------------
// Ownership
//----------------------------------------------------------------------------

CosmeticOwnership& CosmeticOwnership::Instance() {
  static CosmeticOwnership instance;
  return instance;
}

bool CosmeticOwnership::Grant(const std::string& playerId, const std::string& assetId) {
  if (playerId.empty() || assetId.empty()) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  auto& list = owned_[playerId];
  if (std::find(list.begin(), list.end(), assetId) != list.end()) return true;
  list.push_back(assetId);
  return true;
}

size_t CosmeticOwnership::GrantAll(const std::string& playerId,
                                   const std::vector<std::string>& assetIds) {
  size_t n = 0;
  for (const auto& id : assetIds) {
    if (Grant(playerId, id)) ++n;
  }
  return n;
}

bool CosmeticOwnership::Revoke(const std::string& playerId, const std::string& assetId) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = owned_.find(playerId);
  if (it == owned_.end()) return false;
  auto& list = it->second;
  auto pos = std::find(list.begin(), list.end(), assetId);
  if (pos == list.end()) return false;
  list.erase(pos);
  return true;
}

void CosmeticOwnership::RevokePlayer(const std::string& playerId) {
  std::lock_guard<std::mutex> lk(mutex_);
  owned_.erase(playerId);
}

bool CosmeticOwnership::Owns(const std::string& playerId, const std::string& assetId) const {
  if (playerId.empty() || assetId.empty()) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = owned_.find(playerId);
  if (it == owned_.end()) return false;
  return std::find(it->second.begin(), it->second.end(), assetId) != it->second.end();
}

size_t CosmeticOwnership::OwnedCount(const std::string& playerId) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = owned_.find(playerId);
  return it == owned_.end() ? 0 : it->second.size();
}

void CosmeticOwnership::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  owned_.clear();
}

}  // namespace Berry
