// Host tests for the client's LAN wiring (Phase 4/5).
//
// The client is the production composition root: `BuildLan` opens the multicast
// transport, the mDNS discovery and the sync channel from config; the frame
// hook polls and applies; and a changed local loadout is published with no
// script in the picture. These tests drive the real engine so the whole path is
// exercised end to end, with a test-side peer standing in for the remote client:
//
//   * a client configured with sync + a verify key builds a channel and
//     discovery, and reports them through the client accessors;
//   * the frame hook applies a verified remote loadout into the client's own
//     cosmetic service (the production target), and refuses a bad one;
//   * the frame hook publishes the local loadout on a change;
//   * a client with sync off builds no channel and polls nothing (fail closed).
#include "berry/client.h"
#include "berry/cosmetic.h"
#include "berry/cosmetic_relay.h"
#include "berry/cosmetic_relay_transport.h"
#include "berry/cosmetic_sync.h"
#include "berry/engine.h"
#include "berry/manifest.h"
#include "berry/voice_crypto.h"
#include "berry/voice_relay.h"
#include "berry/voice_relay_transport.h"
#include "berry/lan_discovery.h"
#include "berry/lan_transport.h"
#include "berry/script.h"
#include "berry/services.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

// Runs one binding through a real host so the capability gate is exercised
// exactly as it is on device.
ScriptStatus CallBinding(ScriptBindings& bindings, const std::string& name,
                         const std::vector<std::string>& grants,
                         ScriptValue* out,
                         const std::vector<ScriptValue>& args = {}) {
  CapabilitySet caps;
  for (const auto& g : grants) caps.Add(g);
  ScriptHost host("client-lan-test", caps, nullptr);
  host.SetBindings(&bindings);
  return host.CallBinding(name, args, out);
}

const char* kGroup = "239.66.0.1";
const u16 kPort = 47823;

// A tiny Luau module source; the client's loader compiles with the real Luau
// VM, so the source must be valid Luau (a module returning a table).
const char* kScriptSrc =
    "local M = {}\n"
    "function M.on_load() return 0 end\n"
    "return M\n";

/// A unique temp directory for one test, removed by the caller.
std::string MakeTempDir(const char* tag) {
  char tmpl[160];
  std::snprintf(tmpl, sizeof tmpl, "/tmp/berry-client-%s-XXXXXX", tag);
  const char* d = mkdtemp(tmpl);
  return d ? std::string(d) : std::string();
}

/// A deterministic 32-byte seed for a voice identity (as the relay test uses).
std::string Seed(u8 base) {
  std::string s(32, '\0');
  for (size_t i = 0; i < s.size(); ++i) s[i] = (char)(u8)(base + i * 11);
  return s;
}

void WriteFileRaw(const std::string& path, const std::string& data) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fwrite(data.data(), 1, data.size(), f);
  std::fclose(f);
}

/// Creates a script directory (unsigned; the client runs it in dev mode) whose
/// manifest names `name`, and returns the directory.
std::string MakeScriptDir(const std::string& root, const std::string& name) {
  const std::string dir = root + "/script";
  std::string cmd = "mkdir -p '" + dir + "'";
  if (system(cmd.c_str()) != 0) return std::string();
  WriteFileRaw(dir + "/manifest.json",
               "{\"name\":\"" + name + "\",\"version\":\"1.0.0\","
               "\"entry\":\"main.luau\",\"capabilities\":[\"fileio\"]}");
  WriteFileRaw(dir + "/main.luau", kScriptSrc);
  return dir;
}

// The default config with the LAN sync channel enabled on loopback. HMAC keeps
// the test free of a keypair; the client parses it into the channel's key.
BerryConfigC SyncConfig() {
  BerryConfigC cfg{};
  cfg.size = sizeof(cfg);
  cfg.abi_version = BERRY_ABI_VERSION;
  cfg.auto_attach_modules = 1;
  cfg.enable_render = 1;
  cfg.enable_client = 1;
  cfg.enable_cosmetic_sync = 1;
  cfg.sync_verify_key = "shared";
  cfg.sync_use_hmac = 1;
  cfg.sync_self_id = "me";
  cfg.sync_sign_secret = "shared";
  cfg.lan_iface = "127.0.0.1";
  return cfg;
}

// A test-side peer on the same loopback group, standing in for a remote client.
struct Peer {
  LanCosmeticTransport transport;
  CosmeticSyncChannel channel;

  bool Open() {
    LanTransportConfig tcfg;
    tcfg.group = kGroup;
    tcfg.port = kPort;
    tcfg.iface = "127.0.0.1";
    if (!transport.Open(tcfg)) return false;
    // The peer verifies what it receives, so it holds the shared key too.
    channel.SetVerifyKey(CosmeticSyncKey::HmacSecret("shared"));
    channel.SetTransport(&transport);
    return true;
  }

  // Publishes a signed loadout for `id` on the shared secret.
  bool Publish(const std::string& id, const CosmeticLoadout& lo) {
    return channel.Publish(id, lo, "shared", /*useHmac=*/true);
  }
};

}  // namespace

int main() {
  // The built-in registry gives `builtin.cape.red` a real asset to equip and
  // own, so the client's ownership gate admits a verified remote loadout.
  CosmeticRegistry::Instance().Clear();
  CosmeticRegistry::Instance().RegisterBuiltins();
  CosmeticOwnership::Instance().Grant("alice", "builtin.cape.red");

  //--- a client with sync off has no channel and polls nothing ---
  {
    Engine& eng = Engine::Instance();
    CHECK(eng.Bootstrap());
    CHECK(eng.Attach());
    BerryClient* client = eng.Client();
    CHECK(client != nullptr);
    CHECK(client->cosmeticSync() == nullptr);
    CHECK(client->discovery() == nullptr);
    client->BeginFrame(16.0f, Vec2(1280, 720));   // a no-op poll
    client->EndFrame();
    // The built-in clips are authored at attach, so a built-in cape/aura
    // animates without a script or a pack. The client's own animator owns them,
    // and the animation seam samples from that same store.
    CHECK(client->animator().hasClip("builtin.cape.flap"));
    CHECK(client->animator().hasClip("builtin.aura.spin"));
    CHECK(client->cosmetics().animationSource() != nullptr);
    eng.Detach();
  }

  //--- a client with sync on builds the channel + discovery from config ---
  {
    Engine& eng = Engine::Instance();
    BerryConfigC cfg = SyncConfig();
    CHECK(eng.Bootstrap(cfg));
    CHECK(eng.Attach());
    BerryClient* client = eng.Client();
    CHECK(client != nullptr);
    CHECK(client->cosmeticSync() != nullptr);
    CHECK(client->cosmeticSync()->configured());
    CHECK(client->discovery() != nullptr);

    //--- the frame hook applies a verified remote loadout ---
    Peer alice;
    CHECK(alice.Open());
    CosmeticLoadout remote;
    CHECK(remote.Equip("builtin.cape.red"));
    CHECK(alice.Publish("alice", remote));

    bool applied = false;
    for (int i = 0; i < 80 && !applied; ++i) {
      // BeginFrame drains and applies through the production poll path.
      client->BeginFrame(16.0f, Vec2(1280, 720));
      client->EndFrame();
      applied = !client->cosmetics().RemotePlayers().empty();
    }
    CHECK(applied);
    CHECK(client->cosmeticSync()->accepted() >= 1);

    //--- a bad signature is refused, not applied ---
    {
      CosmeticSyncChannel forged;
      forged.SetTransport(&alice.transport);
      CosmeticLoadout bad;
      CHECK(bad.Equip("builtin.cape.red"));
      // Signed with a different secret: the client's key will not verify it.
      CHECK(forged.Publish("mallory", bad, "not-shared", /*useHmac=*/true));
      const u64 rejectedBefore = client->cosmeticSync()->rejected();
      for (int i = 0; i < 40; ++i) {
        client->BeginFrame(16.0f, Vec2(1280, 720));
        client->EndFrame();
        if (client->cosmeticSync()->rejected() > rejectedBefore) break;
      }
      CHECK(client->cosmeticSync()->rejected() > rejectedBefore);
    }

    //--- the frame hook publishes the local loadout on a change ---
    CHECK(client->cosmetics().Equip("builtin.cape.red"));
    bool gotLocal = false;
    for (int i = 0; i < 80 && !gotLocal; ++i) {
      client->BeginFrame(16.0f, Vec2(1280, 720));
      client->EndFrame();
      alice.transport.Poll([&](const std::string& m, const std::string&, u16) {
        if (gotLocal) return;
        CosmeticSyncMessage msg;
        if (alice.channel.Verify(m, &msg) == CosmeticSyncResult::Ok &&
            msg.playerId == "me") {
          gotLocal = true;
        }
      }, 10);
    }
    CHECK(gotLocal);

    alice.transport.Close();
    eng.Detach();
  }

  //--- discovery: a peer on the subnet becomes visible to the client's own
  //    frame poll, and `cosmetic.peers` reports it (the production caller) ---
  {
    Engine& eng = Engine::Instance();
    BerryConfigC cfg = SyncConfig();
    CHECK(eng.Bootstrap(cfg));
    CHECK(eng.Attach());
    BerryClient* client = eng.Client();
    CHECK(client != nullptr);
    CHECK(client->discovery() != nullptr);

    // A remote client publishes itself over mDNS on loopback.
    LanDiscovery peer;
    LanDiscoveryConfig pcfg;
    pcfg.iface = "127.0.0.1";
    CHECK(peer.Open(pcfg));
    CHECK(peer.Publish("remote-peer", 47999, "sync=1"));

    // The client's frame poll browses: it answers the peer's query with its own
    // publish and drains the reply. Drive both sides until the client lists it.
    bool found = false;
    for (int i = 0; i < 60 && !found; ++i) {
      peer.Query();
      peer.Poll(5);
      client->BeginFrame(16.0f, Vec2(1280, 720));
      client->EndFrame();
      for (const LanService& s : client->discovery()->services()) {
        if (s.instance.find("remote-peer") != std::string::npos) found = true;
      }
    }
    CHECK(found);

    // The binding reads the same cache, so a script sees the peer too.
    ScriptValue out;
    CHECK(CallBinding(client->bindings(), "cosmetic.peers", {"cosmetic"}, &out) ==
          ScriptStatus::Ok);
    CHECK(out.AsNumber() >= 1);

    peer.Close();
    eng.Detach();
  }

  //--- script data persistence: the fs store binds to a per-script dir under
  //    the configured root, and a restart sees the same files (Phase E) ---
  {
    const std::string root = MakeTempDir("data");
    CHECK(!root.empty());
    if (!root.empty()) {
      const std::string scriptDir = MakeScriptDir(root, "persister");
      CHECK(!scriptDir.empty());
      const std::string dataRoot = root + "/data";

      // First run: the client loads the script and binds the store, and a
      // script write lands on disk.
      {
        Engine& eng = Engine::Instance();
        BerryConfigC cfg = BerryConfigC();
        cfg.size = sizeof(cfg);
        cfg.abi_version = BERRY_ABI_VERSION;
        cfg.auto_attach_modules = 1;
        cfg.enable_render = 1;
        cfg.enable_client = 1;
        cfg.dev_mode = 1;                 // unsigned script for the test
        cfg.script_directory = scriptDir.c_str();
        cfg.script_data_directory = dataRoot.c_str();
        CHECK(eng.Bootstrap(cfg));
        CHECK(eng.Attach());
        BerryClient* client = eng.Client();
        CHECK(client != nullptr);
        CHECK(client->files().persistent());

        ScriptValue out;
        CHECK(CallBinding(client->bindings(), "fs.write", {"fileio"}, &out,
                          {ScriptValue::Str("save.dat"),
                           ScriptValue::Str("level-9")}) == ScriptStatus::Ok);
        CHECK(out.Truthy());
        eng.Detach();
      }

      // Second run: a fresh engine over the same root reads the data back.
      {
        Engine& eng = Engine::Instance();
        BerryConfigC cfg = BerryConfigC();
        cfg.size = sizeof(cfg);
        cfg.abi_version = BERRY_ABI_VERSION;
        cfg.auto_attach_modules = 1;
        cfg.enable_render = 1;
        cfg.enable_client = 1;
        cfg.dev_mode = 1;
        cfg.script_directory = scriptDir.c_str();
        cfg.script_data_directory = dataRoot.c_str();
        CHECK(eng.Bootstrap(cfg));
        CHECK(eng.Attach());
        BerryClient* client = eng.Client();
        CHECK(client != nullptr);
        CHECK(client->files().persistent());
        CHECK(client->files().FileCount() == 1);
        ScriptValue out;
        CHECK(CallBinding(client->bindings(), "fs.read", {"fileio"}, &out,
                          {ScriptValue::Str("save.dat")}) == ScriptStatus::Ok);
        CHECK(out.AsString() == "level-9");
        eng.Detach();
      }

      // A client with no data directory stays in-memory.
      {
        Engine& eng = Engine::Instance();
        CHECK(eng.Bootstrap());
        CHECK(eng.Attach());
        BerryClient* client = eng.Client();
        CHECK(client != nullptr);
        CHECK(!client->files().persistent());
        eng.Detach();
      }

      std::string cmd = "rm -rf '" + root + "'";
      if (system(cmd.c_str()) != 0) { /* best-effort cleanup */ }
    }
  }

  //--- the presence backend: a client configured with a relay host builds the
  //    relay transport (not the LAN one), no discovery, and a verified remote
  //    loadout arrives through the relay and is applied (two networks) ---
  {
    // A relay room, as a backend would host it.
    CosmeticRelay relay;
    const std::string roomSecret = "0123456789abcdef0123456789abcdef";
    CHECK(relay.AddRoom("lobby", roomSecret));
    CHECK(relay.Open(CosmeticRelayConfig{}));  // binds an ephemeral port
    const u16 relayPort = relay.localPort();
    CHECK(relayPort > 0);

    Engine& eng = Engine::Instance();
    BerryConfigC cfg = SyncConfig();
    // Swap the LAN transport for the relay path.
    cfg.sync_relay_host = "127.0.0.1";
    cfg.sync_relay_port = relayPort;
    cfg.sync_room = "lobby";
    cfg.sync_room_secret = roomSecret.c_str();
    CHECK(eng.Bootstrap(cfg));
    CHECK(eng.Attach());
    BerryClient* client = eng.Client();
    CHECK(client != nullptr);
    CHECK(client->cosmeticSync() != nullptr);
    CHECK(client->syncRelay() != nullptr);   // the relay transport, not LAN
    CHECK(client->discovery() == nullptr);   // LAN-only, not built for relay

    // A remote client reaches the same room through its own relay transport and
    // publishes alice's loadout, which the client's frame poll applies.
    CosmeticRelayTransport alice;
    RelayCosmeticConfig acfg;
    acfg.relayHost = "127.0.0.1";
    acfg.relayPort = relayPort;
    acfg.room = "lobby";
    acfg.roomSecret = roomSecret;
    acfg.bindAddr = "127.0.0.1";
    acfg.registerIntervalMs = 100;
    CHECK(alice.Open(acfg));
    CosmeticSyncChannel aliceChan;
    aliceChan.SetVerifyKey(CosmeticSyncKey::HmacSecret("shared"));
    aliceChan.SetTransport(&alice);
    CosmeticLoadout remote;
    CHECK(remote.Equip("builtin.cape.red"));
    CHECK(aliceChan.Publish("alice", remote, "shared", /*useHmac=*/true));

    bool applied = false;
    for (int i = 0; i < 200 && !applied; ++i) {
      alice.Poll(nullptr, 5);
      relay.Pump(5);
      client->BeginFrame(16.0f, Vec2(1280, 720));
      client->EndFrame();
      applied = !client->cosmetics().RemotePlayers().empty();
    }
    CHECK(applied);
    CHECK(client->cosmeticSync()->accepted() >= 1);

    alice.Close();
    eng.Detach();
    relay.Close();
  }

  //--- the script event bus, driven through the real client: on_load runs on
  //    the worker and subscribes through the production path, and an engine
  //    event is delivered to the script's handler on a later tick's pump.
  //    (The handler's payload delivery is asserted against a real Luau VM in
  //    test_script_bus; here the client wiring — subscribe on load, pump on
  //    tick, drain the queue — is what is exercised.) ---
  {
    const std::string root = MakeTempDir("bus");
    CHECK(!root.empty());
    if (!root.empty()) {
      const std::string dir = root + "/script";
      std::string cmd = "mkdir -p '" + dir + "'";
      CHECK(system(cmd.c_str()) == 0);
      WriteFileRaw(dir + "/manifest.json",
                   "{\"name\":\"bus-script\",\"version\":\"1.0.0\","
                   "\"entry\":\"main.luau\",\"capabilities\":[\"events\"]}");
      // on_load subscribes to a declared channel and to a script-only one.
      WriteFileRaw(dir + "/main.luau",
          "local M = {}\n"
          "function M.on_load()\n"
          "  events.on(\"combatConfirmed\", \"on_combat\")\n"
          "  events.on(\"myCustom\", \"on_custom\")\n"
          "  return 0\n"
          "end\n"
          "function M.on_combat(payload) M.last = payload return 0 end\n"
          "function M.on_custom(payload) M.last = payload return 0 end\n"
          "return M\n");

      Engine& eng = Engine::Instance();
      BerryConfigC cfg = BerryConfigC();
      cfg.size = sizeof(cfg);
      cfg.abi_version = BERRY_ABI_VERSION;
      cfg.auto_attach_modules = 1;
      cfg.enable_render = 1;
      cfg.enable_client = 1;
      cfg.dev_mode = 1;                 // unsigned script for the test
      cfg.script_directory = dir.c_str();
      CHECK(eng.Bootstrap(cfg));
      CHECK(eng.Attach());
      BerryClient* client = eng.Client();
      CHECK(client != nullptr);

      ScriptEventBus& bus = client->api().bus();
      // on_load is posted to the worker at attach; drive ticks until it has run
      // and the two subscriptions exist (bounded wait, not a blind sleep).
      bool subscribed = false;
      for (int i = 0; i < 200 && !subscribed; ++i) {
        eng.OnGameTick((GameTick)i);
        subscribed = bus.CountFor("bus-script") == 2;
        if (!subscribed) usleep(1000);
      }
      CHECK(subscribed);
      // The declared channel is bridged on the engine bus.
      CHECK(EventBus::Instance().HasListeners("combatConfirmed"));

      // An engine event is queued by the listener and delivered on a later
      // tick's pump. `delivered()` proves the handler ran.
      const u64 before = bus.delivered();
      const char* payload = "hit-42";
      EventBus::Instance().Emit("combatConfirmed", (void*)&payload);
      bool delivered = false;
      for (int i = 200; i < 400 && !delivered; ++i) {
        eng.OnGameTick((GameTick)i);
        delivered = bus.delivered() > before;
        if (!delivered) usleep(1000);
      }
      CHECK(delivered);
      CHECK(bus.queued() == 0);

      // Detach must drop the engine listener with the script's subscriptions.
      eng.Detach();
      CHECK(!EventBus::Instance().HasListeners("combatConfirmed"));

      std::string rm = "rm -rf '" + root + "'";
      if (system(rm.c_str()) != 0) { /* best-effort cleanup */ }
    }
  }

  //--- voice selects the relay media plane from config (F2) ---
  {
    // A real relay stands in for the hosted forwarder. The client registers its
    // room and, when a relay host is configured, must build the relay transport
    // instead of the LAN multicast one.
    VoiceRelay relay;
    const std::string roomSecret(32, 'k');
    CHECK(relay.AddRoom("room-1", roomSecret));
    VoiceRelayConfig rcfg;
    rcfg.port = 0;
    rcfg.bindAddr = "127.0.0.1";
    CHECK(relay.Open(rcfg));
    const u16 relayPort = relay.localPort();
    CHECK(relayPort > 0);

    // Two Ed25519 identities: self (raw seed as hex, since the C ABI carries a
    // C string and a raw 32-byte seed can hold a NUL) and one trusted peer, so
    // the anchor is valid and voice is admitted.
    const VoiceIdentity self = VoiceIdentity::FromSeed(Seed(1));
    const VoiceIdentity peer = VoiceIdentity::FromSeed(Seed(2));

    Engine& eng = Engine::Instance();
    BerryConfigC cfg{};
    cfg.size = sizeof(cfg);
    cfg.abi_version = BERRY_ABI_VERSION;
    cfg.auto_attach_modules = 1;
    cfg.enable_render = 1;
    cfg.enable_client = 1;
    cfg.enable_voice = 1;
    cfg.voice_self_id = "me";
    const std::string seedHex = crypto::ToHexLower(Seed(1));
    cfg.voice_identity_seed = seedHex.c_str();
    const std::string peerKey = peer.publicKeyHex();
    cfg.voice_peer_keys = peerKey.c_str();
    cfg.voice_relay_host = "127.0.0.1";
    cfg.voice_relay_port = relayPort;
    cfg.voice_room = "room-1";
    cfg.voice_room_secret = roomSecret.c_str();
    CHECK(eng.Bootstrap(cfg));
    CHECK(eng.Attach());
    BerryClient* client = eng.Client();
    CHECK(client != nullptr);
    // The relay transport is the active one; the LAN transport was not built.
    CHECK(client->voiceRelayTransport() != nullptr);
    CHECK(client->secureVoiceTransport() == nullptr);
    CHECK(client->voiceTransport() == client->voiceRelayTransport());
    CHECK(client->voiceRelayTransport()->open());
    // The client is talking: the voice session started with the configured id.
    CHECK(client->voice().running());
    eng.Detach();
    // Detach tears the relay down; the next block proves the LAN path is still
    // selectable when no relay host is set.
    CHECK(client->voiceRelayTransport() == nullptr);

    //--- without a relay host the LAN secure transport is selected ---
    Engine& eng2 = Engine::Instance();
    BerryConfigC lan{};
    lan.size = sizeof(lan);
    lan.abi_version = BERRY_ABI_VERSION;
    lan.auto_attach_modules = 1;
    lan.enable_render = 1;
    lan.enable_client = 1;
    lan.enable_voice = 1;
    lan.voice_self_id = "me";
    lan.voice_identity_seed = seedHex.c_str();
    lan.voice_peer_keys = peerKey.c_str();
    lan.voice_group = kGroup;
    lan.voice_port = kPort;
    lan.lan_iface = "127.0.0.1";
    CHECK(eng2.Bootstrap(lan));
    CHECK(eng2.Attach());
    BerryClient* lanClient = eng2.Client();
    CHECK(lanClient != nullptr);
    CHECK(lanClient->voiceRelayTransport() == nullptr);
    CHECK(lanClient->secureVoiceTransport() != nullptr);
    CHECK(lanClient->voiceTransport() == lanClient->secureVoiceTransport());
    eng2.Detach();

    relay.Close();
    (void)self;
  }

  if (g_fail == 0) printf("test_client_lan: ALL OK\n");
  return g_fail;
}
