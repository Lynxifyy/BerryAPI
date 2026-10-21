// Cosmetics data model: registry, loadout/wardrobe and ownership.
// See include/berry/cosmetic.h for the rules.
#include "berry/cosmetic.h"
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
  // duplicate-id rule makes a second call a no-op for each asset.
  const CosmeticAsset builtins[] = {
      {"builtin.cape.red", CosmeticSlot::Back, Bone::Back, 1001, 2001,
       Vec3(0, 0, -0.1f), Vec3(0, 0, 0), 1.0f, Vec3(1, 1, 1), "builtin",
       "BerryAPI", {"cape"}},
      {"builtin.wings.angel", CosmeticSlot::Shoulders, Bone::Torso, 1002, 2002,
       Vec3(0, 0.4f, -0.1f), Vec3(0, 0, 0), 1.0f, Vec3(1, 1, 1), "builtin",
       "BerryAPI", {"wings"}},
      {"builtin.hat.top", CosmeticSlot::Head, Bone::Head, 1003, 2003,
       Vec3(0, 0.25f, 0), Vec3(0, 0, 0), 1.0f, Vec3(1, 1, 1), "builtin",
       "BerryAPI", {"hat"}},
      {"builtin.aura.spark", CosmeticSlot::Body, Bone::Torso, 1004, 2004,
       Vec3(0, 0.5f, 0), Vec3(0, 0, 0), 1.0f, Vec3(1, 1, 1), "builtin",
       "BerryAPI", {"aura"}},
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
// Attach composition
//----------------------------------------------------------------------------

CosmeticDrawItem ComposeAttach(const CosmeticAsset& asset,
                               const CosmeticBoneTransform& bone,
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
  const Vec3 off = asset.offset * bone.scale;
  const Vec3 rotated(off.x * c + off.z * s, off.y, -off.x * s + off.z * c);

  item.position = bone.position + rotated;
  item.rotation = Vec3(bone.rotation.x + asset.rotation.x,
                       bone.rotation.y + asset.rotation.y,
                       bone.rotation.z + asset.rotation.z);
  item.scale = bone.scale * asset.scale;
  item.tint = asset.tint;
  return item;
}

//----------------------------------------------------------------------------
// CosmeticService
//----------------------------------------------------------------------------

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
  for (const auto& asset : loadout.Resolved()) {
    CosmeticBoneTransform bone;
    if (!skel->BoneTransform(entity, asset.ResolvedBone(), &bone) ||
        !bone.valid) {
      if (skippedNoBone) ++*skippedNoBone;
      continue;
    }
    items.push_back(ComposeAttach(asset, bone, entity));
  }
  return items;
}

std::vector<CosmeticDrawItem> CosmeticService::Resolve(
    EntityId entity, const CosmeticLoadout& loadout) const {
  u64 skipped = 0;
  auto items = ResolveLocked(entity, loadout, skeleton_, &skipped);
  std::lock_guard<std::mutex> lk(mutex_);
  stats_.skippedNoBone += skipped;
  return items;
}

size_t CosmeticService::PresentLocal(EntityId localPlayer) {
  // Copy the loadout under the lock, then resolve and submit unlocked: the
  // seam callback must not run while the script worker waits on this mutex.
  CosmeticSkeletonSource* skel = skeleton_;
  CosmeticRenderSeam* seam = seam_;
  if (!seam || !skel) return 0;
  CosmeticLoadout snapshot;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    ++stats_.frames;
    snapshot = local_;
  }
  u64 skipped = 0;
  const auto items = ResolveLocked(localPlayer, snapshot, skel, &skipped);
  for (const auto& item : items) seam->SubmitGeometry(item);
  {
    std::lock_guard<std::mutex> lk(mutex_);
    stats_.submitted += items.size();
    stats_.skippedNoBone += skipped;
  }
  return items.size();
}

size_t CosmeticService::PresentRemote(const std::string& playerId,
                                      EntityId entity) {
  CosmeticSkeletonSource* skel = skeleton_;
  CosmeticRenderSeam* seam = seam_;
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
  const auto items = ResolveLocked(entity, snapshot, skel, &skipped);
  for (const auto& item : items) seam->SubmitGeometry(item);
  {
    std::lock_guard<std::mutex> lk(mutex_);
    stats_.submitted += items.size();
    stats_.skippedNoBone += skipped;
  }
  return items.size();
}

CosmeticStats CosmeticService::stats() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return stats_;
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
