// Read-only game reads: inventory, server, chat and screen. See
// include/berry/gamereads.h for the model.
#include "berry/gamereads.h"

#include <algorithm>
#include <cctype>

namespace Berry {

namespace {

/// A bounded, printable string: non-empty, within `maxLen`, and free of control
/// bytes. An over-long or control-bearing value is refused wholesale rather
/// than truncated, so a script never reads a value that only looks real.
bool Printable(const std::string& s, size_t maxLen, bool allowEmpty) {
  if (s.empty()) return allowEmpty;
  if (s.size() > maxLen) return false;
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7F) return false;
  }
  return true;
}

std::string ClampStr(const std::string& s, size_t maxLen) {
  if (s.size() <= maxLen) return s;
  return s.substr(0, maxLen);
}

}  // namespace

bool GameReads::ValidItemId(const std::string& id) {
  return Printable(id, kMaxIdLen, /*allowEmpty=*/false);
}

ItemStack GameReads::Sanitize(const ItemStack& in) {
  ItemStack out;
  if (in.empty) return out;   // an explicit empty slot stays empty
  // A stack with no usable id, or a non-positive count, is not a stack.
  if (!ValidItemId(in.id) || in.count <= 0) return out;
  out.empty = false;
  out.id = in.id;
  out.name = Printable(in.name, kMaxNameLen, /*allowEmpty=*/true) ? in.name
                                                                 : std::string();
  out.count = in.count;
  // Damage is normalized into the item's own range: never negative, never past
  // maxDamage, and maxDamage itself never negative.
  out.maxDamage = in.maxDamage > 0 ? in.maxDamage : 0;
  i32 dmg = in.damage > 0 ? in.damage : 0;
  if (out.maxDamage > 0 && dmg > out.maxDamage) dmg = out.maxDamage;
  out.damage = dmg;
  return out;
}

void GameReads::SetInventorySource(InventorySource* src) {
  std::lock_guard<std::mutex> lk(mutex_);
  inventory_ = src;
  ++revision_;
}

void GameReads::SetServerSource(ServerInfoSource* src) {
  std::lock_guard<std::mutex> lk(mutex_);
  server_ = src;
  ++revision_;
}

void GameReads::SetChatSource(ChatSource* src) {
  std::lock_guard<std::mutex> lk(mutex_);
  chat_ = src;
  ++revision_;
}

void GameReads::SetScreenSource(ScreenSource* src) {
  std::lock_guard<std::mutex> lk(mutex_);
  screen_ = src;
  ++revision_;
}

u64 GameReads::revision() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return revision_;
}

size_t GameReads::slotCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inventory_) return 0;
  return std::min(inventory_->SlotCount(), kMaxSlots);
}

ItemStack GameReads::slot(size_t index) const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inventory_ || index >= kMaxSlots) return ItemStack{};
  if (index >= inventory_->SlotCount()) return ItemStack{};
  ItemStack raw;
  if (!inventory_->GetSlot(index, &raw)) return ItemStack{};
  return Sanitize(raw);
}

std::vector<ItemStack> GameReads::items() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<ItemStack> out;
  if (!inventory_) return out;
  const size_t n = std::min(inventory_->SlotCount(), kMaxSlots);
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    ItemStack raw;
    if (!inventory_->GetSlot(i, &raw)) continue;
    ItemStack s = Sanitize(raw);
    if (!s.empty) out.push_back(std::move(s));
  }
  return out;
}

i32 GameReads::selectedSlot() const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inventory_) return -1;
  const i32 sel = inventory_->SelectedSlot();
  // A selection outside the walked window is not reported, so a script never
  // gets a slot index it cannot also read.
  if (sel < 0 || (size_t)sel >= std::min(inventory_->SlotCount(), kMaxSlots)) {
    return -1;
  }
  return sel;
}

i32 GameReads::countOf(const std::string& id) const {
  if (!ValidItemId(id)) return 0;
  std::lock_guard<std::mutex> lk(mutex_);
  if (!inventory_) return 0;
  const size_t n = std::min(inventory_->SlotCount(), kMaxSlots);
  i64 total = 0;
  for (size_t i = 0; i < n; ++i) {
    ItemStack raw;
    if (!inventory_->GetSlot(i, &raw)) continue;
    ItemStack s = Sanitize(raw);
    if (!s.empty && s.id == id) total += s.count;
  }
  // A count that would overflow the script's number is capped rather than
  // wrapped, so a hostile source cannot produce a negative total.
  if (total > 0x7FFFFFFF) total = 0x7FFFFFFF;
  return (i32)total;
}

bool GameReads::has(const std::string& id, i32 minCount) const {
  if (!ValidItemId(id)) return false;
  const i32 want = minCount > 0 ? minCount : 1;
  return countOf(id) >= want;
}

ServerInfo GameReads::server() const {
  std::lock_guard<std::mutex> lk(mutex_);
  ServerInfo info;
  if (!server_) return info;   // disconnected
  info = server_->Info();
  if (!info.connected) return ServerInfo{};   // a source that reports no session
  // Normalize every string and count so a script never reads a hostile value.
  info.host = Printable(info.host, kMaxHostLen, /*allowEmpty=*/true)
                  ? info.host
                  : std::string();
  info.version = ClampStr(info.version, kMaxNameLen);
  info.motd = ClampStr(info.motd, kMaxTextLen);
  info.gamemode = Printable(info.gamemode, kMaxNameLen, true) ? info.gamemode
                                                              : std::string();
  info.dimension = Printable(info.dimension, kMaxNameLen, true) ? info.dimension
                                                                : std::string();
  if (info.port < 0) info.port = 0;
  if (info.protocol < 0) info.protocol = 0;
  if (info.playerCount < 0) info.playerCount = 0;
  if (info.maxPlayers < 0) info.maxPlayers = 0;
  if (info.playerCount > info.maxPlayers && info.maxPlayers > 0) {
    info.playerCount = info.maxPlayers;
  }
  if (!(info.latencyMs >= 0.0f)) info.latencyMs = 0.0f;   // also catches NaN
  return info;
}

std::vector<ChatMessage> GameReads::chat() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<ChatMessage> out;
  if (!chat_) return out;
  std::vector<ChatMessage> raw = chat_->Recent(kMaxChat);
  if (raw.size() > kMaxChat) raw.resize(kMaxChat);
  out.reserve(raw.size());
  for (const ChatMessage& m : raw) {
    ChatMessage c;
    c.sender = Printable(m.sender, kMaxNameLen, /*allowEmpty=*/true)
                   ? m.sender
                   : std::string();
    c.text = ClampStr(m.text, kMaxTextLen);
    c.atMs = m.atMs;
    c.system = m.system;
    out.push_back(std::move(c));
  }
  return out;
}

size_t GameReads::chatCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!chat_) return 0;
  return std::min(chat_->Recent(kMaxChat).size(), kMaxChat);
}

std::string GameReads::chatText(size_t index) const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!chat_ || index >= kMaxChat) return std::string();
  std::vector<ChatMessage> raw = chat_->Recent(kMaxChat);
  if (raw.size() > kMaxChat) raw.resize(kMaxChat);
  if (index >= raw.size()) return std::string();
  return ClampStr(raw[index].text, kMaxTextLen);
}

ScreenState GameReads::screen() const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!screen_) return ScreenState{};   // closed
  ScreenState s = screen_->Current();
  if (!s.open) return ScreenState{};
  s.name = Printable(s.name, kMaxScreenNameLen, /*allowEmpty=*/true) ? s.name
                                                                    : std::string();
  s.title = ClampStr(s.title, kMaxTextLen);
  if (!(s.width >= 0.0f)) s.width = 0.0f;
  if (!(s.height >= 0.0f)) s.height = 0.0f;
  return s;
}

}  // namespace Berry
