// Host tests for the read-only game reads service (inventory, server, chat,
// screen).
//
// These drive the real service over hand-written sources, so no device and no
// game are needed. The point of the suite is the boundary, not the happy path:
// a slot with no usable id or a non-positive count must read as empty, damage
// must be clamped into the item's own range, a source that reports more slots
// than the cap must not be walked past it, a chat line with a control byte in
// its sender must read as a system line, an over-long MOTD must be bounded, and
// an absent source must read as empty rather than as fabricated data.
#include "berry/gamereads.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

//--- test sources ----------------------------------------------------------

struct FakeInventory : InventorySource {
  std::vector<ItemStack> slots;
  i32 selected = -1;
  bool failRead = false;
  size_t SlotCount() const override { return slots.size(); }
  bool GetSlot(size_t slot, ItemStack* out) const override {
    if (failRead || slot >= slots.size()) return false;
    *out = slots[slot];
    return true;
  }
  i32 SelectedSlot() const override { return selected; }
};

struct FakeServer : ServerInfoSource {
  ServerInfo info;
  ServerInfo Info() const override { return info; }
};

struct FakeChat : ChatSource {
  std::vector<ChatMessage> lines;
  std::vector<ChatMessage> Recent(size_t maxMessages) const override {
    if (lines.size() <= maxMessages) return lines;
    return std::vector<ChatMessage>(lines.begin(), lines.begin() + maxMessages);
  }
};

struct FakeScreen : ScreenSource {
  ScreenState state;
  ScreenState Current() const override { return state; }
};

static ItemStack Stack(const std::string& id, i32 count, i32 dmg = 0, i32 maxDmg = 0) {
  ItemStack s;
  s.empty = false;
  s.id = id;
  s.count = count;
  s.damage = dmg;
  s.maxDamage = maxDmg;
  return s;
}

//--- tests -----------------------------------------------------------------

static void TestFailsClosedWithoutSources() {
  GameReads r;
  CHECK(r.slotCount() == 0);
  CHECK(r.slot(0).empty);
  CHECK(r.items().empty());
  CHECK(r.selectedSlot() == -1);
  CHECK(r.countOf("minecraft:stone") == 0);
  CHECK(!r.has("minecraft:stone", 1));
  CHECK(!r.server().connected);
  CHECK(r.server().host.empty());
  CHECK(r.chat().empty());
  CHECK(r.chatCount() == 0);
  CHECK(r.chatText(0).empty());
  CHECK(!r.screen().open);
  CHECK(r.screen().name.empty());
  CHECK(r.revision() == 0);
}

static void TestItemIdValidation() {
  CHECK(GameReads::ValidItemId("minecraft:diamond_sword"));
  CHECK(GameReads::ValidItemId("stone"));
  CHECK(!GameReads::ValidItemId(""));                      // empty
  CHECK(!GameReads::ValidItemId(std::string(200, 'a')));   // over the cap
  CHECK(!GameReads::ValidItemId("bad\nid"));               // control byte
}

static void TestInventoryReads() {
  FakeInventory inv;
  inv.slots = {Stack("minecraft:stone", 64), ItemStack{}, Stack("minecraft:bread", 3)};
  inv.selected = 2;
  GameReads r;
  r.SetInventorySource(&inv);

  CHECK(r.slotCount() == 3);
  CHECK(!r.slot(0).empty);
  CHECK(r.slot(0).id == "minecraft:stone");
  CHECK(r.slot(0).count == 64);
  CHECK(r.slot(1).empty);                 // the empty slot stays empty
  CHECK(r.slot(99).empty);                // out of range reads empty
  CHECK(r.items().size() == 2);           // only non-empty stacks
  CHECK(r.selectedSlot() == 2);
  CHECK(r.countOf("minecraft:stone") == 64);
  CHECK(r.countOf("minecraft:bread") == 3);
  CHECK(r.countOf("minecraft:gold") == 0);
  CHECK(r.has("minecraft:stone", 64));
  CHECK(!r.has("minecraft:stone", 65));
  CHECK(r.has("minecraft:stone", 0));     // <=0 means "any"
  CHECK(!r.has("", 1));                   // a bad id can never be held
}

static void TestInventoryNormalization() {
  FakeInventory inv;
  inv.slots = {
      Stack("", 5),                      // no usable id -> empty
      Stack("minecraft:stick", 0),       // non-positive count -> empty
      Stack("minecraft:stick", -3),      // negative count -> empty
      Stack("minecraft:pickaxe", 1, 500, 250),  // damage past max -> clamped
      Stack("minecraft:pickaxe", 1, -5, 250),   // negative damage -> 0
      Stack("minecraft:pickaxe", 1, 10, -1),    // negative max -> 0
  };
  GameReads r;
  r.SetInventorySource(&inv);

  CHECK(r.slot(0).empty);
  CHECK(r.slot(1).empty);
  CHECK(r.slot(2).empty);
  CHECK(!r.slot(3).empty);
  CHECK(r.slot(3).damage == 250);        // clamped to maxDamage
  CHECK(r.slot(3).maxDamage == 250);
  CHECK(r.slot(4).damage == 0);          // negative damage reads as 0
  CHECK(r.slot(5).maxDamage == 0);       // negative max reads as 0
  CHECK(r.items().size() == 3);          // the three pickaxe stacks survive
  CHECK(r.countOf("minecraft:stick") == 0);
}

static void TestInventoryWalkIsBounded() {
  FakeInventory inv;
  // A hostile source reports far more slots than the cap allows, each holding
  // the largest per-slot count, so the total would overflow a 32-bit sum.
  inv.slots.assign(GameReads::kMaxSlots + 500, Stack("minecraft:stone", 0x7FFFFFF0));
  GameReads r;
  r.SetInventorySource(&inv);
  CHECK(r.slotCount() == GameReads::kMaxSlots);
  CHECK(r.items().size() == GameReads::kMaxSlots);
  // A slot past the walked window reads empty even though the source has it.
  CHECK(r.slot(GameReads::kMaxSlots).empty);
  // The total is capped rather than wrapped into a negative number.
  CHECK(r.countOf("minecraft:stone") == 0x7FFFFFFF);
}

static void TestInventorySelectionOutsideWindow() {
  FakeInventory inv;
  inv.slots.assign(4, Stack("minecraft:stone", 1));
  inv.selected = 3;
  GameReads r;
  r.SetInventorySource(&inv);
  CHECK(r.selectedSlot() == 3);
  inv.selected = 99;                    // a selection the walk never covered
  CHECK(r.selectedSlot() == -1);
  inv.selected = -1;
  CHECK(r.selectedSlot() == -1);
}

static void TestReadFailureIsEmpty() {
  FakeInventory inv;
  inv.slots = {Stack("minecraft:stone", 1)};
  inv.failRead = true;
  GameReads r;
  r.SetInventorySource(&inv);
  CHECK(r.slot(0).empty);
  CHECK(r.items().empty());
  CHECK(r.countOf("minecraft:stone") == 0);
}

static void TestServerReads() {
  FakeServer srv;
  srv.info.connected = true;
  srv.info.host = "play.example.com";
  srv.info.port = 19132;
  srv.info.version = "1.21.0";
  srv.info.protocol = 685;
  srv.info.playerCount = 7;
  srv.info.maxPlayers = 20;
  srv.info.motd = "A server";
  srv.info.gamemode = "survival";
  srv.info.dimension = "overworld";
  srv.info.latencyMs = 42.5f;
  GameReads r;
  r.SetServerSource(&srv);

  const ServerInfo i = r.server();
  CHECK(i.connected);
  CHECK(i.host == "play.example.com");
  CHECK(i.port == 19132);
  CHECK(i.protocol == 685);
  CHECK(i.playerCount == 7);
  CHECK(i.maxPlayers == 20);
  CHECK(i.motd == "A server");
  CHECK(i.gamemode == "survival");
  CHECK(i.latencyMs > 42.0f && i.latencyMs < 43.0f);
}

static void TestServerNormalization() {
  FakeServer srv;
  srv.info.connected = true;
  srv.info.host = std::string(400, 'h');           // over the host cap
  srv.info.motd = std::string(2000, 'm');          // over the text cap
  srv.info.port = -1;
  srv.info.protocol = -5;
  srv.info.playerCount = 50;                       // more players than slots
  srv.info.maxPlayers = 20;
  srv.info.latencyMs = -1.0f;
  GameReads r;
  r.SetServerSource(&srv);

  const ServerInfo i = r.server();
  CHECK(i.host.empty());                           // refused, not truncated
  CHECK(i.motd.size() == GameReads::kMaxTextLen);  // bounded
  CHECK(i.port == 0);
  CHECK(i.protocol == 0);
  CHECK(i.playerCount == 20);                      // clamped to maxPlayers
  CHECK(i.latencyMs == 0.0f);
}

static void TestServerDisconnectedFromSource() {
  FakeServer srv;                                  // connected == false
  srv.info.host = "should-not-show";
  GameReads r;
  r.SetServerSource(&srv);
  const ServerInfo i = r.server();
  CHECK(!i.connected);
  CHECK(i.host.empty());                           // a disconnected read is inert
}

static void TestChatReads() {
  FakeChat chat;
  ChatMessage a;
  a.sender = "Steve";
  a.text = "hello";
  a.atMs = 1000;
  ChatMessage b;
  b.text = "joined the game";
  b.system = true;
  b.atMs = 2000;
  chat.lines = {a, b};
  GameReads r;
  r.SetChatSource(&chat);

  const std::vector<ChatMessage> msgs = r.chat();
  CHECK(msgs.size() == 2);
  CHECK(msgs[0].sender == "Steve");
  CHECK(msgs[0].text == "hello");
  CHECK(!msgs[0].system);
  CHECK(msgs[1].system);
  CHECK(r.chatCount() == 2);
  CHECK(r.chatText(0) == "hello");
  CHECK(r.chatText(1) == "joined the game");
  CHECK(r.chatText(9).empty());                    // out of range
}

static void TestChatNormalization() {
  FakeChat chat;
  ChatMessage bad;
  bad.sender = "bad\nsender";                      // a control byte -> no sender
  bad.text = "ok";
  ChatMessage longLine;
  longLine.sender = "Steve";
  longLine.text = std::string(4000, 'x');          // over the text cap
  chat.lines = {bad, longLine};
  GameReads r;
  r.SetChatSource(&chat);

  const std::vector<ChatMessage> msgs = r.chat();
  CHECK(msgs[0].sender.empty());                   // refused, not truncated
  CHECK(msgs[1].text.size() == GameReads::kMaxTextLen);
}

static void TestChatHistoryIsBounded() {
  FakeChat chat;
  chat.lines.assign(GameReads::kMaxChat + 50, ChatMessage{});
  GameReads r;
  r.SetChatSource(&chat);
  CHECK(r.chat().size() == GameReads::kMaxChat);
  CHECK(r.chatCount() == GameReads::kMaxChat);
  CHECK(r.chatText(GameReads::kMaxChat).empty());  // past the cap
}

static void TestScreenReads() {
  FakeScreen scr;
  scr.state.open = true;
  scr.state.name = "chat";
  scr.state.title = "Chat";
  scr.state.width = 1280;
  scr.state.height = 720;
  scr.state.keyboardOpen = true;
  GameReads r;
  r.SetScreenSource(&scr);

  const ScreenState s = r.screen();
  CHECK(s.open);
  CHECK(s.name == "chat");
  CHECK(s.title == "Chat");
  CHECK(s.width == 1280);
  CHECK(s.height == 720);
  CHECK(s.keyboardOpen);

  scr.state.open = false;
  CHECK(!r.screen().open);                         // a closed source is inert
  CHECK(r.screen().name.empty());
}

static void TestScreenNormalization() {
  FakeScreen scr;
  scr.state.open = true;
  scr.state.name = "bad\x01name";                  // control byte -> no name
  scr.state.title = std::string(4000, 't');
  scr.state.width = -5.0f;
  scr.state.height = -1.0f;
  GameReads r;
  r.SetScreenSource(&scr);

  const ScreenState s = r.screen();
  CHECK(s.open);
  CHECK(s.name.empty());
  CHECK(s.title.size() == GameReads::kMaxTextLen);
  CHECK(s.width == 0.0f);
  CHECK(s.height == 0.0f);
}

static void TestRevisionBumpsOnSwap() {
  GameReads r;
  const u64 before = r.revision();
  FakeInventory inv;
  r.SetInventorySource(&inv);
  const u64 after = r.revision();
  CHECK(after > before);
  FakeChat chat;
  r.SetChatSource(&chat);
  CHECK(r.revision() > after);
}

static void TestSourcesAreIndependent() {
  FakeInventory inv;
  inv.slots = {Stack("minecraft:stone", 1)};
  FakeServer srv;
  srv.info.connected = true;
  srv.info.host = "h";
  GameReads r;
  r.SetInventorySource(&inv);
  r.SetServerSource(&srv);
  // Binding one source must not disturb another's reads.
  CHECK(r.slotCount() == 1);
  CHECK(r.server().host == "h");
  r.SetChatSource(nullptr);                        // clearing one is fine
  CHECK(r.slotCount() == 1);
  CHECK(r.server().connected);
}

int main() {
  TestFailsClosedWithoutSources();
  TestItemIdValidation();
  TestInventoryReads();
  TestInventoryNormalization();
  TestInventoryWalkIsBounded();
  TestInventorySelectionOutsideWindow();
  TestReadFailureIsEmpty();
  TestServerReads();
  TestServerNormalization();
  TestServerDisconnectedFromSource();
  TestChatReads();
  TestChatNormalization();
  TestChatHistoryIsBounded();
  TestScreenReads();
  TestScreenNormalization();
  TestRevisionBumpsOnSwap();
  TestSourcesAreIndependent();
  if (g_fail == 0) printf("test_gamereads: ALL OK\n");
  return g_fail;
}
