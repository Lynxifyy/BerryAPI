// Host tests for the client-side audio service (Phase D).
#include "berry/audio.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {
// Records what would have played, so the tests assert on real calls, not mocks
// of the service under test.
struct RecordingSeam : AudioSeam {
  std::vector<AudioCue> played;
  bool accept = true;
  bool Play(const AudioCue& cue) override {
    if (!accept) return false;
    played.push_back(cue);
    return true;
  }
};
}  // namespace

static void TestPlayAndHistory() {
  AudioService svc;
  RecordingSeam seam;
  svc.SetSeam(&seam);

  CHECK(svc.Play("hit", 0.8f, 1.0f));
  CHECK(svc.Play("kill", 1.0f, 1.5f));
  CHECK(seam.played.size() == 2);
  CHECK(seam.played[0].name == "hit");
  CHECK(svc.HistoryCount() == 2);
  const std::vector<AudioCue> h = svc.History();
  CHECK(h.size() == 2);
  CHECK(h[1].name == "kill");

  // The history is bounded.
  for (size_t i = 0; i < AudioService::kMaxHistory + 20; ++i) {
    svc.Play("tick", 1.0f, 1.0f);
  }
  CHECK(svc.HistoryCount() == AudioService::kMaxHistory);

  svc.ClearHistory();
  CHECK(svc.HistoryCount() == 0);
}

static void TestValidationAndClamping() {
  AudioService svc;
  RecordingSeam seam;
  svc.SetSeam(&seam);

  // A malformed name is refused before the backend is reached.
  CHECK(!svc.Play(""));
  CHECK(!svc.Play(std::string(100, 'a')));
  CHECK(!svc.Play("bad\nname"));
  CHECK(seam.played.empty());

  // Volume and pitch are clamped, not refused.
  CHECK(svc.Play("hit", 5.0f, 99.0f));
  CHECK(seam.played.back().volume == 1.0f);
  CHECK(seam.played.back().pitch == 4.0f);
  CHECK(svc.Play("hit", -2.0f, 0.0f));
  CHECK(seam.played.back().volume == 0.0f);
  CHECK(seam.played.back().pitch == 0.25f);

  // Master volume clamps.
  svc.SetVolume(2.0f);
  CHECK(svc.volume() == 1.0f);
  svc.SetVolume(-1.0f);
  CHECK(svc.volume() == 0.0f);
}

static void TestMuteAndNoBackend() {
  AudioService svc;
  RecordingSeam seam;
  svc.SetSeam(&seam);

  svc.SetMuted(true);
  CHECK(svc.muted());
  CHECK(!svc.Play("hit", 1.0f, 1.0f));   // dropped, not queued
  CHECK(seam.played.empty());
  CHECK(svc.HistoryCount() == 0);

  svc.SetMuted(false);
  CHECK(svc.Play("hit", 1.0f, 1.0f));

  // With no backend, a play fails closed and records nothing.
  AudioService bare;
  CHECK(!bare.Play("hit", 1.0f, 1.0f));
  CHECK(bare.HistoryCount() == 0);

  // A backend that refuses reports false.
  RecordingSeam refusing;
  refusing.accept = false;
  svc.SetSeam(&refusing);
  CHECK(!svc.Play("hit", 1.0f, 1.0f));
}

int main() {
  TestPlayAndHistory();
  TestValidationAndClamping();
  TestMuteAndNoBackend();
  if (g_fail == 0) printf("test_audio: ALL OK\n");
  return g_fail;
}
