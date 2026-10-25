// Replay mod service. See include/berry/replay_service.h for the model.
#include "berry/replay_service.h"

#include "berry/logger.h"

#include <cstdio>

namespace Berry {

ReplayService::ReplayService() = default;
ReplayService::~ReplayService() {
  // The server holds a raw file pointer; drop the server first so nothing
  // outlives the file it streams.
  server_.Stop();
}

bool ReplayService::Load(const std::string& bytes, ReplayError* err) {
  // Decode outside the lock into a temporary, then adopt it, so a rejected
  // replay leaves the current one untouched.
  auto parsed = std::make_unique<ReplayFile>();
  ReplayReader reader;
  const ReplayError e = reader.Read(bytes, parsed.get());
  if (e != ReplayError::Ok) {
    if (err) *err = e;
    return false;
  }
  // A replay is only meaningful to the parsers of the build it was recorded
  // against: the packet ids and layouts it carries are a property of the
  // protocol version. Refuse a mismatch before adopting, so a wrong-version
  // file never reaches the world, the review or the loopback server. The
  // fingerprint error already means "recorded against another build", which is
  // exactly this refusal.
  const u32 expected = expectedProtocolVersion_.load();
  if (expected != 0 && parsed->fingerprint.protocolVersion != expected) {
    if (err) *err = ReplayError::BadFingerprint;
    return false;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  review_.Stop();
  server_.Attach(nullptr);
  file_ = std::move(parsed);
  world_ = std::make_unique<ReplayWorld>(file_.get());
  server_.Attach(file_.get());
  if (err) *err = ReplayError::Ok;
  return true;
}

ReplayError ReplayService::Save(std::string* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (file_) {
    ReplayWriter w;
    return w.Write(*file_, out);
  }
  // Nothing loaded: export whatever the capture holds, so a script can save a
  // recording without a separate load.
  return const_cast<ReplayCapture&>(capture_).Save(out);
}

bool ReplayService::loaded() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return file_ != nullptr && !file_->sections.empty();
}

const ReplayFile* ReplayService::file() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return file_.get();
}

ReplayWorld* ReplayService::world() {
  std::lock_guard<std::mutex> lk(mutex_);
  return world_.get();
}

void ReplayService::Unload() {
  std::lock_guard<std::mutex> lk(mutex_);
  review_.Stop();
  server_.Attach(nullptr);
  world_.reset();
  file_.reset();
}

void ReplayService::SetLocalPlayer(EntityId id) {
  localPlayerRaw_.store(id.raw);
}

EntityId ReplayService::localPlayer() const {
  EntityId id;
  id.raw = localPlayerRaw_.load();
  id.runtime = (u16)id.raw;
  return id;
}

void ReplayService::SetLiveSessionProbe(std::function<bool()> probe) {
  std::lock_guard<std::mutex> lk(probeMutex_);
  liveProbe_ = std::move(probe);
}

void ReplayService::SetExpectedProtocolVersion(u32 version) {
  expectedProtocolVersion_.store(version);
}

u32 ReplayService::expectedProtocolVersion() const {
  return expectedProtocolVersion_.load();
}

void ReplayService::ApplyServerGuards(ReplayServerConfig* cfg) const {
  if (!cfg) return;
  cfg->expectedProtocolVersion = expectedProtocolVersion_.load();
  std::lock_guard<std::mutex> lk(probeMutex_);
  // The probe is copied by value: a caller that outlives the install (a script
  // binding that starts the server, then the script unloads) still gets a safe
  // answer, and the copy holds the target by shared ownership of whatever it
  // captured.
  cfg->liveSessionActive = liveProbe_;
}

void ReplayService::Tick(u64 tick, LevelSeam& seam,
                         const std::vector<WorldDiffEvent>& diff) {
  capture_.CaptureTick(tick, seam, diff, localPlayer());
}

size_t ReplayService::PumpServer() { return server_.Pump(0); }

void ReplayService::AdvanceReview(f32 dtSeconds) {
  review_.Advance(dtSeconds);
}

size_t ReplayService::PresentReview(const CameraState& cam, Renderer& r,
                                    const Vec2& screen) {
  if (!review_.active()) return 0;
  return review_.Present(cam, r, screen);
}

std::string ReplayService::status() const {
  const CaptureState st = capture_.state();
  char buf[256];
  std::snprintf(buf, sizeof buf,
                "replay: %s sections=%zu bytes=%zu loaded=%d client=%d",
                CaptureStateName(st), capture_.sectionCount(), capture_.bytes(),
                loaded() ? 1 : 0, server_.clientConnected() ? 1 : 0);
  return std::string(buf);
}

}  // namespace Berry
