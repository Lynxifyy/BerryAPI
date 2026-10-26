// Packet capture: the receive path that fills a replay with real packets.
// See include/berry/packet_capture.h for the model.
#include "berry/packet_capture.h"

#include "berry/event.h"
#include "berry/logger.h"
#include "berry/replay_capture.h"

#include <cstring>
#include <utility>

namespace Berry {

namespace {

// The two singleton hooks, one per direction. A hook backend patches a single
// function pointer, so a direction has exactly one live hook; the detours below
// forward through these. Guarded by `g_hookMutex`.
std::mutex g_hookMutex;
PacketReceiveHook* g_inbound = nullptr;
PacketReceiveHook* g_outbound = nullptr;

PacketReceiveHook*& SlotFor(PacketDir dir) {
  return dir == PacketDir::Outbound ? g_outbound : g_inbound;
}

// The built-in table: the well-known Bedrock protocol ids. These are a property
// of the protocol version, not of this library, so an embedder replaces the
// rows it cares about through `PacketIdTable::Set` / `CopyRowsFrom`. Ids that
// are not listed classify as `Other`, which is safe: an unclassified packet is
// still stored, just without a specialised kind.
struct BuiltinRow {
  u32 id;
  PacketClass cls;
  const char* name;
};

// The numbers are from the Bedrock protocol (gophertunnel's `id.go`), which is
// the de-facto reference. They shift between protocol versions; the table is a
// starting point, not a promise.
constexpr BuiltinRow kBuiltin[] = {
    // Time / weather.
    {9, PacketClass::Time, "SetTime"},
    {24, PacketClass::Time, "LevelEvent"},
    // Chunk / sub-chunk.
    {57, PacketClass::Chunk, "LevelChunk"},
    {120, PacketClass::Chunk, "NetworkChunkPublisherUpdate"},
    {128, PacketClass::Chunk, "ClientCacheStatus"},
    {129, PacketClass::Chunk, "OnScreenTextureAnimation"},
    {134, PacketClass::Chunk, "ClientCacheBlobStatus"},
    {135, PacketClass::Chunk, "ClientCacheMissResponse"},
    {171, PacketClass::Chunk, "UpdateSubChunkBlocks"},
    {173, PacketClass::Chunk, "SubChunk"},
    {174, PacketClass::Chunk, "SubChunkRequest"},
    // Blocks.
    {20, PacketClass::Block, "UpdateBlock"},
    {21, PacketClass::Block, "AddPainting"},
    {25, PacketClass::Block, "BlockEvent"},
    {55, PacketClass::Block, "BlockActorData"},
    {109, PacketClass::Block, "UpdateBlockSynced"},
    // Entities.
    {11, PacketClass::Entity, "AddPlayer"},
    {12, PacketClass::Entity, "AddActor"},
    {13, PacketClass::Entity, "RemoveActor"},
    {14, PacketClass::Entity, "AddItemActor"},
    {17, PacketClass::Entity, "MoveActorAbsolute"},
    {18, PacketClass::Entity, "MovePlayer"},
    {38, PacketClass::Entity, "SetActorData"},
    {39, PacketClass::Entity, "SetActorMotion"},
    {40, PacketClass::Entity, "SetActorLink"},
    {92, PacketClass::Entity, "PlayerSkin"},
    {110, PacketClass::Entity, "MoveActorDelta"},
    {117, PacketClass::Entity, "SpawnParticleEffect"},
    {156, PacketClass::Entity, "MotionPredictionHints"},
    {157, PacketClass::Entity, "AnimateEntity"},
    {160, PacketClass::Entity, "CorrectPlayerMovePrediction"},
    {165, PacketClass::Entity, "AddVolumeEntity"},
    {166, PacketClass::Entity, "RemoveVolumeEntity"},
    // The local player's own input / actions.
    {35, PacketClass::Player, "PlayerAction"},
    {112, PacketClass::Player, "SetLocalPlayerAsInitialised"},
    {143, PacketClass::Player, "PlayerAuthInput"},
    {146, PacketClass::Player, "ItemStackRequest"},
    {175, PacketClass::Player, "ClientStartItemCooldown"},
    // Misc well-known.
    {8, PacketClass::Other, "Text"},
    {10, PacketClass::Other, "StartGame"},
    {63, PacketClass::Other, "SimpleEvent"},
};

}  // namespace

const char* PacketClassName(PacketClass c) {
  switch (c) {
    case PacketClass::Chunk: return "chunk";
    case PacketClass::Block: return "block";
    case PacketClass::Entity: return "entity";
    case PacketClass::Time: return "time";
    case PacketClass::Player: return "player";
    default: return "other";
  }
}

//----------------------------------------------------------------------------
// PacketIdTable
//----------------------------------------------------------------------------

const PacketIdTable& PacketIdTable::Default() {
  // A function-local static with static lifetime: constructed once, never
  // copied (the table is non-copyable and its interned names must not move).
  static const PacketIdTable* table = [] {
    PacketIdTable* t = new PacketIdTable();
    for (const BuiltinRow& r : kBuiltin) t->Set(r.id, r.cls, r.name);
    return t;
  }();
  return *table;
}

PacketIdInfo PacketIdTable::Lookup(u32 id) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = rows_.find(id);
  if (it == rows_.end()) return PacketIdInfo{};
  return it->second;
}

void PacketIdTable::Set(u32 id, PacketClass cls, const char* name) {
  std::lock_guard<std::mutex> lk(mutex_);
  // Intern the name so the returned `PacketIdInfo::name` stays valid for the
  // table's lifetime; the deque never invalidates an element's address.
  names_.emplace_back(name ? name : "");
  PacketIdInfo info;
  info.cls = cls;
  info.name = names_.back().c_str();
  rows_[id] = info;
}

void PacketIdTable::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  rows_.clear();
  names_.clear();
}

void PacketIdTable::CopyRowsFrom(const PacketIdTable& other) {
  // Snapshot the other table's rows under its lock, then install them here, so
  // two tables cannot deadlock against each other and a concurrent mutation of
  // `other` cannot tear the copy.
  std::vector<std::pair<u32, std::pair<PacketClass, std::string>>> snap;
  {
    std::lock_guard<std::mutex> lk(other.mutex_);
    snap.reserve(other.rows_.size());
    for (const auto& [id, info] : other.rows_) {
      snap.emplace_back(id, std::make_pair(info.cls, std::string(info.name)));
    }
  }
  std::lock_guard<std::mutex> lk(mutex_);
  rows_.clear();
  names_.clear();
  for (auto& [id, pair] : snap) {
    names_.push_back(std::move(pair.second));
    PacketIdInfo info;
    info.cls = pair.first;
    info.name = names_.back().c_str();
    rows_[id] = info;
  }
}

size_t PacketIdTable::size() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return rows_.size();
}

ReplayRecordKind ReplayKindForClass(PacketClass cls) {
  switch (cls) {
    case PacketClass::Chunk:
    case PacketClass::Block:
      return ReplayRecordKind::Chunk;
    default:
      return ReplayRecordKind::Packet;
  }
}

//----------------------------------------------------------------------------
// PacketCaptureHub
//----------------------------------------------------------------------------

PacketCaptureHub::PacketCaptureHub() {
  table_.CopyRowsFrom(PacketIdTable::Default());
}

void PacketCaptureHub::AddObserver(PacketObserver* o) {
  if (!o) return;
  std::lock_guard<std::mutex> lk(mutex_);
  for (PacketObserver* e : observers_) {
    if (e == o) return;   // idempotent
  }
  observers_.push_back(o);
}

void PacketCaptureHub::RemoveObserver(PacketObserver* o) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto it = observers_.begin(); it != observers_.end(); ++it) {
    if (*it == o) {
      observers_.erase(it);
      return;
    }
  }
}

size_t PacketCaptureHub::observerCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return observers_.size();
}

size_t PacketCaptureHub::Dispatch(PacketDir dir, u32 id,
                                  const std::string& payload, u64 tick) {
  // Classify under the table's own lock, then snapshot the observer list, so a
  // slow observer does not hold the hub lock while it runs and an observer that
  // registers mid-dispatch is not half-visited.
  const PacketClass cls = table_.Lookup(id).cls;
  std::vector<PacketObserver*> obs;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    obs = observers_;
    ++dispatched_;
  }
  for (PacketObserver* o : obs) {
    if (o) o->OnPacket(dir, id, cls, payload, tick);
  }
  return obs.size();
}

u64 PacketCaptureHub::dispatched() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return dispatched_;
}

//----------------------------------------------------------------------------
// The production observers.
//----------------------------------------------------------------------------

void PacketLogObserver::OnPacket(PacketDir dir, u32 id, PacketClass /*cls*/,
                                 const std::string& payload, u64 /*tick*/) {
  if (!log_) return;
  // Resolve the name from the hub's table so the log carries it without a
  // second look-up; a null table (or an unknown id) yields an empty name, which
  // is the log's own "unknown" marker. The payload is measured, never stored —
  // the log is metadata only by construction.
  const char* name = table_ ? table_->Lookup(id).name : nullptr;
  if (log_->Record(dir, id, (u32)payload.size(), NowMs(),
                   name ? std::string(name) : std::string())) {
    ++recorded_;
  }
}

void ReplayPacketObserver::OnPacket(PacketDir dir, u32 id, PacketClass cls,
                                    const std::string& payload, u64 tick) {
  if (!capture_) return;
  if (capture_->CapturePacket(dir, id, payload, tick,
                              ReplayKindForClass(cls))) {
    ++stored_;
  }
}

//----------------------------------------------------------------------------
// PacketReceiveHook
//----------------------------------------------------------------------------

PacketReceiveHook::~PacketReceiveHook() { Uninstall(); }

void PacketReceiveHook::InboundDetour(void* self, const void* packet) {
  DetourFor(PacketDir::Inbound, self, packet);
}

void PacketReceiveHook::OutboundDetour(void* self, const void* packet) {
  DetourFor(PacketDir::Outbound, self, packet);
}

void PacketReceiveHook::DetourFor(PacketDir dir, void* self,
                                  const void* packet) {
  PacketReceiveHook* h = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    h = SlotFor(dir);
  }
  if (h) h->Handle(self, packet);
}

void PacketReceiveHook::Handle(void* self, const void* packet) {
  // Call the game's original first, so this hook never changes what the client
  // does — it only observes. The original is the trampoline the backend
  // produced.
  ReceiveFn orig = hook_.original();
  if (orig) orig(self, packet);
  if (!packet || !hub_) return;
  u32 id = 0;
  std::string payload;
  if (!abi_.decode || !abi_.decode(packet, &id, &payload)) return;
  const u64 tick = abi_.tick ? abi_.tick() : 0;
  hub_->Dispatch(dir_, id, payload, tick);
  ++captured_;
}

bool PacketReceiveHook::Install(void* target, PacketReceiveAbi abi,
                                IHook* backend, PacketCaptureHub* hub,
                                PacketDir dir) {
  if (installed_) return false;
  if (!target || !backend || !hub) return false;
  if (dir != PacketDir::Inbound && dir != PacketDir::Outbound) return false;
  if (!abi.decode) {
    // Without a decoder the hook cannot turn the packet object into bytes, so
    // it would capture nothing. Refuse rather than install a hook that does
    // nothing (which would also occupy the direction's singleton slot).
    Log::Warn("PacketReceiveHook: no decode ABI; capture not installed");
    return false;
  }
  if (!backend->CanHook()) return false;

  // One hook per direction: the detour is a fixed static function, so a second
  // hook on the same direction would have to share this singleton slot.
  {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    if (SlotFor(dir) != nullptr) return false;
  }

  backend_ = backend;
  abi_ = std::move(abi);
  hub_ = hub;
  dir_ = dir;
  const ReceiveFn detour =
      (dir == PacketDir::Outbound) ? &PacketReceiveHook::OutboundDetour
                                   : &PacketReceiveHook::InboundDetour;
  hook_.SetBackend(backend);
  if (hook_.Install(target, detour) != HookStatus::Ok) {
    backend_ = nullptr;
    hub_ = nullptr;
    return false;
  }
  HookManager::Instance().Track(
      target, HookSymbol{target, (void*)detour, (void*)hook_.original(), true});
  {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    SlotFor(dir) = this;
  }
  installed_ = true;
  Log::Info("PacketReceiveHook: installed on %p (%s)", target,
            dir == PacketDir::Outbound ? "outbound" : "inbound");
  return true;
}

void PacketReceiveHook::Uninstall() {
  if (!installed_) return;
  if (backend_ && hook_.installed()) {
    backend_->Uninstall(hook_.target());
    HookManager::Instance().Untrack(hook_.target());
  }
  {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    if (SlotFor(dir_) == this) SlotFor(dir_) = nullptr;
  }
  abi_ = PacketReceiveAbi{};
  hub_ = nullptr;
  backend_ = nullptr;
  installed_ = false;
}

}  // namespace Berry
