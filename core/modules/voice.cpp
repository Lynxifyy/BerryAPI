#include "berry/voice.h"

#include <algorithm>
#include <cmath>

namespace Berry {

namespace {

// The IMA ADPCM step table and index-adjust table (the canonical ones). Both
// directions use them through VoiceCodec::Step, so encoder and decoder agree.
const i32 kStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
    230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
    29794, 32767};
const i32 kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                             -1, -1, -1, -1, 2, 4, 6, 8};

i32 ClampIndex(i32 i) {
  if (i < 0) return 0;
  if (i > 88) return 88;
  return i;
}

}  // namespace

i32 VoiceCodec::Step(i32 code, State* state) {
  const i32 step = kStepTable[ClampIndex(state->index)];
  i32 diff = step >> 3;
  if (code & 4) diff += step;
  if (code & 2) diff += step >> 1;
  if (code & 1) diff += step >> 2;
  if (code & 8) {
    state->predictor -= diff;
  } else {
    state->predictor += diff;
  }
  if (state->predictor > 32767) state->predictor = 32767;
  if (state->predictor < -32768) state->predictor = -32768;
  state->index = ClampIndex(state->index + kIndexTable[code & 0xf]);
  return state->predictor;
}

size_t VoiceCodec::Encode(const i16* pcm, size_t samples, u8* out, size_t outCap,
                          State* state) {
  if (!pcm || !out || !state || samples == 0) return 0;
  if (samples > kMaxSamples) return 0;
  const size_t need = EncodedBytes(samples);
  if (need > outCap) return 0;

  size_t produced = 0;
  for (size_t i = 0; i < samples; i += 2) {
    u8 byte = 0;
    // Two nibbles per byte, low nibble first (the decoder's order).
    for (int half = 0; half < 2; ++half) {
      const size_t idx = i + (size_t)half;
      if (idx >= samples) break;
      const i32 step = kStepTable[ClampIndex(state->index)];
      const i32 target = pcm[idx];
      i32 diff = target - state->predictor;
      i32 code = 0;
      if (diff < 0) {
        code = 8;
        diff = -diff;
      }
      // The nibble is the number of step/2, step/4, step/8 units in the
      // difference, the same reconstruction Step applies.
      i32 vpdiff = step >> 3;
      if (diff >= step) {
        code |= 4;
        diff -= step;
        vpdiff += step;
      }
      if (diff >= step >> 1) {
        code |= 2;
        diff -= step >> 1;
        vpdiff += step >> 1;
      }
      if (diff >= step >> 2) {
        code |= 1;
        diff -= step >> 2;
        vpdiff += step >> 2;
      }
      byte |= (u8)(code << (4 * half));
      // Advance the state with the SAME reconstruction the decoder will do, so
      // the two stay locked: predictor moves by the code's own vpdiff.
      if (code & 8) {
        state->predictor -= vpdiff;
      } else {
        state->predictor += vpdiff;
      }
      if (state->predictor > 32767) state->predictor = 32767;
      if (state->predictor < -32768) state->predictor = -32768;
      state->index = ClampIndex(state->index + kIndexTable[code & 0xf]);
    }
    out[produced++] = byte;
  }
  return produced;
}

size_t VoiceCodec::Decode(const u8* adpcm, size_t bytes, i16* out, size_t outCap,
                          State* state) {
  if (!adpcm || !out || !state || bytes == 0) return 0;
  const size_t samples = bytes * 2;
  if (samples > outCap) return 0;
  if (samples > kMaxSamples) return 0;

  size_t produced = 0;
  for (size_t i = 0; i < bytes; ++i) {
    const u8 byte = adpcm[i];
    out[produced++] = (i16)Step(byte & 0xf, state);
    out[produced++] = (i16)Step((byte >> 4) & 0xf, state);
  }
  return produced;
}

//----------------------------------------------------------------------------
// Service
//----------------------------------------------------------------------------

void VoiceService::SetProximityResolver(ProximityResolver r) {
  std::lock_guard<std::mutex> lk(mutex_);
  proximity_ = std::move(r);
}

bool VoiceService::ValidId(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  for (unsigned char c : id) {
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

VoiceConfig VoiceService::config() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return cfg_;
}

bool VoiceService::SetConfig(const VoiceConfig& cfg) {
  if (!std::isfinite(cfg.maxRange) || cfg.maxRange <= 0.0f) return false;
  if (cfg.maxRange > kMaxRangeLimit) return false;
  if (!std::isfinite(cfg.fullVolumeRange) || cfg.fullVolumeRange < 0.0f) return false;
  if (cfg.fullVolumeRange > cfg.maxRange) return false;
  if (!std::isfinite(cfg.masterVolume) || cfg.masterVolume < 0.0f ||
      cfg.masterVolume > 1.0f) {
    return false;
  }
  if (cfg.maxSpeakers == 0 || cfg.maxSpeakers > kMaxSpeakersLimit) return false;
  if (cfg.maxFrameBytes == 0 || cfg.maxFrameBytes > kMaxFrameBytesLimit) return false;
  if (cfg.holdMs == 0) return false;
  if (cfg.minSendIntervalMs == 0) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  cfg_ = cfg;
  return true;
}

bool VoiceService::Start(const std::string& selfId) {
  if (!ValidId(selfId)) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  if (running_) return false;
  if (!transport_) return false;   // nothing to talk over
  running_ = true;
  selfId_ = selfId;
  talking_ = false;
  muted_ = false;
  seq_ = 0;
  lastSendMs_ = 0;
  encodeState_ = VoiceCodec::State{};
  decodeState_.clear();
  speakers_.clear();
  return true;
}

void VoiceService::Stop() {
  std::lock_guard<std::mutex> lk(mutex_);
  running_ = false;
  talking_ = false;
  selfId_.clear();
  speakers_.clear();
  decodeState_.clear();
}

bool VoiceService::running() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return running_;
}

std::string VoiceService::selfId() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return selfId_;
}

void VoiceService::SetTalking(bool talking) {
  std::lock_guard<std::mutex> lk(mutex_);
  talking_ = talking;
}

bool VoiceService::talking() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return talking_;
}

void VoiceService::SetMuted(bool muted) {
  std::lock_guard<std::mutex> lk(mutex_);
  muted_ = muted;
}

bool VoiceService::muted() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return muted_;
}

void VoiceService::SetPosition(const Vec3& pos) {
  std::lock_guard<std::mutex> lk(mutex_);
  position_ = pos;
}

Vec3 VoiceService::position() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return position_;
}

f32 VoiceService::VolumeForDistance(f32 distance) const {
  if (distance >= cfg_.maxRange) return 0.0f;
  if (distance <= cfg_.fullVolumeRange) return cfg_.masterVolume;
  const f32 span = cfg_.maxRange - cfg_.fullVolumeRange;
  if (span <= 0.0f) return cfg_.masterVolume;
  const f32 t = 1.0f - (distance - cfg_.fullVolumeRange) / span;
  return cfg_.masterVolume * std::max(0.0f, std::min(1.0f, t));
}

void VoiceService::Pump(u64 nowMs, int pollBudgetMs) {
  VoiceCaptureSeam* capture = nullptr;
  VoiceTransport* transport = nullptr;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!running_) return;
    capture = capture_;
    transport = transport_;
    // Age out speakers that stopped talking, so the activity list and the
    // per-speaker decode state do not grow without bound.
    PruneSpeakers(nowMs);
  }
  if (!transport) return;

  //--- capture half: only a talking, unmuted session sends ---
  bool talking = false;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    talking = talking_ && !muted_;
  }
  if (talking && capture) {
    i16 pcm[VoiceCodec::kSamplesPerFrame];
    const size_t got = capture->Capture(pcm, VoiceCodec::kSamplesPerFrame);
    if (got > 0 && got <= VoiceCodec::kSamplesPerFrame) {
      bool send = false;
      u32 seq = 0;
      std::vector<u8> adpcm(VoiceCodec::EncodedBytes(got));
      {
        std::lock_guard<std::mutex> lk(mutex_);
        // Rate-limit the outgoing stream so a fast capture loop cannot flood.
        if (nowMs - lastSendMs_ >= cfg_.minSendIntervalMs) {
          const size_t n = VoiceCodec::Encode(pcm, got, adpcm.data(),
                                              adpcm.size(), &encodeState_);
          if (n > 0) {
            adpcm.resize(n);
            seq = ++seq_;
            lastSendMs_ = nowMs;
            send = true;
            stats_.framesCaptured++;
          }
        }
      }
      if (send) {
        std::string id;
        {
          std::lock_guard<std::mutex> lk(mutex_);
          id = selfId_;
        }
        if (transport->SendFrame(id, seq, adpcm)) {
          std::lock_guard<std::mutex> lk(mutex_);
          stats_.framesSent++;
          stats_.bytesSent += adpcm.size();
        }
      }
    }
  }

  //--- receive half: always, so a listener hears others while silent ---
  transport->Poll(
      [this, nowMs](const std::string& speaker, u32 seq,
                    const std::vector<u8>& adpcm, const std::string&, u16) {
        HandleFrame(speaker, seq, adpcm, nowMs);
      },
      pollBudgetMs);
}

void VoiceService::PruneSpeakers(u64 nowMs) {
  for (auto it = speakers_.begin(); it != speakers_.end();) {
    const VoiceSpeaker& sp = it->second;
    // A frame from the future (a clock step backwards) is kept rather than
    // aged out, so a wall-clock jump never wipes the activity list.
    if (nowMs >= sp.lastMs && nowMs - sp.lastMs > cfg_.holdMs) {
      decodeState_.erase(it->first);
      it = speakers_.erase(it);
    } else {
      ++it;
    }
  }
}

bool VoiceService::HandleFrame(const std::string& speaker, u32,
                               const std::vector<u8>& adpcm, u64 nowMs) {
  VoicePlaybackSeam* playback = nullptr;
  ProximityResolver proximity;
  Vec3 self;
  VoiceConfig cfg;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!running_) return false;
    // Ignore our own looped-back frames and any malformed or oversized frame.
    if (speaker == selfId_ || !ValidId(speaker)) {
      stats_.framesDropped++;
      return false;
    }
    if (adpcm.empty() || adpcm.size() > cfg_.maxFrameBytes) {
      stats_.framesDropped++;
      return false;
    }
    // Bound the tracked set: a flood of distinct speaker ids must not grow the
    // per-speaker decode state without limit. A known speaker is always kept.
    if (speakers_.find(speaker) == speakers_.end() &&
        speakers_.size() >= kMaxSpeakersLimit) {
      stats_.framesDropped++;
      return false;
    }
    playback = playback_;
    proximity = proximity_;
    self = position_;
    cfg = cfg_;
    stats_.framesReceived++;
    stats_.bytesReceived += adpcm.size();
  }

  // Proximity is judged against the LISTENER's own view of the speaker: a
  // speaker the listener cannot see is not heard. This is what keeps the
  // feature local-only.
  if (!proximity) {
    std::lock_guard<std::mutex> lk(mutex_);
    stats_.framesDropped++;
    return false;
  }
  Vec3 speakerPos;
  if (!proximity(speaker, &speakerPos)) {
    std::lock_guard<std::mutex> lk(mutex_);
    stats_.framesDropped++;
    return false;
  }
  const f32 distance = (speakerPos - self).Length();
  const f32 volume = VolumeForDistance(distance);
  if (volume <= 0.0f) {
    std::lock_guard<std::mutex> lk(mutex_);
    stats_.framesDropped++;
    return false;
  }

  // Decode with this speaker's own state, so a stream from one speaker is
  // continuous and speakers do not corrupt each other's predictors.
  std::vector<i16> pcm(adpcm.size() * 2);
  size_t samples = 0;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    samples = VoiceCodec::Decode(adpcm.data(), adpcm.size(), pcm.data(),
                                 pcm.size(), &decodeState_[speaker]);
    if (samples == 0) {
      stats_.framesDropped++;
      return false;
    }
    pcm.resize(samples);
    // RMS level for the activity indicator, computed before the play so it is
    // reported even when no playback seam is bound.
    f64 sum = 0.0;
    for (i16 s : pcm) {
      const f64 v = (f64)s / 32768.0;
      sum += v * v;
    }
    const f32 level = (f32)std::sqrt(sum / (f64)samples);
    VoiceSpeaker& sp = speakers_[speaker];
    sp.id = speaker;
    sp.level = level;
    sp.distance = distance;
    sp.lastMs = nowMs;
    stats_.framesPlayed++;
  }

  if (playback) playback->Play(speaker, pcm.data(), pcm.size(), volume);
  return true;
}

std::vector<VoiceSpeaker> VoiceService::speakers() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<VoiceSpeaker> out;
  for (const auto& [id, sp] : speakers_) out.push_back(sp);
  std::sort(out.begin(), out.end(),
            [](const VoiceSpeaker& a, const VoiceSpeaker& b) {
              if (a.level != b.level) return a.level > b.level;
              return a.id < b.id;
            });
  if (out.size() > cfg_.maxSpeakers) out.resize(cfg_.maxSpeakers);
  return out;
}

VoiceStats VoiceService::stats() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return stats_;
}

void VoiceService::Reset() {
  std::lock_guard<std::mutex> lk(mutex_);
  speakers_.clear();
  decodeState_.clear();
  stats_ = VoiceStats{};
}

}  // namespace Berry
