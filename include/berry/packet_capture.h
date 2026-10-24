#pragma once
// Packet capture: the receive path that fills a replay with real packets.
//
// `ReplayCapture` knows how to *store* a packet; this is what *delivers* one in
// production. It has three pieces:
//
//   * **`PacketIdTable`** classifies a protocol id (chunk? block? entity?) so a
//     record carries the right `ReplayRecordKind` without a consumer having to
//     re-parse the id. The built-in table covers the well-known Bedrock ids;
//     an embedder replaces it per build, because ids are a property of the
//     protocol version, not of this library.
//   * **`PacketCaptureHub`** is the fan-out: the receive path calls `Dispatch`
//     once per packet and every registered `PacketObserver` sees it. The two
//     production observers are the metadata-only `PacketLog` and the payload
//     `ReplayCapture`.
//   * **`PacketReceiveHook`** installs a detour on the game's packet receive
//     (or send) function through the pluggable `IHook` backend and forwards
//     each packet to the hub. The game ABI is per build, so the hook takes a
//     `PacketReceiveAbi` — the two accessors a device adapter fills from the
//     resolved per-build code. Nothing here guesses a struct layout: with no
//     ABI supplied the hook captures nothing.
//
// Fail-closed by construction: a null target, an unresolved signature, or an
// ABI whose `decode` refuses leaves the packet uncaptured rather than storing a
// wrong one. Capture is read-only with respect to the world — it copies bytes
// the client already received and never writes game memory.

#include "berry/export.h"
#include "berry/hooks.h"
#include "berry/packet.h"
#include "berry/replay_format.h"
#include "berry/types.h"

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {

/// A coarse class for a protocol id, enough to route a captured packet.
enum class PacketClass : u8 {
  Other = 0,
  Chunk,      // chunk / sub-chunk payload
  Block,      // a single block or block-actor update
  Entity,     // entity spawn / move / metadata
  Time,       // level time or weather
  Player,     // the local player's own input
};

const char* PacketClassName(PacketClass c);

/// The classification of one id: its class and a resolved name ("" when
/// unknown). `name` has static lifetime — a literal from the built-in table or
/// an interned copy owned by the table.
struct BERRY_API PacketIdInfo {
  PacketClass cls = PacketClass::Other;
  const char* name = "";
};

/// Maps a protocol id to a `PacketIdInfo`. Thread-safe; the built-in table is
/// immutable and a custom table is mutated behind a lock.
class BERRY_API PacketIdTable {
 public:
  PacketIdTable() = default;
  PacketIdTable(const PacketIdTable&) = delete;
  PacketIdTable& operator=(const PacketIdTable&) = delete;

  /// The built-in table for the well-known Bedrock ids. Immutable.
  static const PacketIdTable& Default();

  /// The info for `id`, or a default-constructed `PacketIdInfo` when the id is
  /// unknown (class Other, empty name) — never a fabricated name.
  PacketIdInfo Lookup(u32 id) const;

  /// Adds or replaces one row. `name` is copied and interned, so a caller's
  /// buffer need not outlive the call.
  void Set(u32 id, PacketClass cls, const char* name);

  void Clear();

  /// Replaces this table's rows with a copy of `other`'s. Used to seed a hub's
  /// mutable table from the immutable built-in one.
  void CopyRowsFrom(const PacketIdTable& other);

  size_t size() const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<u32, PacketIdInfo> rows_;
  // Interned names for `Set`: a deque never invalidates the addresses of its
  // elements, so a `PacketIdInfo::name` stays valid for the table's lifetime.
  std::deque<std::string> names_;
};

/// The `ReplayRecordKind` a captured packet of `cls` is stored as. Chunk and
/// block classes map to `Chunk` (the container's one block-data kind);
/// everything else is a plain `Packet`.
BERRY_API ReplayRecordKind ReplayKindForClass(PacketClass cls);

//----------------------------------------------------------------------------
// Observers and the fan-out hub.
//----------------------------------------------------------------------------

/// Receives every observed packet. Implemented by the metadata log and the
/// replay capture. `cls` is the hub's classification of `id`, so an observer
/// does not look it up again.
class BERRY_API PacketObserver {
 public:
  virtual ~PacketObserver() = default;
  virtual void OnPacket(PacketDir dir, u32 id, PacketClass cls,
                        const std::string& payload, u64 tick) = 0;
};

/// The fan-out. The receive path calls `Dispatch` once per packet; every
/// registered observer sees it in registration order. Thread-safe: the receive
/// hook runs on the network worker while observers are added on attach.
class BERRY_API PacketCaptureHub {
 public:
  PacketCaptureHub();

  void AddObserver(PacketObserver* o);
  void RemoveObserver(PacketObserver* o);
  size_t observerCount() const;

  /// Classifies `id` and forwards to every observer. Returns how many observers
  /// saw it (0 when none is registered). Never throws; a null observer is
  /// skipped.
  size_t Dispatch(PacketDir dir, u32 id, const std::string& payload, u64 tick);

  /// The mutable classification table. The embedder replaces rows per build.
  PacketIdTable& table() { return table_; }
  const PacketIdTable& table() const { return table_; }

  /// How many packets have been dispatched since construction.
  u64 dispatched() const;

 private:
  mutable std::mutex mutex_;
  std::vector<PacketObserver*> observers_;
  PacketIdTable table_;
  u64 dispatched_ = 0;
};

//----------------------------------------------------------------------------
// The receive hook.
//----------------------------------------------------------------------------

/// The two accessors a device adapter supplies so the hook can turn the game's
/// opaque packet object into (id, payload) and stamp it with a tick. Both are
/// per-build; a missing `decode` makes the hook capture nothing.
struct BERRY_API PacketReceiveAbi {
  /// Reads the id and payload out of the game's packet object. Returns false
  /// (and leaves the outputs untouched) when the object is not a packet this
  /// build understands, so a wrong layout fails closed.
  std::function<bool(const void* packet, u32* id, std::string* payload)> decode;
  /// The tick the packet arrived on. Empty means "stamp 0".
  std::function<u64()> tick;
};

/// Installs a detour on one packet path (receive or send) and forwards each
/// packet to the hub. Fail-closed: `Install` returns false — and no hook is
/// left behind — when the target is null, the backend cannot hook, or no ABI is
/// supplied.
///
/// The detour ABI is the conventional Bedrock one, `void(void* self, const
/// void* packet)`. The game's real signature is resolved per build by the
/// caller; the `decode` accessor handles the packet's actual layout, so this
/// class never assumes a struct.
class BERRY_API PacketReceiveHook {
 public:
  using ReceiveFn = void (*)(void*, const void*);

  PacketReceiveHook() = default;
  ~PacketReceiveHook();

  PacketReceiveHook(const PacketReceiveHook&) = delete;
  PacketReceiveHook& operator=(const PacketReceiveHook&) = delete;

  /// Installs the detour. `backend` is the hook engine (`Hooks().Backend()`).
  /// `hub` must outlive the hook. Returns false on a refusal; on success
  /// `installed()` is true and the hook is tracked by the backend.
  bool Install(void* target, PacketReceiveAbi abi, IHook* backend,
               PacketCaptureHub* hub, PacketDir dir = PacketDir::Inbound);

  /// Removes the detour and clears the ABI. Idempotent.
  void Uninstall();

  bool installed() const { return installed_; }
  PacketDir direction() const { return dir_; }
  /// How many packets this hook has forwarded to the hub.
  u64 captured() const { return captured_; }

 private:
  /// The detour body: call the original through the trampoline, then decode and
  /// dispatch to the hub.
  void Handle(void* self, const void* packet);

  /// The two static detours the backend installs. A backend patches one target
  /// with one plain function pointer, so there is exactly one per direction;
  /// the detour forwards to the singleton hook registered for that direction.
  /// A second install of the same direction is refused rather than silently
  /// sharing the target.
  static void InboundDetour(void* self, const void* packet);
  static void OutboundDetour(void* self, const void* packet);
  static void DetourFor(PacketDir dir, void* self, const void* packet);

  IHook* backend_ = nullptr;
  Hook<ReceiveFn> hook_;
  PacketReceiveAbi abi_;
  PacketCaptureHub* hub_ = nullptr;
  PacketDir dir_ = PacketDir::Inbound;
  bool installed_ = false;
  u64 captured_ = 0;
};

}  // namespace Berry
