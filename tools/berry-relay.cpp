// berry-relay: a standalone host for the proximity-voice UDP forwarder.
//
// It is the process an operator runs to let two phones on different networks
// hear each other. It binds one UDP socket, registers a set of rooms (each with
// a secret), and forwards each registered peer's *already-encrypted* datagrams
// to the room's other members. It holds no key and never decrypts, so it cannot
// read or forge the audio; see include/berry/voice_relay.h for the envelope and
// the auth scheme.
//
// Usage:
//   berry-relay --port 47825 --room party=<64-hex-secret> [--room ...]
//   berry-relay --port 47825 --secret-file /etc/berry/rooms
//   berry-relay --print-room-tag party <64-hex-secret>
//
// A secret on the command line is visible in `ps`, so a production deployment
// should use --secret-file (mode 0600, one `name:hex-secret` per line) or the
// BERRY_RELAY_SECRET environment variable. The relay prints a room's tag with
// --print-room-tag, which is the value a client derives (VoiceRelay::RoomTagHex)
// and the token issuer embeds; it is safe to print (it is a public lookup key,
// not the secret).
//
// Exit codes: 0 clean shutdown (SIGINT/SIGTERM), 2 a configuration error.

#include "berry/logger.h"
#include "berry/manifest.h"
#include "berry/voice_relay.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;
void OnSignal(int) { g_stop = 1; }

struct RoomSpec {
  std::string name;
  std::string secretHex;
};

void Usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s --port <n> [--room <name>=<hex-secret>]...\n"
          "       %s --port <n> --secret-file <path>\n"
          "       %s --print-room-tag <name> <hex-secret>\n"
          "\n"
          "  --port <n>            UDP port to bind (default 47825)\n"
          "  --bind <addr>         address to bind (default 0.0.0.0)\n"
          "  --room <name>=<hex>   register a room (repeatable)\n"
          "  --secret-file <path>  read `name:hex-secret` lines (mode 0600)\n"
          "  --mtu <n>             max datagram to forward (default 1400)\n"
          "  --ttl-ms <n>          peer registration TTL (default 15000)\n"
          "  --print-room-tag <name> <hex-secret>   print the room tag and exit\n"
          "\n"
          "The secret must be at least 16 bytes (32 hex chars). Prefer\n"
          "--secret-file or BERRY_RELAY_SECRET over --room: a secret on the\n"
          "command line is visible in `ps`.\n",
          argv0, argv0, argv0);
}

/// Splits "name=hex" into its two parts; false when there is no '='.
bool SplitRoom(const std::string& arg, RoomSpec* out) {
  const size_t eq = arg.find('=');
  if (eq == std::string::npos || eq == 0 || eq + 1 >= arg.size()) return false;
  out->name = arg.substr(0, eq);
  out->secretHex = arg.substr(eq + 1);
  return true;
}

/// Reads `name:hex-secret` lines, ignoring blanks and `#` comments.
bool ReadSecretFile(const std::string& path, std::vector<RoomSpec>* out,
                    std::string* error) {
  FILE* f = fopen(path.c_str(), "rb");
  if (f == nullptr) {
    *error = "cannot open " + path;
    return false;
  }
  char line[1024];
  while (fgets(line, sizeof line, f) != nullptr) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (s.empty() || s[0] == '#') continue;
    const size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0) continue;
    RoomSpec r;
    r.name = s.substr(0, colon);
    r.secretHex = s.substr(colon + 1);
    out->push_back(r);
  }
  fclose(f);
  return true;
}

/// The BERRY_RELAY_SECRET variable holds `name:hex` pairs separated by ';'.
bool ReadSecretEnv(std::vector<RoomSpec>* out) {
  const char* env = getenv("BERRY_RELAY_SECRET");
  if (env == nullptr || *env == '\0') return false;
  std::string s(env);
  size_t start = 0;
  while (start <= s.size()) {
    const size_t semi = s.find(';', start);
    const std::string item =
        s.substr(start, semi == std::string::npos ? std::string::npos
                                                  : semi - start);
    const size_t colon = item.find(':');
    if (colon != std::string::npos && colon > 0) {
      RoomSpec r;
      r.name = item.substr(0, colon);
      r.secretHex = item.substr(colon + 1);
      out->push_back(r);
    }
    if (semi == std::string::npos) break;
    start = semi + 1;
  }
  return !out->empty();
}

}  // namespace

int main(int argc, char** argv) {
  Berry::VoiceRelayConfig cfg;
  std::vector<RoomSpec> rooms;
  std::string secretFile;
  bool printTag = false;
  std::string printTagName, printTagSecret;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        fprintf(stderr, "berry-relay: %s needs a value\n", what);
        exit(2);
      }
      return argv[++i];
    };
    if (a == "--port") {
      cfg.port = (Berry::u16)strtoul(next("--port"), nullptr, 10);
    } else if (a == "--bind") {
      cfg.bindAddr = next("--bind");
    } else if (a == "--mtu") {
      cfg.mtu = (size_t)strtoul(next("--mtu"), nullptr, 10);
    } else if (a == "--ttl-ms") {
      cfg.peerTtlMs = strtoull(next("--ttl-ms"), nullptr, 10);
    } else if (a == "--secret-file") {
      secretFile = next("--secret-file");
    } else if (a == "--room") {
      RoomSpec r;
      if (!SplitRoom(next("--room"), &r)) {
        fprintf(stderr, "berry-relay: --room needs <name>=<hex-secret>\n");
        return 2;
      }
      rooms.push_back(r);
    } else if (a == "--print-room-tag") {
      printTag = true;
      printTagName = next("--print-room-tag");
      printTagSecret = next("--print-room-tag");
    } else if (a == "-h" || a == "--help") {
      Usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "berry-relay: unknown option '%s'\n", a.c_str());
      Usage(argv[0]);
      return 2;
    }
  }

  if (printTag) {
    std::string secret;
    if (!Berry::crypto::FromHex(printTagSecret, &secret) ||
        secret.size() < Berry::VoiceRelay::kMinSecretBytes) {
      fprintf(stderr, "berry-relay: secret must be >= %zu bytes\n",
              Berry::VoiceRelay::kMinSecretBytes);
      return 2;
    }
    printf("%s\n", Berry::VoiceRelay::RoomTagHex(printTagName, secret).c_str());
    return 0;
  }

  if (!secretFile.empty()) {
    std::string err;
    if (!ReadSecretFile(secretFile, &rooms, &err)) {
      fprintf(stderr, "berry-relay: %s\n", err.c_str());
      return 2;
    }
  }
  ReadSecretEnv(&rooms);  // adds any rooms from the environment

  if (rooms.empty()) {
    fprintf(stderr, "berry-relay: no rooms configured (see --help)\n");
    return 2;
  }

  Berry::VoiceRelay relay;
  for (const RoomSpec& r : rooms) {
    std::string secret;
    if (!Berry::crypto::FromHex(r.secretHex, &secret) ||
        secret.size() < Berry::VoiceRelay::kMinSecretBytes) {
      fprintf(stderr, "berry-relay: room '%s' secret must be >= %zu bytes\n",
              r.name.c_str(), Berry::VoiceRelay::kMinSecretBytes);
      return 2;
    }
    if (!relay.AddRoom(r.name, secret)) {
      fprintf(stderr, "berry-relay: room '%s' refused (duplicate?)\n",
              r.name.c_str());
      return 2;
    }
  }

  if (!relay.Open(cfg)) {
    fprintf(stderr, "berry-relay: cannot bind %s:%u\n", cfg.bindAddr.c_str(),
            (unsigned)cfg.port);
    return 2;
  }
  signal(SIGINT, OnSignal);
  signal(SIGTERM, OnSignal);

  printf("berry-relay: listening on %s:%u, %zu room(s)\n",
         cfg.bindAddr.c_str(), (unsigned)relay.localPort(), relay.roomCount());
  fflush(stdout);

  while (!g_stop) {
    relay.Pump(200);
  }

  const Berry::VoiceRelay::Stats s = relay.stats();
  printf("\nberry-relay: shutdown — %llu received, %llu forwarded, "
         "%llu registrations, %llu dropped, %llu peers evicted\n",
         (unsigned long long)s.datagramsReceived,
         (unsigned long long)s.datagramsForwarded,
         (unsigned long long)s.registrations,
         (unsigned long long)s.datagramsDropped,
         (unsigned long long)s.peersEvicted);
  relay.Close();
  return 0;
}
