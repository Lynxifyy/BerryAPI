#pragma once
// BerryAPI cosmetics.
//
// A cosmetic is an asset attached to a slot on a player's skeleton (a cape on
// the back, wings between the shoulder blades, a hat on the head). This layer
// is the DATA MODEL and the local render path:
//
//   * CosmeticSlot / Bone  — where an asset may be attached.
//   * CosmeticAsset        — id + slot + bone + geometry/texture ids + attach
//                            transform. Assets are data, not code.
//   * CosmeticRegistry     — the catalog a script or a creator pack registers
//                            into, plus a small built-in set.
//   * CosmeticLoadout      — the slots a player has equipped (switchable
//                            presets live in CosmeticWardrobe).
//   * CosmeticOwnership    — who may wear what. Fail closed: nothing is
//                            wearable until it is registered AND owned.
//
// Like the HUD and the PvP overlay, this is a CLIENT-SIDE service, not a new
// strict Berry:: module: it draws through Berry::Render and owns no engine
// pointers. The roadmap's rule holds — cosmetics render on YOUR screen unless
// a sync service tells other copies of the client what you are wearing, so the
// sync channel (Phase 4) is explicitly optional and off by default.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/game.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {

//----------------------------------------------------------------------------
// Attachment points.
//----------------------------------------------------------------------------

/// A slot on the player model. Slots are exclusive: one asset per slot per
/// player, so equipping a second cape replaces the first instead of stacking.
enum class CosmeticSlot : u32 {
  None = 0,
  Head,        // hats, helmets
  Back,        // capes, backpacks
  Shoulders,   // wings, pauldrons
  Body,        // auras, model overlays
  Trail,       // particle trails behind the player
  Effect,      // hit / kill effects
  Emote,       // one-shot animations
  Count,
};

const char* CosmeticSlotName(CosmeticSlot s);
CosmeticSlot CosmeticSlotFromName(const std::string& name);

/// A bone on the player skeleton an asset attaches to. The render seam reports
/// a world transform per bone; the cosmetic renderer composes the asset's
/// attach transform on top of it.
enum class Bone : u32 {
  None = 0,
  Root,        // entity origin
  Head,
  Torso,       // chest / upper body
  LeftArm,
  RightArm,
  LeftLeg,
  RightLeg,
  Back,        // virtual: behind the torso, for capes
  Count,
};

const char* BoneName(Bone b);
Bone BoneFromName(const std::string& name);

/// The default bone for a slot, used when an asset does not name one.
Bone DefaultBoneFor(CosmeticSlot slot);

//----------------------------------------------------------------------------
// Assets.
//----------------------------------------------------------------------------

/// An opaque handle to loaded geometry / texture data. The actual GL upload
/// lives behind the render backend; on the host these stay as ids. 0 = none.
using GeometryId = u64;
using TextureId = u64;

struct BERRY_API CosmeticAsset {
  std::string id;            // stable id, e.g. "pack.cape.dragon"
  CosmeticSlot slot = CosmeticSlot::None;
  Bone bone = Bone::None;    // DefaultBoneFor(slot) when None
  GeometryId geometry = 0;   // loaded mesh handle
  TextureId texture = 0;     // loaded texture handle
  /// Attach transform relative to the bone: translation (blocks), rotation
  /// (degrees, pitch/yaw/roll) and uniform scale.
  Vec3 offset;
  Vec3 rotation;
  f32 scale = 1.0f;
  /// Tint applied to the asset (multiplied). White = unmodified.
  Vec3 tint = Vec3(1, 1, 1);
  /// Optional creator pack this asset came from (for provenance/ownership).
  std::string pack;
  std::string author;
  /// Free-form tags, for loadout presets and filtering.
  std::vector<std::string> tags;

  bool valid() const { return !id.empty() && slot != CosmeticSlot::None; }
  /// The bone to attach to, resolving None to the slot default.
  Bone ResolvedBone() const {
    return bone == Bone::None ? DefaultBoneFor(slot) : bone;
  }
};

//----------------------------------------------------------------------------
// Registry.
//----------------------------------------------------------------------------

/// Why a registration or equip was refused. Fail closed: every failure leaves
/// the registry / loadout unchanged.
enum class CosmeticError : u32 {
  Ok = 0,
  EmptyId,
  DuplicateId,
  UnknownAsset,
  UnknownSlot,
  NotOwned,
  BadTransform,     // NaN / non-finite offset, rotation or scale
  BadScale,         // non-positive scale
  SlotOccupied,     // reserved for strict one-per-slot registries
  TooManyAssets,
};

const char* CosmeticErrorName(CosmeticError e);

/// The catalog of known assets. Thread-safe.
class BERRY_API CosmeticRegistry {
 public:
  /// Cap on registered assets, so a creator pack cannot exhaust memory.
  static constexpr size_t kMaxAssets = 4096;

  static CosmeticRegistry& Instance();

  /// Registers an asset. Rejects an empty id, a duplicate id, an unknown slot,
  /// a non-finite transform, a non-positive scale, and a full registry.
  bool Register(const CosmeticAsset& asset, CosmeticError* out = nullptr);

  bool Contains(const std::string& id) const;
  bool Find(const std::string& id, CosmeticAsset* out) const;

  /// Every asset for a slot, in registration order.
  std::vector<CosmeticAsset> ForSlot(CosmeticSlot slot) const;

  /// Every registered asset.
  std::vector<CosmeticAsset> All() const;

  size_t Count() const;
  void Clear();

  /// Registers the built-in assets (a cape, wings, a hat and an aura) so a
  /// fresh client has something to show. Idempotent: a second call is a no-op.
  /// Returns how many were added.
  size_t RegisterBuiltins();

 private:
  CosmeticRegistry() = default;
  mutable std::mutex mutex_;
  std::vector<CosmeticAsset> assets_;   // insertion order preserved
  std::unordered_map<std::string, size_t> index_;
};

/// Validates an asset's numeric fields. Exposed so a script binding and the
/// registry share one rule.
BERRY_API bool ValidCosmeticTransform(const CosmeticAsset& a, CosmeticError* out);

//----------------------------------------------------------------------------
// Loadout + wardrobe.
//----------------------------------------------------------------------------

/// What one player has equipped: at most one asset per slot. A slot maps to an
/// asset id; an empty id means "nothing in that slot".
class BERRY_API CosmeticLoadout {
 public:
  /// Equips `assetId` in its own slot. Fails closed when the asset is unknown,
  /// the slot is None, or the registry does not hold it. Returns the slot used
  /// via `outSlot` when non-null.
  bool Equip(const std::string& assetId, CosmeticError* out = nullptr);
  /// Equips an already-validated asset.
  bool Equip(const CosmeticAsset& asset, CosmeticError* out = nullptr);

  /// Removes whatever is in `slot`. Returns false when the slot was empty.
  bool Unequip(CosmeticSlot slot);

  /// The asset id in a slot, or an empty string.
  std::string Equipped(CosmeticSlot slot) const;
  /// True when `assetId` is equipped in any slot.
  bool IsEquipped(const std::string& assetId) const;

  /// The equipped assets, resolved against the registry. Unknown ids (a pack
  /// that was uninstalled) are skipped rather than returned half-formed.
  std::vector<CosmeticAsset> Resolved() const;

  /// The raw slot -> id map, for serialization and the sync channel.
  const std::map<CosmeticSlot, std::string>& slots() const { return slots_; }
  size_t Count() const { return slots_.size(); }
  bool empty() const { return slots_.empty(); }
  void Clear() { slots_.clear(); }

  /// Canonical text form (slots sorted by kind), for hashing and wire framing.
  std::string Canonical() const;

  /// Parses the canonical form back. Fail closed: any malformed line leaves
  /// the loadout untouched and returns false.
  bool FromCanonical(const std::string& text);

 private:
  std::map<CosmeticSlot, std::string> slots_;
};

/// Switchable presets: a named set of loadouts a player can flip between.
class BERRY_API CosmeticWardrobe {
 public:
  static constexpr size_t kMaxPresets = 64;

  /// Saves `loadout` under `name`, replacing an existing preset of that name.
  bool Save(const std::string& name, const CosmeticLoadout& loadout);
  bool Load(const std::string& name, CosmeticLoadout* out) const;
  bool Remove(const std::string& name);
  bool Has(const std::string& name) const;
  std::vector<std::string> Names() const;
  size_t Count() const;
  void Clear();

 private:
  std::map<std::string, CosmeticLoadout> presets_;
};

//----------------------------------------------------------------------------
// Skeleton + render seams.
//
// Cosmetics need two things the core must not assume: a world transform per
// bone (the player-actor render hook) and a way to submit geometry attached to
// it. Both are seams, so the host harness can exercise the whole path headless
// and the device supplies the real GLES path.
//----------------------------------------------------------------------------

/// A bone's world transform, as reported by the skeleton seam.
struct BERRY_API CosmeticBoneTransform {
  Vec3 position;   // world position of the bone origin
  Vec3 rotation;   // degrees: pitch, yaw, roll
  f32 scale = 1.0f;
  bool valid = false;
};

/// Reports bone transforms for an entity. On device this is filled from the
/// actor's rendered skeleton; the host shim provides a synthetic rig.
class BERRY_API CosmeticSkeletonSource {
 public:
  virtual ~CosmeticSkeletonSource() = default;
  /// Fills the transform for `bone` on `entity`. Returns false (and leaves
  /// `out` invalid) when the skeleton or entity is not resolvable.
  virtual bool BoneTransform(EntityId entity, Bone bone,
                             CosmeticBoneTransform* out) const = 0;
};

/// One geometry submission: a resolved asset placed at a world transform.
struct BERRY_API CosmeticDrawItem {
  std::string assetId;
  CosmeticSlot slot = CosmeticSlot::None;
  Bone bone = Bone::None;
  GeometryId geometry = 0;
  TextureId texture = 0;
  Vec3 position;             // world attach position
  Vec3 rotation;             // degrees
  f32 scale = 1.0f;
  Vec3 tint = Vec3(1, 1, 1);
  EntityId entity;           // whose cosmetic this is
};

/// Receives resolved cosmetics for drawing. The device implementation binds
/// the geometry/texture ids to GLES programs; the host captures them.
class BERRY_API CosmeticRenderSeam {
 public:
  virtual ~CosmeticRenderSeam() = default;
  virtual void SubmitGeometry(const CosmeticDrawItem& item) = 0;
};

/// Composes an asset's attach transform onto a bone transform. Exposed so the
/// math is testable without a seam.
BERRY_API CosmeticDrawItem ComposeAttach(const CosmeticAsset& asset,
                                         const CosmeticBoneTransform& bone,
                                         EntityId entity);

//----------------------------------------------------------------------------
// CosmeticService — the local render path.
//----------------------------------------------------------------------------

struct BERRY_API CosmeticStats {
  u64 frames = 0;
  u64 submitted = 0;      // geometry submissions
  u64 skippedNoBone = 0;  // assets whose bone could not be resolved
  u64 skippedNotOwned = 0;
};

/// Owns the local player's loadout and wardrobe and renders it through the
/// seams. Local-only by default: nothing is drawn for a remote player unless a
/// verified loadout is supplied (the sync channel does that in Phase 4).
///
/// Thread-safe: script bindings read and mutate the loadout from the script
/// worker while the render thread resolves and submits it. The critical
/// sections are short map operations, and submission happens outside the lock.
class BERRY_API CosmeticService {
 public:
  CosmeticService() = default;

  void SetSkeletonSource(CosmeticSkeletonSource* s) { skeleton_ = s; }
  void SetRenderSeam(CosmeticRenderSeam* s) { seam_ = s; }
  const CosmeticSkeletonSource* skeletonSource() const { return skeleton_; }
  const CosmeticRenderSeam* renderSeam() const { return seam_; }

  //--- local loadout --------------------------------------------------------
  /// A snapshot of the local loadout. Returned by value so a caller never
  /// holds a reference into state the render thread may mutate.
  CosmeticLoadout localLoadout() const;

  /// Equips an asset on the local player. Refuses an unknown asset.
  bool Equip(const std::string& assetId, CosmeticError* out = nullptr);
  bool Unequip(CosmeticSlot slot);
  /// The asset id equipped in `slot` on the local player, or "".
  std::string Equipped(CosmeticSlot slot) const;

  /// Saves the current local loadout as preset `name`.
  bool SavePreset(const std::string& name);
  /// Replaces the local loadout with preset `name`. False when it is unknown.
  bool ApplyPreset(const std::string& name);
  bool HasPreset(const std::string& name) const;
  std::vector<std::string> PresetNames() const;

  //--- remote loadouts (sync) ----------------------------------------------
  /// Installs a loadout for a remote player, but only for assets that player
  /// provably owns; unowned entries are dropped. This is the gate the sync
  /// channel goes through, so a forged loadout cannot render someone else's
  /// cosmetics.
  size_t SetRemoteLoadout(const std::string& playerId,
                          const CosmeticLoadout& loadout);
  bool ClearRemoteLoadout(const std::string& playerId);
  std::vector<std::string> RemotePlayers() const;

  /// When false, remote loadouts are ignored and only the local player renders.
  /// Atomic: flipped from the script worker, read on the render thread.
  bool localOnly() const { return localOnly_.load(); }
  void SetLocalOnly(bool v) { localOnly_.store(v); }

  //--- resolution + present -------------------------------------------------
  /// Resolves a loadout into draw items for `entity`, using the skeleton seam.
  /// Assets whose bone cannot be resolved are skipped.
  std::vector<CosmeticDrawItem> Resolve(EntityId entity,
                                        const CosmeticLoadout& loadout) const;

  /// Resolves and submits the local player's loadout. Returns how many items
  /// were submitted. Safe to call with no seam (returns 0).
  size_t PresentLocal(EntityId localPlayer);

  /// Submits a remote player's verified loadout (no-op when localOnly).
  size_t PresentRemote(const std::string& playerId, EntityId entity);

  CosmeticStats stats() const;

 private:
  /// Resolves `loadout` without taking the lock. `skel` must be non-null.
  std::vector<CosmeticDrawItem> ResolveLocked(
      EntityId entity, const CosmeticLoadout& loadout,
      const CosmeticSkeletonSource* skel, u64* skippedNoBone) const;

  CosmeticSkeletonSource* skeleton_ = nullptr;
  CosmeticRenderSeam* seam_ = nullptr;

  mutable std::mutex mutex_;
  CosmeticLoadout local_;
  CosmeticWardrobe wardrobe_;
  std::map<std::string, CosmeticLoadout> remote_;
  std::atomic<bool> localOnly_{true};
  mutable CosmeticStats stats_;
};

//----------------------------------------------------------------------------
// Ownership.
//----------------------------------------------------------------------------

/// Tracks which player owns which asset. Local-only cosmetics do not need this
/// (you always "own" what you equip on your own screen), but the sync channel
/// does: a peer's loadout is applied only for assets that peer provably owns.
/// Fail closed — an unknown (player, asset) pair is not owned.
class BERRY_API CosmeticOwnership {
 public:
  static CosmeticOwnership& Instance();

  /// Records that `playerId` owns `assetId`. Returns false for an empty player
  /// or asset id.
  bool Grant(const std::string& playerId, const std::string& assetId);
  /// Records a set of assets at once.
  size_t GrantAll(const std::string& playerId,
                  const std::vector<std::string>& assetIds);
  bool Revoke(const std::string& playerId, const std::string& assetId);
  void RevokePlayer(const std::string& playerId);

  bool Owns(const std::string& playerId, const std::string& assetId) const;
  size_t OwnedCount(const std::string& playerId) const;

  void Clear();

 private:
  CosmeticOwnership() = default;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::vector<std::string>> owned_;
};

}  // namespace Berry
