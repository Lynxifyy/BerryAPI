#pragma once
// Client-side proximity voice chat.
//
// This is a *client feature*, not a new strict Berry:: module (the strict set
// stays Combat/Render/World/Network). It carries the local player's own voice
// to players standing near them on the same LAN, and plays theirs back. It
// never reveals anything the client was not already sent: the audio is the
// speaker's own, and proximity is judged against the listener's own view of
// where the speaker is, so a frame is only played when the speaker is a player
// the listener can already see.
//
// The four pieces the feature needs, each real here:
//
//   * capture — a `VoiceCaptureSeam` the device adapter fills with mic PCM; the
//     core never touches a device.
//   * codec   — `VoiceCodec`, a self-contained IMA ADPCM coder (4 bits/sample,
//     4:1 over 16-bit PCM) with encoder and decoder sharing one step update, so
//     the two can never drift. It is the wire format, so it is pinned by tests.
//   * LAN transport — `LanVoiceTransport`, a concrete UDP multicast transport
//     (one datagram per frame, no reassembly: a frame is small) bound through
//     the `VoiceTransport` seam. A test double implements the same seam.
//   * activity indicator — `speakers()`, the players heard within the hold
//     window with their level and distance, for the HUD/overlay.
//
// Nothing here is authoritative and nothing leaves the subnet: the transport is
// a private IPv4 multicast group, exactly like the cosmetic-sync channel, so it
// is never routed off the LAN. A listener gates every frame by proximity against
// its own world view; a frame from an unknown or out-of-range speaker is
// dropped, never played.
//
// The service owns no clock: `Pump(nowMs, budgetMs)` is the whole input, so the
// ScriptApi frame hook drives it on device and a host test drives it directly.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/math.h"
#include "berry/game.h"

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {

//----------------------------------------------------------------------------
// VoiceCodec — IMA ADPCM over 16-bit mono PCM.
//----------------------------------------------------------------------------

/// A stateless-across-frames ADPCM coder. The `State` carries the predictor and
/// step index from one frame to the next, so a stream decodes continuously; a
/// caller that drops a frame must reset the state (the transport stamps a
/// sequence number so a gap is visible).
class BERRY_API VoiceCodec {
 public:
  /// 20 ms at 16 kHz, mono — the frame the codec and the transport are sized to.
  static constexpr size_t kSamplesPerFrame = 320;
  static constexpr size_t kSampleRate = 16000;
  /// Bounds a decode/encode call, so a hostile length cannot overrun a buffer.
  static constexpr size_t kMaxSamples = 8192;

  struct State {
    i32 predictor = 0;
    i32 index = 0;
  };

  /// Encodes `samples` 16-bit samples into `out` (2 samples per byte). Returns
  /// the byte count, or 0 when the input is empty or exceeds `kMaxSamples` /
  /// `outCap`. `state` is advanced.
  static size_t Encode(const i16* pcm, size_t samples, u8* out, size_t outCap,
                       State* state);
  /// Decodes `bytes` ADPCM bytes into `out` (2 samples per byte). Returns the
  /// sample count, or 0 on a bad length or a small `outCap`. `state` advances.
  static size_t Decode(const u8* adpcm, size_t bytes, i16* out, size_t outCap,
                       State* state);
  /// The encoded size of `samples` samples.
  static size_t EncodedBytes(size_t samples) { return (samples + 1) / 2; }

 private:
  /// The one step update both directions share, so they cannot drift. Returns
  /// the reconstructed sample for `code` and advances the state.
  static i32 Step(i32 code, State* state);
};

//----------------------------------------------------------------------------
// Seams
//----------------------------------------------------------------------------

/// Pulls one frame of microphone PCM. The device adapter implements this; a host
/// test binds a generator. `Capture` fills up to `maxSamples` and returns how
/// many it wrote (0 = nothing to send this pump).
class BERRY_API VoiceCaptureSeam {
 public:
  virtual ~VoiceCaptureSeam() = default;
  virtual size_t Capture(i16* out, size_t maxSamples) = 0;
};

/// Plays one decoded frame. The device adapter mixes; the core hands it a single
/// speaker's PCM with the volume proximity chose.
class BERRY_API VoicePlaybackSeam {
 public:
  virtual ~VoicePlaybackSeam() = default;
  virtual bool Play(const std::string& speaker, const i16* pcm, size_t samples,
                    f32 volume) = 0;
};

/// Moves encoded frames to and from the peers. `LanVoiceTransport` is the
/// concrete implementation; a test binds a double that implements the same seam.
class BERRY_API VoiceTransport {
 public:
  virtual ~VoiceTransport() = default;
  /// Sends one encoded frame. False when the transport is closed or the send
  /// failed; the caller drops the frame (voice is best-effort).
  virtual bool SendFrame(const std::string& speaker, u32 seq,
                         const std::vector<u8>& adpcm) = 0;
  /// Drains incoming frames for up to `budgetMs`, invoking `onFrame` once per
  /// frame. `fromHost`/`fromPort` identify the sender on the wire.
  using FrameCb = std::function<void(const std::string& speaker, u32 seq,
                                     const std::vector<u8>& adpcm,
                                     const std::string& fromHost, u16 fromPort)>;
  virtual void Poll(const FrameCb& onFrame, int budgetMs) = 0;
  /// A short name for diagnostics ("lan", "test", ...).
  virtual std::string name() const = 0;
};

//----------------------------------------------------------------------------
// Service
//----------------------------------------------------------------------------

struct BERRY_API VoiceConfig {
  /// The furthest a speaker is heard, in blocks.
  f32 maxRange = 32.0f;
  /// Within this distance a speaker plays at full volume; volume falls linearly
  /// to zero at `maxRange`.
  f32 fullVolumeRange = 4.0f;
  /// Master volume for playback, [0, 1].
  f32 masterVolume = 1.0f;
  /// The most speakers mixed at once; extra speakers are dropped this pump.
  size_t maxSpeakers = 8;
  /// How long a speaker stays listed as talking after its last frame.
  u64 holdMs = 400;
  /// The largest inbound frame payload accepted, in bytes.
  size_t maxFrameBytes = 512;
  /// The shortest gap between outgoing frames, so a misbehaving capture loop
  /// cannot flood the subnet.
  u64 minSendIntervalMs = 10;
};

/// One speaker the listener is hearing, for the activity indicator.
struct BERRY_API VoiceSpeaker {
  std::string id;
  f32 level = 0.0f;      // 0..1, the RMS of the last decoded frame
  f32 distance = 0.0f;   // blocks from the local player
  u64 lastMs = 0;
};

struct BERRY_API VoiceStats {
  u64 framesCaptured = 0;
  u64 framesSent = 0;
  u64 framesReceived = 0;
  u64 framesDropped = 0;   // malformed, oversized, unknown or out of range
  u64 framesPlayed = 0;
  u64 bytesSent = 0;
  u64 bytesReceived = 0;
};

/// Owns the config, the seams, the outgoing sequence and the per-speaker state.
/// Thread-safe.
class BERRY_API VoiceService {
 public:
  // Bounds: a hostile or fat-fingered config cannot make one pump unbounded.
  static constexpr f32 kMaxRangeLimit = 128.0f;
  static constexpr size_t kMaxSpeakersLimit = 32;
  static constexpr size_t kMaxFrameBytesLimit = 2048;

  VoiceService() = default;

  /// Resolves a speaker id to a world position, from the LISTENER's own world
  /// view. A speaker the listener cannot see is not heard — this is what keeps
  /// the feature local-only. Returns false when the speaker is unknown.
  using ProximityResolver =
      std::function<bool(const std::string& speakerId, Vec3* outPos)>;
  void SetProximityResolver(ProximityResolver r);

  //--- seams ---
  void SetCapture(VoiceCaptureSeam* c) { capture_ = c; }
  void SetPlayback(VoicePlaybackSeam* p) { playback_ = p; }
  void SetTransport(VoiceTransport* t) { transport_ = t; }
  VoiceCaptureSeam* capture() const { return capture_; }
  VoicePlaybackSeam* playback() const { return playback_; }
  VoiceTransport* transport() const { return transport_; }

  //--- config ---
  VoiceConfig config() const;
  /// Replaces the config. Returns false (and changes nothing) when a value is
  /// out of range: a non-finite/negative range, a full-volume range outside
  /// [0, maxRange], a volume outside [0, 1], a speaker cap of 0 or above the
  /// limit, an oversized frame cap, or a bad hold/interval.
  bool SetConfig(const VoiceConfig& cfg);

  //--- session ---
  /// Starts a session under `selfId`. Returns false when already running, when
  /// `selfId` is malformed, or when no transport is bound (nothing to talk
  /// over). Capture and playback seams may be bound later.
  bool Start(const std::string& selfId);
  void Stop();
  bool running() const;
  std::string selfId() const;

  /// Push-to-talk (or open-mic) state: only a talking, unmuted session sends.
  void SetTalking(bool talking);
  bool talking() const;
  void SetMuted(bool muted);
  bool muted() const;

  /// The local player's position, for the proximity of what the listener hears.
  /// Set from the world seam by the frame hook.
  void SetPosition(const Vec3& pos);
  Vec3 position() const;

  /// One voice cycle: capture -> encode -> send while talking, then poll ->
  /// decode -> proximity-gate -> play. Runs the receive half even when not
  /// talking, so the listener still hears others. A no-op when not running.
  void Pump(u64 nowMs, int pollBudgetMs = 0);

  /// The speakers heard within the hold window, loudest first, for the activity
  /// indicator. Bounded by `maxSpeakers`.
  std::vector<VoiceSpeaker> speakers() const;

  VoiceStats stats() const;
  /// Forgets the speaker state and the counters, keeping the config and seams.
  void Reset();

  /// A session id is bounded, printable and non-empty; anything else is refused
  /// so a peer id cannot carry a control character or a delimiter.
  static bool ValidId(const std::string& id);

 private:
  /// The volume for a speaker at `distance`, from the config's falloff.
  f32 VolumeForDistance(f32 distance) const;
  /// Drops speakers whose last frame is older than `holdMs` (and their decode
  /// state), so the activity list ages out and the per-speaker maps stay
  /// bounded. Called at the top of `Pump`; expects `mutex_` held.
  void PruneSpeakers(u64 nowMs);
  /// Decodes, proximity-gates and plays one inbound frame. Returns true when it
  /// was played. Called from the transport's poll callback.
  bool HandleFrame(const std::string& speaker, u32 seq,
                   const std::vector<u8>& adpcm, u64 nowMs);

  mutable std::mutex mutex_;
  VoiceConfig cfg_{};
  VoiceCaptureSeam* capture_ = nullptr;
  VoicePlaybackSeam* playback_ = nullptr;
  VoiceTransport* transport_ = nullptr;
  ProximityResolver proximity_;

  bool running_ = false;
  std::string selfId_;
  bool talking_ = false;
  bool muted_ = false;
  Vec3 position_{};
  u32 seq_ = 0;
  u64 lastSendMs_ = 0;

  VoiceCodec::State encodeState_{};
  /// Per-speaker decoder state, keyed by peer id.
  std::unordered_map<std::string, VoiceCodec::State> decodeState_;
  std::unordered_map<std::string, VoiceSpeaker> speakers_;
  VoiceStats stats_{};
};

}  // namespace Berry
