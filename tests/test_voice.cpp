// Host tests for the client-side proximity voice chat: the ADPCM codec, the
// service (capture/send, poll/decode/proximity/play), and the LAN transport.
//
// Covers:
//   * the codec: a round trip reconstructs a waveform within quantization error,
//     the state carries across frames, and bad lengths fail closed;
//   * the service: config validation, Start gating (a transport is required), the
//     push-to-talk and mute gates, the send rate limit, the proximity gate (an
//     unknown or out-of-range speaker is dropped, never played), the speaker
//     activity list and its ordering, the counters, and Reset;
//   * the LAN transport: a frame survives a multicast loopback send, a malformed
//     datagram is dropped, and BuildDatagram rejects a bad id or an oversized
//     payload.
#include "berry/voice.h"
#include "berry/voice_transport.h"
#include "berry/net.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

// A capture seam that emits a fixed tone, so the encode path has real PCM.
struct ToneCapture : VoiceCaptureSeam {
  size_t total = 0;
  size_t frameSamples = VoiceCodec::kSamplesPerFrame;
  double phase = 0.0;
  size_t Capture(i16* out, size_t maxSamples) override {
    const size_t n = std::min(maxSamples, frameSamples);
    for (size_t i = 0; i < n; ++i) {
      out[i] = (i16)(std::sin(phase) * 12000.0);
      phase += 0.15;
    }
    total += n;
    return n;
  }
};

// A playback seam that records what was played.
struct RecordingPlayback : VoicePlaybackSeam {
  std::vector<std::string> speakers;
  std::vector<size_t> sizes;
  std::vector<f32> volumes;
  bool accept = true;
  bool Play(const std::string& speaker, const i16*, size_t samples,
            f32 volume) override {
    if (!accept) return false;
    speakers.push_back(speaker);
    sizes.push_back(samples);
    volumes.push_back(volume);
    return true;
  }
};

// A transport double: SendFrame records; Poll replays a queued inbox. This is a
// seam double, not a mock of the code under test — the service's real encode,
// decode, proximity and play paths all run.
struct FakeTransport : VoiceTransport {
  std::vector<std::string> sentSpeakers;
  std::vector<u32> sentSeqs;
  std::vector<size_t> sentBytes;
  bool sendOk = true;

  struct Inbox {
    std::string speaker;
    u32 seq = 0;
    std::vector<u8> adpcm;
  };
  std::vector<Inbox> inbox;

  bool SendFrame(const std::string& speaker, u32 seq,
                 const std::vector<u8>& adpcm) override {
    sentSpeakers.push_back(speaker);
    sentSeqs.push_back(seq);
    sentBytes.push_back(adpcm.size());
    return sendOk;
  }
  void Poll(const FrameCb& onFrame, int) override {
    std::vector<Inbox> pending;
    pending.swap(inbox);
    for (const Inbox& f : pending) {
      onFrame(f.speaker, f.seq, f.adpcm, "127.0.0.1", 1234);
    }
  }
  std::string name() const override { return "test"; }
};

}  // namespace

static void TestCodecRoundTrip() {
  // A ramp encodes and decodes back to roughly itself (ADPCM is lossy).
  i16 pcm[VoiceCodec::kSamplesPerFrame];
  for (size_t i = 0; i < VoiceCodec::kSamplesPerFrame; ++i) {
    pcm[i] = (i16)(std::sin((double)i * 0.1) * 20000.0);
  }
  std::vector<u8> enc(VoiceCodec::EncodedBytes(VoiceCodec::kSamplesPerFrame));
  VoiceCodec::State es, ds;
  const size_t n = VoiceCodec::Encode(pcm, VoiceCodec::kSamplesPerFrame,
                                      enc.data(), enc.size(), &es);
  CHECK(n == enc.size());

  std::vector<i16> dec(VoiceCodec::kSamplesPerFrame);
  const size_t got = VoiceCodec::Decode(enc.data(), n, dec.data(), dec.size(), &ds);
  CHECK(got == VoiceCodec::kSamplesPerFrame);

  // Mean absolute error should be small relative to the signal.
  double err = 0.0;
  for (size_t i = 0; i < got; ++i) err += std::fabs((double)dec[i] - pcm[i]);
  err /= (double)got;
  CHECK(err < 1200.0);

  // Empty and oversize inputs fail closed.
  CHECK(VoiceCodec::Encode(nullptr, 4, enc.data(), enc.size(), &es) == 0);
  CHECK(VoiceCodec::Encode(pcm, 0, enc.data(), enc.size(), &es) == 0);
  CHECK(VoiceCodec::Decode(nullptr, 4, dec.data(), dec.size(), &ds) == 0);
  CHECK(VoiceCodec::Decode(enc.data(), 0, dec.data(), dec.size(), &ds) == 0);
  // A too-small output buffer is refused.
  i16 tiny[2];
  CHECK(VoiceCodec::Decode(enc.data(), 100, tiny, 2, &ds) == 0);
}

static void TestCodecStateContinuity() {
  // Two frames in a row, decoded with a carried state, track the encoder's
  // carried state; a reset between frames would be visible as a larger error.
  i16 a[160], b[160];
  for (size_t i = 0; i < 160; ++i) {
    a[i] = (i16)(1000 + i * 20);
    b[i] = (i16)(1000 + (i + 160) * 20);
  }
  VoiceCodec::State es, ds;
  std::vector<u8> ea(VoiceCodec::EncodedBytes(160)), eb(VoiceCodec::EncodedBytes(160));
  CHECK(VoiceCodec::Encode(a, 160, ea.data(), ea.size(), &es) == ea.size());
  CHECK(VoiceCodec::Encode(b, 160, eb.data(), eb.size(), &es) == eb.size());

  std::vector<i16> da(160), db(160);
  CHECK(VoiceCodec::Decode(ea.data(), ea.size(), da.data(), da.size(), &ds) == 160);
  CHECK(VoiceCodec::Decode(eb.data(), eb.size(), db.data(), db.size(), &ds) == 160);
  // The carried state means the second frame decodes near its input.
  double err = 0.0;
  for (size_t i = 0; i < 160; ++i) err += std::fabs((double)db[i] - b[i]);
  CHECK(err / 160.0 < 400.0);
}

static void TestConfigValidation() {
  VoiceService svc;
  VoiceConfig cfg = svc.config();
  CHECK(svc.SetConfig(cfg));

  cfg = svc.config();
  cfg.maxRange = 0.0f;
  CHECK(!svc.SetConfig(cfg));
  cfg = svc.config();
  cfg.maxRange = -5.0f;
  CHECK(!svc.SetConfig(cfg));
  cfg = svc.config();
  cfg.maxRange = 1000.0f;   // over the limit
  CHECK(!svc.SetConfig(cfg));
  cfg = svc.config();
  cfg.fullVolumeRange = 100.0f;   // > maxRange
  CHECK(!svc.SetConfig(cfg));
  cfg = svc.config();
  cfg.masterVolume = 2.0f;
  CHECK(!svc.SetConfig(cfg));
  cfg = svc.config();
  cfg.maxSpeakers = 0;
  CHECK(!svc.SetConfig(cfg));
  cfg = svc.config();
  cfg.maxFrameBytes = VoiceService::kMaxFrameBytesLimit + 1;
  CHECK(!svc.SetConfig(cfg));
  cfg = svc.config();
  cfg.holdMs = 0;
  CHECK(!svc.SetConfig(cfg));
  cfg = svc.config();
  cfg.minSendIntervalMs = 0;
  CHECK(!svc.SetConfig(cfg));

  // A valid change sticks.
  cfg = svc.config();
  cfg.maxRange = 48.0f;
  cfg.masterVolume = 0.5f;
  CHECK(svc.SetConfig(cfg));
  CHECK(std::fabs(svc.config().maxRange - 48.0f) < 1e-6);
}

static void TestStartGating() {
  VoiceService svc;
  // No transport: nothing to talk over, Start refuses.
  CHECK(!svc.Start("me"));
  CHECK(!svc.running());

  FakeTransport t;
  svc.SetTransport(&t);
  // A malformed id is refused.
  CHECK(!svc.Start(""));
  CHECK(!svc.Start(std::string(200, 'a')));
  CHECK(!svc.Start("bad\nid"));

  CHECK(svc.Start("me"));
  CHECK(svc.running());
  CHECK(svc.selfId() == "me");
  // A second Start while running is refused.
  CHECK(!svc.Start("me2"));

  svc.Stop();
  CHECK(!svc.running());
}

static void TestTalkGateAndSend() {
  VoiceService svc;
  FakeTransport t;
  ToneCapture cap;
  svc.SetTransport(&t);
  svc.SetCapture(&cap);
  CHECK(svc.Start("me"));

  // Not talking: no capture, no send.
  svc.Pump(1000, 0);
  CHECK(t.sentSpeakers.empty());
  CHECK(cap.total == 0);

  // Muted while talking: still nothing.
  svc.SetTalking(true);
  svc.SetMuted(true);
  svc.Pump(1001, 0);
  CHECK(t.sentSpeakers.empty());

  // Talking and unmuted: one frame captured and sent.
  svc.SetMuted(false);
  svc.Pump(1002, 0);
  CHECK(t.sentSpeakers.size() == 1);
  CHECK(t.sentSpeakers[0] == "me");
  CHECK(t.sentBytes[0] == VoiceCodec::EncodedBytes(VoiceCodec::kSamplesPerFrame));
  CHECK(svc.stats().framesSent == 1);

  // The rate limit: a second pump at the same instant sends nothing.
  svc.Pump(1002, 0);
  CHECK(t.sentSpeakers.size() == 1);
  // Past the interval it sends again, with an incremented sequence.
  svc.Pump(1100, 0);
  CHECK(t.sentSpeakers.size() == 2);
  CHECK(t.sentSeqs[1] == t.sentSeqs[0] + 1);
}

static void TestReceiveProximityAndPlay() {
  VoiceService svc;
  FakeTransport t;
  RecordingPlayback play;
  svc.SetTransport(&t);
  svc.SetPlayback(&play);
  svc.SetPosition(Vec3(0, 0, 0));

  // The listener's own view: "near" is 2 blocks away, "far" is 1000 away.
  std::unordered_map<std::string, Vec3> world;
  world["near"] = Vec3(2, 0, 0);
  world["far"] = Vec3(1000, 0, 0);
  svc.SetProximityResolver([&](const std::string& id, Vec3* out) {
    auto it = world.find(id);
    if (it == world.end()) return false;
    *out = it->second;
    return true;
  });

  CHECK(svc.Start("me"));

  // Build one encoded frame to inject.
  i16 pcm[VoiceCodec::kSamplesPerFrame];
  for (size_t i = 0; i < VoiceCodec::kSamplesPerFrame; ++i) {
    pcm[i] = (i16)(std::sin((double)i * 0.2) * 9000.0);
  }
  std::vector<u8> enc(VoiceCodec::EncodedBytes(VoiceCodec::kSamplesPerFrame));
  VoiceCodec::State st;
  CHECK(VoiceCodec::Encode(pcm, VoiceCodec::kSamplesPerFrame, enc.data(),
                           enc.size(), &st) > 0);

  t.inbox.push_back({"near", 1, enc});
  t.inbox.push_back({"far", 2, enc});
  t.inbox.push_back({"ghost", 3, enc});   // unknown to the listener
  t.inbox.push_back({"me", 4, enc});      // our own looped-back frame
  svc.Pump(2000, 0);

  // Only the near, known, non-self speaker is played.
  CHECK(play.speakers.size() == 1);
  CHECK(play.speakers[0] == "near");
  CHECK(play.sizes[0] == VoiceCodec::kSamplesPerFrame);
  CHECK(play.volumes[0] > 0.0f && play.volumes[0] <= 1.0f);
  CHECK(svc.stats().framesPlayed == 1);
  CHECK(svc.stats().framesDropped == 3);

  // The activity list names the speaker heard.
  std::vector<VoiceSpeaker> sp = svc.speakers();
  CHECK(sp.size() == 1);
  CHECK(sp[0].id == "near");
  CHECK(sp[0].level > 0.0f);
  CHECK(std::fabs(sp[0].distance - 2.0f) < 1e-4);

  // Volume falls off: at fullVolumeRange it is master, beyond it is less.
  VoiceConfig cfg = svc.config();
  cfg.fullVolumeRange = 1.0f;
  cfg.maxRange = 32.0f;
  CHECK(svc.SetConfig(cfg));
  t.inbox.push_back({"near", 5, enc});
  svc.Pump(3000, 0);
  CHECK(play.volumes.size() == 2);
  CHECK(play.volumes[1] < play.volumes[0]);
}

static void TestNoProximityResolverDrops() {
  VoiceService svc;
  FakeTransport t;
  RecordingPlayback play;
  svc.SetTransport(&t);
  svc.SetPlayback(&play);
  CHECK(svc.Start("me"));
  // With no resolver the listener cannot place the speaker, so nothing plays.
  std::vector<u8> enc(40, 0x11);
  t.inbox.push_back({"someone", 1, enc});
  svc.Pump(1000, 0);
  CHECK(play.speakers.empty());
  CHECK(svc.stats().framesPlayed == 0);
}

static void TestSpeakersOrderingAndReset() {
  VoiceService svc;
  FakeTransport t;
  svc.SetTransport(&t);
  std::unordered_map<std::string, Vec3> world;
  world["a"] = Vec3(1, 0, 0);
  world["b"] = Vec3(1, 0, 0);
  svc.SetProximityResolver([&](const std::string& id, Vec3* out) {
    auto it = world.find(id);
    if (it == world.end()) return false;
    *out = it->second;
    return true;
  });
  CHECK(svc.Start("me"));

  // A loud frame for "a", a quiet frame for "b": the loudest is listed first.
  i16 loud[VoiceCodec::kSamplesPerFrame], quiet[VoiceCodec::kSamplesPerFrame];
  for (size_t i = 0; i < VoiceCodec::kSamplesPerFrame; ++i) {
    loud[i] = (i16)(std::sin((double)i * 0.3) * 30000.0);
    quiet[i] = (i16)(std::sin((double)i * 0.3) * 500.0);
  }
  std::vector<u8> el(VoiceCodec::EncodedBytes(VoiceCodec::kSamplesPerFrame));
  std::vector<u8> eq(el.size());
  VoiceCodec::State s1, s2;
  CHECK(VoiceCodec::Encode(loud, VoiceCodec::kSamplesPerFrame, el.data(), el.size(), &s1) > 0);
  CHECK(VoiceCodec::Encode(quiet, VoiceCodec::kSamplesPerFrame, eq.data(), eq.size(), &s2) > 0);
  t.inbox.push_back({"a", 1, el});
  t.inbox.push_back({"b", 2, eq});
  svc.Pump(4000, 0);

  std::vector<VoiceSpeaker> sp = svc.speakers();
  CHECK(sp.size() == 2);
  CHECK(sp[0].id == "a");   // loudest first
  CHECK(sp[0].level > sp[1].level);

  svc.Reset();
  CHECK(svc.speakers().empty());
  CHECK(svc.stats().framesPlayed == 0);
}

static void TestSpeakerHoldAgesOut() {
  VoiceService svc;
  FakeTransport t;
  svc.SetTransport(&t);
  svc.SetProximityResolver([](const std::string&, Vec3* out) {
    *out = Vec3(0, 0, 0);
    return true;
  });
  CHECK(svc.Start("me"));

  i16 pcm[VoiceCodec::kSamplesPerFrame];
  for (size_t i = 0; i < VoiceCodec::kSamplesPerFrame; ++i) pcm[i] = 5000;
  std::vector<u8> enc(VoiceCodec::EncodedBytes(VoiceCodec::kSamplesPerFrame));
  VoiceCodec::State st;
  CHECK(VoiceCodec::Encode(pcm, VoiceCodec::kSamplesPerFrame, enc.data(),
                           enc.size(), &st) > 0);

  t.inbox.push_back({"near", 1, enc});
  svc.Pump(1000, 0);
  CHECK(svc.speakers().size() == 1);

  // Within the hold window the speaker stays listed even with no new frame.
  svc.Pump(1000 + svc.config().holdMs, 0);
  CHECK(svc.speakers().size() == 1);

  // Past the hold window the speaker ages out.
  svc.Pump(1000 + svc.config().holdMs + 1, 0);
  CHECK(svc.speakers().empty());

  // A clock step backwards keeps the speaker rather than wiping the list.
  t.inbox.push_back({"near", 2, enc});
  svc.Pump(5000, 0);
  CHECK(svc.speakers().size() == 1);
  svc.Pump(4000, 0);
  CHECK(svc.speakers().size() == 1);
}

static void TestSpeakerCapIsBounded() {
  VoiceService svc;
  FakeTransport t;
  svc.SetTransport(&t);
  svc.SetProximityResolver([](const std::string&, Vec3* out) {
    *out = Vec3(0, 0, 0);
    return true;
  });
  CHECK(svc.Start("me"));

  std::vector<u8> enc(40, 0x33);
  // One more distinct speaker than the tracked cap allows.
  for (size_t i = 0; i < VoiceService::kMaxSpeakersLimit + 8; ++i) {
    t.inbox.push_back({"s" + std::to_string(i), (u32)i, enc});
  }
  svc.Pump(1000, 0);
  // The tracked set never exceeds the cap, so a flood of ids cannot grow it.
  CHECK(svc.speakers().size() <= VoiceService::kMaxSpeakersLimit);
  CHECK(svc.stats().framesDropped > 0);
}

static void TestLanTransportLoopback() {
  LanVoiceTransport a, b;
  VoiceTransportConfig acfg, bcfg;
  acfg.group = "239.66.0.21";
  acfg.port = 47931;
  acfg.iface = "127.0.0.1";
  bcfg = acfg;
  CHECK(a.Open(acfg));
  CHECK(b.Open(bcfg));
  CHECK(a.open() && b.open());

  std::vector<u8> adpcm(80);
  for (size_t i = 0; i < adpcm.size(); ++i) adpcm[i] = (u8)(i * 7);

  CHECK(a.SendFrame("alice", 42, adpcm));

  std::string gotSpeaker;
  std::vector<u8> gotFrame;
  u32 gotSeq = 0;
  for (int i = 0; i < 60 && gotFrame.empty(); ++i) {
    b.Poll([&](const std::string& sp, u32 seq, const std::vector<u8>& f,
               const std::string&, u16) {
      if (gotFrame.empty()) {
        gotSpeaker = sp;
        gotSeq = seq;
        gotFrame = f;
      }
    }, 20);
  }
  CHECK(gotSpeaker == "alice");
  CHECK(gotSeq == 42);
  CHECK(gotFrame == adpcm);
  CHECK(b.stats().framesReceived == 1);

  // A malformed datagram (bad magic) is dropped, not delivered.
  const char junk[32] = {'X', 'X', 'X', 'X', 1, 1, 0, 0, 4, 0, 'a', 'b', 'c', 'd'};
  CHECK(a.socket().SendTo(junk, 14, "127.0.0.1", b.localPort()));
  bool delivered = false;
  for (int i = 0; i < 10; ++i) {
    b.Poll([&](const std::string&, u32, const std::vector<u8>&, const std::string&,
               u16) { delivered = true; }, 10);
  }
  CHECK(!delivered);
  CHECK(b.stats().datagramsDropped >= 1);

  // BuildDatagram refuses a bad id and an oversized payload.
  std::vector<u8> big(acfg.mtu, 0);
  CHECK(!a.SendFrame("", 1, adpcm));
  CHECK(!a.SendFrame(std::string(LanVoiceTransport::kMaxIdBytes + 1, 'x'), 1, adpcm));
  CHECK(!a.SendFrame("ok", 1, big));
  CHECK(!a.SendFrame("ok", 1, {}));

  a.Close();
  b.Close();
  CHECK(!a.SendFrame("ok", 1, adpcm));
}

int main() {
  TestCodecRoundTrip();
  TestCodecStateContinuity();
  TestConfigValidation();
  TestStartGating();
  TestTalkGateAndSend();
  TestReceiveProximityAndPlay();
  TestNoProximityResolverDrops();
  TestSpeakersOrderingAndReset();
  TestSpeakerHoldAgesOut();
  TestSpeakerCapIsBounded();
  TestLanTransportLoopback();

  if (g_fail == 0) {
    printf("test_voice: ALL OK\n");
    return 0;
  }
  printf("test_voice: %d FAILURES\n", g_fail);
  return 1;
}
