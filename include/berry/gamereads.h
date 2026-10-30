#pragma once
// Read-only game reads: inventory, server, chat and screen.
//
// These are *client features*, not new strict Berry:: modules (the strict set
// stays Combat/Render/World/Network). Every one of them reports state the
// client already holds — the player's own inventory, the server it is already
// connected to, the chat it was already sent, and the screen it is already
// drawing — so nothing here reveals anything the client was not already shown.
// There is deliberately no "read another player's inventory" and no
// "read the server's player list beyond the count the tab list already shows".
//
// Like every other seam-backed service in BerryAPI, the core never reads engine
// memory: an adapter (the on-device build) implements the four source
// interfaces below, and the host tests implement them over a shim. A source
// that is absent makes its reads fail closed (an empty inventory, a
// disconnected server, no chat, a closed screen) rather than fabricating data.
//
// Two properties matter here:
//
//   * Everything is bounded. The inventory walk, the chat history and every
//     string are capped, so one query cannot walk a hostile 4096-slot inventory
//     or copy an unbounded MOTD.
//   * Everything is normalized on the way in. A count below zero reads as zero,
//     a stack with no usable id reads as empty, and an oversized string is
//     refused at the seam rather than truncated mid-item, so a script never
//     sees a half-item that looks real.

#include "berry/export.h"
#include "berry/types.h"

#include <mutex>
#include <string>
#include <vector>

namespace Berry {

/// One item stack in the local player's inventory. `empty` is true for an empty
/// slot, in which case every other field is zeroed.
struct BERRY_API ItemStack {
  bool empty = true;
  std::string id;        // "minecraft:diamond_sword", "" when empty
  std::string name;      // display name, may be "" when the item has none
  i32 count = 0;
  i32 damage = 0;        // durability already used; 0 when undamaged
  i32 maxDamage = 0;     // 0 when the item is not damageable
};

/// The server the client is connected to. All of this is what the client
/// already knows: the address it dialled, the protocol it negotiated and the
/// counts the tab list shows. `connected` is false when there is no session.
struct BERRY_API ServerInfo {
  bool connected = false;
  std::string host;
  i32 port = 0;
  std::string version;    // engine version string, e.g. "1.21.0"
  i32 protocol = 0;       // negotiated protocol number
  i32 playerCount = 0;    // players the tab list already shows
  i32 maxPlayers = 0;
  std::string motd;
  std::string gamemode;   // the local player's own gamemode name
  std::string dimension;  // the local player's own dimension name
  f32 latencyMs = 0;      // round-trip time to the server, ms
};

/// One chat line the client was already sent.
struct BERRY_API ChatMessage {
  std::string sender;   // "" for a system line
  std::string text;
  u64 atMs = 0;
  bool system = false;
};

/// The screen the client is currently drawing. `open` is false when the client
/// is in the world with no menu up.
struct BERRY_API ScreenState {
  bool open = false;
  std::string name;      // "chat", "inventory", "pause", … ("" when none)
  std::string title;
  f32 width = 0;
  f32 height = 0;
  bool keyboardOpen = false;
};

//----------------------------------------------------------------------------
// The seams an adapter implements. Every method is read-only and const, so an
// adapter cannot mutate game state through this interface.
//----------------------------------------------------------------------------

class BERRY_API InventorySource {
 public:
  virtual ~InventorySource() = default;
  /// How many slots the inventory has. The service caps how many it walks.
  virtual size_t SlotCount() const = 0;
  /// Fills `out` for `slot`. Returns false when the slot is out of range or the
  /// source cannot answer; the service then reports that slot as empty.
  virtual bool GetSlot(size_t slot, ItemStack* out) const = 0;
  /// The selected hotbar slot, or -1 when unknown.
  virtual i32 SelectedSlot() const { return -1; }
};

class BERRY_API ServerInfoSource {
 public:
  virtual ~ServerInfoSource() = default;
  /// The current session info. `connected` is false when there is no session.
  virtual ServerInfo Info() const = 0;
};

class BERRY_API ChatSource {
 public:
  virtual ~ChatSource() = default;
  /// The recent chat history, oldest first. The service caps how many it reads.
  virtual std::vector<ChatMessage> Recent(size_t maxMessages) const = 0;
};

class BERRY_API ScreenSource {
 public:
  virtual ~ScreenSource() = default;
  /// The screen currently drawn, or a closed state.
  virtual ScreenState Current() const = 0;
};

//----------------------------------------------------------------------------
// GameReads — the service the script bindings read through.
//----------------------------------------------------------------------------

class BERRY_API GameReads {
 public:
  // Bounds. A script cannot make the service walk an unbounded inventory, copy
  // an unbounded chat history, or hold an unbounded string.
  static constexpr size_t kMaxSlots = 256;          // slots walked per read
  static constexpr size_t kMaxChat = 128;           // chat lines returned
  static constexpr size_t kMaxIdLen = 128;
  static constexpr size_t kMaxTextLen = 512;
  static constexpr size_t kMaxNameLen = 128;
  static constexpr size_t kMaxHostLen = 255;
  static constexpr size_t kMaxScreenNameLen = 64;

  GameReads() = default;

  //--- seam binding (client thread; a swap bumps the revision) -------------

  void SetInventorySource(InventorySource* src);
  void SetServerSource(ServerInfoSource* src);
  void SetChatSource(ChatSource* src);
  void SetScreenSource(ScreenSource* src);

  //--- inventory reads -----------------------------------------------------

  /// How many slots the inventory reports (0 when no source). Capped at
  /// `kMaxSlots`; a source that reports more is not walked past the cap.
  size_t slotCount() const;
  /// The stack in `slot`. An out-of-range or unreadable slot reads as empty.
  ItemStack slot(size_t slot) const;
  /// Every non-empty stack, in slot order.
  std::vector<ItemStack> items() const;
  /// The selected hotbar slot, or -1 when unknown.
  i32 selectedSlot() const;
  /// The total count of `id` across the inventory, 0 when none.
  i32 countOf(const std::string& id) const;
  /// Whether the inventory holds at least `minCount` of `id` (`minCount` <= 0
  /// means "any"). False for an empty or oversized id, or no source.
  bool has(const std::string& id, i32 minCount) const;

  //--- server reads --------------------------------------------------------

  ServerInfo server() const;

  //--- chat reads ----------------------------------------------------------

  /// The recent chat, oldest first, capped at `kMaxChat`.
  std::vector<ChatMessage> chat() const;
  /// How many lines the history holds (capped).
  size_t chatCount() const;
  /// The text of chat line `index` (0 = oldest), or "" when out of range.
  std::string chatText(size_t index) const;

  //--- screen reads --------------------------------------------------------

  ScreenState screen() const;

  /// A counter that increases whenever a seam is swapped. Exposed so a script
  /// can cheaply notice that the reads are now backed by something else; it is
  /// deliberately not a per-frame poll signal (the reads are pull-through).
  u64 revision() const;

  /// True when `id` is a usable item id (non-empty, bounded, printable).
  static bool ValidItemId(const std::string& id);

 private:
  /// Sanitizes a stack read from a source: an unusable id or a non-positive
  /// count becomes an empty stack, and damage is clamped to the stack's own
  /// range so a script never sees a negative count or an over-damaged item.
  static ItemStack Sanitize(const ItemStack& in);

  mutable std::mutex mutex_;
  InventorySource* inventory_ = nullptr;
  ServerInfoSource* server_ = nullptr;
  ChatSource* chat_ = nullptr;
  ScreenSource* screen_ = nullptr;
  u64 revision_ = 0;
};

}  // namespace Berry
