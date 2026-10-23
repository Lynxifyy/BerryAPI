// Client-side animator. See include/berry/animator.h.
//
// Pure data: no clock, no globals, no game writes. Sampling is a binary search
// over the clip's sorted keys, so the result is deterministic and a replay
// reproduces it exactly.
#include "berry/animator.h"

#include <algorithm>
#include <cmath>

#include "../script/json_min.h"
#include "../script/json_write.h"

namespace Berry {

namespace {

using detail::json::Value;
using detail::jsonw::Writer;

/// The `version` Save() writes and Load() requires. Bump it when a field's
/// meaning changes, so an old build refuses a document it would misread.
constexpr i64 kFormatVersion = 1;

/// A bone's name for serialisation, e.g. "left_arm". BoneName is the same
/// spelling the cosmetic pack format uses, so a clip round-trips through the
/// same vocabulary.
bool BoneFromJson(const Value& v, Bone* out) {
  if (!v.IsString()) return false;
  const Bone b = BoneFromName(v.str);
  if (b == Bone::None || b == Bone::Count) return false;
  *out = b;
  return true;
}

bool Vec3FromJson(const Value& v, Vec3* out) {
  if (!v.IsArray() || v.array.size() != 3) return false;
  for (const Value& e : v.array) {
    if (!e.IsNumber()) return false;
  }
  *out = Vec3((f32)v.array[0].number, (f32)v.array[1].number,
              (f32)v.array[2].number);
  return true;
}

void WriteVec3(Writer* w, const Vec3& v) {
  w->BeginArray();
  w->Number((f64)v.x);
  w->Number((f64)v.y);
  w->Number((f64)v.z);
  w->EndArray();
}

}  // namespace

//----------------------------------------------------------------------------
// Authoring
//----------------------------------------------------------------------------

bool Animator::AddClip(const std::string& name, f32 duration, bool looping) {
  if (name.empty() || name.size() > kMaxNameLen) return false;
  if (!std::isfinite(duration) || duration <= 0.0f) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  if (FindLocked(name)) return false;
  if (clips_.size() >= kMaxClips) return false;
  AnimClip c;
  c.name = name;
  c.duration = duration;
  c.looping = looping;
  clips_.push_back(std::move(c));
  return true;
}

bool Animator::AddKey(const std::string& clip, Bone bone, f32 time,
                      const Vec3& position, const Vec3& rotation, f32 scale) {
  if (bone == Bone::None || (u32)bone >= (u32)Bone::Count) return false;
  if (!std::isfinite(time) || time < 0.0f) return false;
  if (!std::isfinite(scale) || scale <= 0.0f) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  AnimClip* c = FindLocked(clip);
  if (!c) return false;
  if (time > c->duration) return false;
  if (c->keys.size() >= kMaxKeysPerClip) return false;
  AnimKey k;
  k.bone = bone;
  k.time = time;
  k.position = position;
  k.rotation = rotation;
  k.scale = scale;
  // Keep sorted by (bone, time): sampling scans one bone's run, and a stable
  // order means the same clip always samples the same way.
  auto it = std::lower_bound(
      c->keys.begin(), c->keys.end(), k,
      [](const AnimKey& a, const AnimKey& b) {
        if (a.bone != b.bone) return (u32)a.bone < (u32)b.bone;
        return a.time < b.time;
      });
  c->keys.insert(it, k);
  return true;
}

bool Animator::RemoveClip(const std::string& name) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto it = clips_.begin(); it != clips_.end(); ++it) {
    if (it->name == name) {
      clips_.erase(it);
      if (current_ == name) {
        current_.clear();
        time_ = 0.0f;
        playing_ = false;
      }
      return true;
    }
  }
  return false;
}

bool Animator::ClearClip(const std::string& name) {
  std::lock_guard<std::mutex> lk(mutex_);
  AnimClip* c = FindLocked(name);
  if (!c) return false;
  c->keys.clear();
  return true;
}

size_t Animator::clipCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return clips_.size();
}

bool Animator::hasClip(const std::string& name) const {
  std::lock_guard<std::mutex> lk(mutex_);
  return FindLocked(name) != nullptr;
}

size_t Animator::keyCount(const std::string& name) const {
  std::lock_guard<std::mutex> lk(mutex_);
  const AnimClip* c = FindLocked(name);
  return c ? c->keys.size() : 0;
}

//----------------------------------------------------------------------------
// Playback
//----------------------------------------------------------------------------

bool Animator::Play(const std::string& name) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!FindLocked(name)) return false;
  current_ = name;
  time_ = 0.0f;
  playing_ = true;
  return true;
}

void Animator::Stop() {
  std::lock_guard<std::mutex> lk(mutex_);
  playing_ = false;
}

void Animator::Reset() {
  std::lock_guard<std::mutex> lk(mutex_);
  time_ = 0.0f;
  playing_ = false;
}

bool Animator::playing() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return playing_;
}

f32 Animator::time() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return time_;
}

f32 Animator::duration() const {
  std::lock_guard<std::mutex> lk(mutex_);
  const AnimClip* c = FindLocked(current_);
  return c ? c->duration : 0.0f;
}

std::string Animator::currentClip() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return current_;
}

void Animator::Advance(f32 dtSeconds) {
  if (!std::isfinite(dtSeconds) || dtSeconds <= 0.0f) return;
  std::lock_guard<std::mutex> lk(mutex_);
  if (!playing_) return;
  const AnimClip* c = FindLocked(current_);
  if (!c || c->duration <= 0.0f) {
    playing_ = false;
    return;
  }
  time_ += dtSeconds;
  if (c->looping) {
    // Wrap into [0, duration). fmod keeps a huge delta bounded in one step.
    if (time_ >= c->duration) time_ = std::fmod(time_, c->duration);
  } else if (time_ >= c->duration) {
    time_ = c->duration;
    playing_ = false;   // a one-shot stops at its end
  }
}

bool Animator::Seek(f32 seconds) {
  if (!std::isfinite(seconds)) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  const AnimClip* c = FindLocked(current_);
  if (!c || c->duration <= 0.0f) return false;
  f32 t = seconds;
  if (t < 0.0f) t = 0.0f;
  if (c->looping) {
    if (t >= c->duration) t = std::fmod(t, c->duration);
  } else if (t > c->duration) {
    t = c->duration;
  }
  time_ = t;
  return true;
}

//----------------------------------------------------------------------------
// Sampling
//----------------------------------------------------------------------------

bool Animator::BracketLocked(Bone bone, f32 time, const AnimKey** lo,
                             const AnimKey** hi, f32* t) const {
  const AnimClip* c = FindLocked(current_);
  if (!c) return false;
  const std::vector<AnimKey>& keys = c->keys;
  const AnimKey* base = keys.data();
  // The keys for one bone are contiguous and sorted by time. Find the run.
  const size_t first = (size_t)(std::lower_bound(
      keys.begin(), keys.end(), bone,
      [](const AnimKey& a, Bone b) { return (u32)a.bone < (u32)b; }) - keys.begin());
  const size_t last = (size_t)(std::upper_bound(
      keys.begin(), keys.end(), bone,
      [](Bone b, const AnimKey& a) { return (u32)b < (u32)a.bone; }) - keys.begin());
  if (first == last) return false;   // the bone has no keys

  // The first key after `time` bounds the bracket from above.
  const size_t upper = (size_t)(std::upper_bound(
      keys.begin() + first, keys.begin() + last, time,
      [](f32 v, const AnimKey& a) { return v < a.time; }) - keys.begin());
  if (upper == first) {
    // Before the first key: hold it.
    *lo = base + first;
    *hi = base + first;
    *t = 0.0f;
    return true;
  }
  if (upper == last) {
    // At or past the last key: hold it.
    const AnimKey* k = base + (last - 1);
    *lo = k;
    *hi = k;
    *t = 0.0f;
    return true;
  }
  const AnimKey* a = base + (upper - 1);
  const AnimKey* b = base + upper;
  const f32 span = b->time - a->time;
  *lo = a;
  *hi = b;
  *t = span > 0.0f ? (time - a->time) / span : 0.0f;
  return true;
}

Animator::BonePose Animator::SampleLocked(Bone bone, f32 time) const {
  BonePose out;
  if (!std::isfinite(time)) return out;
  const AnimKey* a = nullptr;
  const AnimKey* b = nullptr;
  f32 t = 0.0f;
  if (!BracketLocked(bone, time, &a, &b, &t)) return out;
  out.position = a->position.Lerp(b->position, t);
  out.rotation = a->rotation.Lerp(b->rotation, t);
  out.scale = a->scale + (b->scale - a->scale) * t;
  out.valid = true;
  return out;
}

Animator::BonePose Animator::Sample(Bone bone, f32 time) const {
  std::lock_guard<std::mutex> lk(mutex_);
  return SampleLocked(bone, time);
}

Animator::BonePose Animator::SampleNow(Bone bone) const {
  std::lock_guard<std::mutex> lk(mutex_);
  return SampleLocked(bone, time_);
}

std::vector<std::pair<Bone, Animator::BonePose>> Animator::SamplePose() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<std::pair<Bone, BonePose>> out;
  const AnimClip* c = FindLocked(current_);
  if (!c) return out;
  // One entry per distinct bone, in bone order. There are fewer than 32 bones,
  // so a bitmask of seen bones is enough and keeps the order stable.
  u32 seen = 0;
  for (const AnimKey& k : c->keys) {
    const u32 bit = 1u << (u32)k.bone;
    if (seen & bit) continue;
    seen |= bit;
    BonePose p = SampleLocked(k.bone, time_);
    if (p.valid) out.emplace_back(k.bone, p);
  }
  return out;
}

//----------------------------------------------------------------------------
// Persistence
//----------------------------------------------------------------------------

std::string Animator::Save() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::string out;
  Writer w(&out);
  w.BeginObject();
  w.Key("format");
  w.String("berry-animator");
  w.Key("version");
  w.Int(kFormatVersion);
  w.Key("clips");
  w.BeginArray();
  for (const AnimClip& c : clips_) {
    w.BeginObject();
    w.Key("name");
    w.String(c.name);
    w.Key("duration");
    w.Number((f64)c.duration);
    w.Key("looping");
    w.Bool(c.looping);
    w.Key("keys");
    w.BeginArray();
    for (const AnimKey& k : c.keys) {
      w.BeginObject();
      w.Key("bone");
      w.String(BoneName(k.bone));
      w.Key("time");
      w.Number((f64)k.time);
      w.Key("position");
      WriteVec3(&w, k.position);
      w.Key("rotation");
      WriteVec3(&w, k.rotation);
      w.Key("scale");
      w.Number((f64)k.scale);
      w.EndObject();
    }
    w.EndArray();
    w.EndObject();
  }
  w.EndArray();
  w.EndObject();
  return out;
}

bool Animator::Load(const std::string& json) {
  Value root;
  std::string err;
  if (!detail::json::Parse(json, &root, &err) || !root.IsObject()) return false;
  const Value* format = root.Get("format");
  if (!format || !format->IsString() || format->str != "berry-animator") {
    return false;
  }
  // A future version may mean a field this build would silently misread, so
  // refuse anything that is not exactly the version Save() writes. Compare as
  // a float so "1.5" is refused rather than truncated to 1.
  const Value* version = root.Get("version");
  if (!version || !version->IsNumber() || version->number != (f64)kFormatVersion) {
    return false;
  }
  const Value* clips = root.Get("clips");
  if (!clips || !clips->IsArray()) return false;
  if (clips->array.size() > kMaxClips) return false;

  // Parse into a scratch list first, so a bad document never disturbs the live
  // clips.
  std::vector<AnimClip> parsed;
  for (const Value& cv : clips->array) {
    if (!cv.IsObject()) return false;
    const Value* name = cv.Get("name");
    const Value* dur = cv.Get("duration");
    const Value* looping = cv.Get("looping");
    const Value* keys = cv.Get("keys");
    if (!name || !name->IsString()) return false;
    if (name->str.empty() || name->str.size() > kMaxNameLen) return false;
    if (!dur || !dur->IsNumber()) return false;
    const f32 duration = (f32)dur->number;
    if (!std::isfinite(duration) || duration <= 0.0f) return false;
    if (!looping || !looping->IsBool()) return false;
    if (!keys || !keys->IsArray()) return false;
    if (keys->array.size() > kMaxKeysPerClip) return false;

    AnimClip c;
    c.name = name->str;
    c.duration = duration;
    c.looping = looping->boolean;
    // The authoring path refuses a duplicate name (AddClip); a hand-edited
    // document must not create one either, or FindLocked would silently pick
    // the first and the second clip would be unreachable.
    for (const AnimClip& prev : parsed) {
      if (prev.name == c.name) return false;
    }
    for (const Value& kv : keys->array) {
      if (!kv.IsObject()) return false;
      const Value* bone = kv.Get("bone");
      const Value* time = kv.Get("time");
      const Value* pos = kv.Get("position");
      const Value* rot = kv.Get("rotation");
      const Value* scale = kv.Get("scale");
      if (!bone || !time || !pos || !rot || !scale) return false;
      AnimKey k;
      if (!BoneFromJson(*bone, &k.bone)) return false;
      if (!time->IsNumber()) return false;
      k.time = (f32)time->number;
      if (!std::isfinite(k.time) || k.time < 0.0f || k.time > duration) return false;
      if (!Vec3FromJson(*pos, &k.position)) return false;
      if (!Vec3FromJson(*rot, &k.rotation)) return false;
      if (!scale->IsNumber()) return false;
      k.scale = (f32)scale->number;
      if (!std::isfinite(k.scale) || k.scale <= 0.0f) return false;
      c.keys.push_back(k);
    }
    // Re-establish the sorted order the authoring path guarantees, so a
    // hand-edited document still samples correctly.
    std::stable_sort(c.keys.begin(), c.keys.end(),
                     [](const AnimKey& a, const AnimKey& b) {
                       if (a.bone != b.bone) return (u32)a.bone < (u32)b.bone;
                       return a.time < b.time;
                     });
    parsed.push_back(std::move(c));
  }

  std::lock_guard<std::mutex> lk(mutex_);
  clips_ = std::move(parsed);
  current_.clear();
  time_ = 0.0f;
  playing_ = false;
  return true;
}

void Animator::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  clips_.clear();
  current_.clear();
  time_ = 0.0f;
  playing_ = false;
}

//----------------------------------------------------------------------------
// Internals
//----------------------------------------------------------------------------

const AnimClip* Animator::FindLocked(const std::string& name) const {
  for (const AnimClip& c : clips_) {
    if (c.name == name) return &c;
  }
  return nullptr;
}

AnimClip* Animator::FindLocked(const std::string& name) {
  for (AnimClip& c : clips_) {
    if (c.name == name) return &c;
  }
  return nullptr;
}

//----------------------------------------------------------------------------
// AnimatedSkeletonSource
//----------------------------------------------------------------------------

bool AnimatedSkeletonSource::BoneTransform(EntityId entity, Bone bone,
                                           CosmeticBoneTransform* out) const {
  if (!out) return false;
  // Start from the base seam's transform, or a valid identity when there is no
  // base: the animator can drive a rig on its own.
  CosmeticBoneTransform base;
  if (base_ && base_->BoneTransform(entity, bone, &base)) {
    // keep the base
  } else {
    base = CosmeticBoneTransform{};
    base.valid = true;
  }
  if (!Animates(entity)) {
    *out = base;
    return true;
  }

  // Apply the clip's offset on top of the base transform. A bone the clip does
  // not key is left exactly as the base reported it.
  const Animator::BonePose pose = animator_->SampleNow(bone);
  if (!pose.valid) {
    *out = base;
    return true;
  }
  CosmeticBoneTransform r = base;
  r.position = base.position + pose.position;
  r.rotation = base.rotation + pose.rotation;
  r.scale = base.scale * pose.scale;
  r.valid = true;
  *out = r;
  return true;
}

bool AnimatedSkeletonSource::PoseRevision(EntityId entity, u64* out) const {
  if (Animates(entity) && animator_->playing()) {
    // The pose is moving every tick, so a cached resolve must never be reused.
    // Report no revision, which disables the cosmetic cache for this entity.
    (void)out;
    return false;
  }
  if (base_) return base_->PoseRevision(entity, out);
  return false;
}

}  // namespace Berry
