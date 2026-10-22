// Session recorder. See include/berry/session_recorder.h for the model.
#include "berry/session_recorder.h"

namespace Berry {

SessionRecorder::SessionRecorder() = default;
SessionRecorder::~SessionRecorder() = default;

//----------------------------------------------------------------------------
// Capture
//----------------------------------------------------------------------------

bool SessionRecorder::Start() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (recording_) return false;
  tape_.Clear();
  recorder_ = std::make_unique<Recorder>(&tape_);
  overflowed_ = false;
  recording_ = true;
  return true;
}

bool SessionRecorder::Stop() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!recording_) return false;
  recording_ = false;
  return true;
}

bool SessionRecorder::recording() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return recording_;
}

void SessionRecorder::CaptureTick(u64 tick, LevelSeam& seam,
                                  const std::vector<WorldDiffEvent>& diff) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!recording_ || !recorder_) return;

  recorder_->BeginFrame(tick, seam.GetTime());
  recorder_->CaptureFrom(seam);

  // Record the same transitions the grid published, so a replay carries the
  // entered/left/updated events and not only the snapshots. The channel and
  // payload are exactly what PublishWorldDiffToEvents would emit, so a
  // replayed event feeds an `Events.once` listener unchanged.
  for (const WorldDiffEvent& e : diff) {
    const char* channel = WorldDiffChannel(e.kind);
    if (!channel) continue;
    const EntitySnapshot& snap =
        e.kind == WorldDiff::Left ? e.before : e.after;
    if (!recorder_->AddEvent(channel, WorldDiffPayload(snap))) {
      overflowed_ = true;
      break;
    }
  }

  if (!recorder_->CommitFrame()) {
    // Either the frame overflowed, or the tick did not strictly increase. Both
    // mean the frame was not written; overflow is the reportable case.
    if (recorder_->overflowed()) overflowed_ = true;
  }
  if (recorder_->overflowed()) overflowed_ = true;
}

bool SessionRecorder::overflowed() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return overflowed_;
}

//----------------------------------------------------------------------------
// Tape
//----------------------------------------------------------------------------

size_t SessionRecorder::frames() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return tape_.frameCount();
}

bool SessionRecorder::empty() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return tape_.empty();
}

void SessionRecorder::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  tape_.Clear();
  overflowed_ = false;
  if (replay_) replay_->Rewind();
}

std::string SessionRecorder::Save() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return tape_.ToJson();
}

bool SessionRecorder::Load(const std::string& json, TapeError* outError,
                           std::string* detail) {
  // Parse outside the lock into a temporary, then adopt it, so a rejected
  // document leaves the current tape untouched.
  Recording parsed;
  TapeError err = TapeError::Ok;
  std::string why;
  if (!Recording::FromJson(json, &parsed, &err, &why)) {
    if (outError) *outError = err;
    if (detail) *detail = why;
    return false;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  tape_ = std::move(parsed);
  if (replay_) replay_->Rewind();
  if (outError) *outError = TapeError::Ok;
  if (detail) detail->clear();
  return true;
}

//----------------------------------------------------------------------------
// Replay
//----------------------------------------------------------------------------

std::shared_ptr<LevelSeam> SessionRecorder::ReplaySeamHandle() {
  std::lock_guard<std::mutex> lk(mutex_);
  // The seam borrows &tape_. Because `tape_` is a member, its address is stable
  // for the life of this recorder: Clear and Load mutate it in place rather
  // than replacing the object, so a handle handed out here stays valid.
  if (!replay_) replay_ = std::make_shared<ReplaySeam>(&tape_);
  return replay_;
}

bool SessionRecorder::ReplayStep() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!replay_) replay_ = std::make_shared<ReplaySeam>(&tape_);
  return replay_->Step();
}

bool SessionRecorder::ReplaySeek(size_t index) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!replay_) replay_ = std::make_shared<ReplaySeam>(&tape_);
  return replay_->SeekTo(index);
}

void SessionRecorder::ReplayRewind() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (replay_) replay_->Rewind();
}

bool SessionRecorder::ReplayAtEnd() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return !replay_ || replay_->atEnd();
}

u64 SessionRecorder::ReplayTick() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return replay_ ? replay_->currentTick() : 0;
}

i64 SessionRecorder::ReplayIndex() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return replay_ ? replay_->frameIndex() : -1;
}

}  // namespace Berry
