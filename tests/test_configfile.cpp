// Host tests for the validated config file (improvement 89).
//
// Covers: a good document parses; a syntax error, an unknown key, a wrong type
// and an out-of-range port are each refused; a missing file is optional; env
// overrides apply only the allow-listed names.

#include "berry/configfile.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

int main() {
  std::string err;

  // --- a good document ---
  {
    ConfigFile cfg;
    CHECK(cfg.Parse(R"({
      "log_level": "warn",
      "capture_log": true,
      "dev_mode": true,
      "script_directory": "/srv/scripts",
      "script_anchor_key": "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff",
      "voice_port": 47825,
      "enable_voice": true
    })", &err));
    CHECK(cfg.present);
    CHECK(cfg.logLevel == "warn");
    CHECK(cfg.captureLog);
    CHECK(cfg.devMode);
    CHECK(cfg.scriptDirectory == "/srv/scripts");
    CHECK(cfg.voicePort == 47825);
    CHECK(cfg.enableVoice);
    // An absent key keeps its default.
    CHECK(cfg.enableRender);
    CHECK(!cfg.safeMode);
  }

  // --- an empty document is valid (all defaults) ---
  {
    ConfigFile cfg;
    CHECK(cfg.Parse("{}", &err));
    CHECK(cfg.present);
    CHECK(cfg.autoAttachModules);
  }

  // --- fail-closed refusals ---
  {
    ConfigFile cfg;
    CHECK(!cfg.Parse("{not json", &err));
    CHECK(!err.empty());

    CHECK(!cfg.Parse(R"({"bogus_key": 1})", &err));   // unknown key
    CHECK(err.find("bogus_key") != std::string::npos);

    CHECK(!cfg.Parse(R"({"log_level": "shout"})", &err));   // bad enum
    CHECK(!cfg.Parse(R"({"capture_log": "yes"})", &err));   // wrong type
    CHECK(!cfg.Parse(R"({"voice_port": 70000})", &err));    // out of range
    CHECK(!cfg.Parse(R"({"voice_port": 443.7})", &err));    // non-integer
    CHECK(!cfg.Parse(R"({"script_anchor_key": "abc"})", &err));  // bad hex
  }

  // --- a missing file is optional ---
  {
    ConfigFile cfg;
    CHECK(cfg.Load("/tmp/berry_definitely_absent_config.json", &err));
    CHECK(!cfg.present);
  }

  // --- a present file is parsed ---
  {
    const std::string path = "/tmp/berry_test_config.json";
    FILE* f = fopen(path.c_str(), "w");
    CHECK(f != nullptr);
    if (f) { fputs(R"({"log_level":"debug"})", f); fclose(f); }
    ConfigFile cfg;
    CHECK(cfg.Load(path, &err));
    CHECK(cfg.present);
    CHECK(cfg.logLevel == "debug");
    remove(path.c_str());
  }

  // --- env overrides: only the allow-list ---
  {
    ConfigFile cfg;
    CHECK(cfg.Parse("{}", &err));
    setenv("BERRY_SAFE_MODE", "1", 1);
    setenv("BERRY_LOG_LEVEL", "error", 1);
    setenv("BERRY_BOGUS", "x", 1);   // ignored
    int n = cfg.ApplyEnvOverrides();
    CHECK(cfg.safeMode);
    CHECK(cfg.logLevel == "error");
    CHECK(n >= 2);
    unsetenv("BERRY_SAFE_MODE");
    unsetenv("BERRY_LOG_LEVEL");
    unsetenv("BERRY_BOGUS");
  }

  if (g_fail == 0) printf("test_configfile: ALL OK\n");
  return g_fail;
}
