// berry-run: the developer CLI for the scripting layer.
//
// Three modes, all host-side and device-free:
//
//   berry-run test <file-or-dir> [options]
//       Discovers and runs a script's tests through `ScriptTestRunner`. The
//       tests are compiled with the same bindings a real script gets (the IDL
//       surface + the async layer), so a test exercises the real API, and a
//       binding that is unbound reports "service unavailable" exactly as it
//       would in the game. Exit code is 0 only when every test passes (an
//       expected failure counts as a pass, a stale `xfail_` does not).
//
//   berry-run run <script-dir> [options]
//       Loads a script directory through the one verified path (`ScriptLoader`:
//       manifest signature + source hash, unless --dev), builds the real client
//       host and drives it headlessly for a number of ticks and frames. This is
//       the "does my script load and tick" loop, with the loader's refusal
//       reason printed verbatim when it does not.
//
//   berry-run replay <file> [options]
//       Reads a replay container and prints its fingerprint, section index and
//       record histogram. Untrusted input goes through the same bounded reader
//       the client uses, so a malformed file is refused rather than parsed
//       optimistically.
//
// A fourth, `berry-run version`, prints the library/ABI/script-API versions.
//
// Exit codes: 0 success, 1 a test or load failed, 2 a usage/config error.

#include "berry/engine.h"
#include "berry/client.h"
#include "berry/generated_bindings.h"   // api::ScriptApiVersion
#include "berry/loader.h"
#include "berry/manifest.h"
#include "berry/replay_format.h"
#include "berry/script.h"
#include "berry/scripttest.h"
#include "berry/thread.h"
#include "berry/version.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <dirent.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace {

using namespace Berry;

//----------------------------------------------------------------------------
// Small helpers.
//----------------------------------------------------------------------------

/// Every capability kind a `--cap` argument may name, in the taxonomy's order.
const CapKind kAllKinds[] = {
    CapKind::Network, CapKind::Render,   CapKind::FileIO,  CapKind::World,
    CapKind::Combat,  CapKind::Input,    CapKind::Hud,     CapKind::Perf,
    CapKind::Events,  CapKind::Audio,    CapKind::Cosmetic, CapKind::Packets,
    CapKind::Voice,   CapKind::Crystal,  CapKind::GameReads,
};

/// The full grantable set. A test runner needs every kind a suite might touch,
/// because the runner is testing the suite, not the gate (the gate has its own
/// tests); the CLI still prints what it granted so the choice is visible.
CapabilitySet AllCapabilities() {
  CapabilitySet s;
  for (CapKind k : kAllKinds) s.Add(std::string(CapKindName(k)));
  return s;
}

bool IsDirectory(const std::string& path) {
#if defined(_WIN32)
  DWORD a = GetFileAttributesA(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool ReadWholeFile(const std::string& path, std::string* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::string data;
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
  const bool bad = std::ferror(f) != 0;
  std::fclose(f);
  if (bad) return false;
  *out = std::move(data);
  return true;
}

/// Lists `*.luau` files directly under `dir`, sorted. The test mode uses it so a
/// directory of suites runs without the caller naming each file.
std::vector<std::string> ListLuauFiles(const std::string& dir) {
  std::vector<std::string> out;
#if defined(_WIN32)
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((dir + "\\*.luau").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) return out;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    const std::string name = e->d_name;
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || name.substr(dot) != ".luau") continue;
    struct stat st;
    const std::string full = dir + "/" + name;
    if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) continue;
    out.push_back(name);
  }
  closedir(d);
#endif
  std::sort(out.begin(), out.end());
  return out;
}

/// True when `path` names a regular file (not a directory / missing).
bool IsFile(const std::string& path) {
#if defined(_WIN32)
  DWORD a = GetFileAttributesA(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

void SleepMs(u64 ms) {
#if defined(_WIN32)
  Sleep((DWORD)ms);
#else
  usleep((useconds_t)(ms * 1000));
#endif
}

const char* OutcomeGlyph(TestOutcome o) {
  switch (o) {
    case TestOutcome::Passed: return "ok  ";
    case TestOutcome::Failed: return "FAIL";
    case TestOutcome::Skipped: return "skip";
    case TestOutcome::ExpectedFailure: return "xfail";
    case TestOutcome::UnexpectedPass: return "XPASS";
  }
  return "?   ";
}

//----------------------------------------------------------------------------
// A headless engine the modes share.
//
// `test` needs the real client bindings; `run` needs the real client host. Both
// attach a normal engine with the client enabled, exactly like the game would,
// so nothing here is a special test-only path.
//----------------------------------------------------------------------------

class HeadlessEngine {
 public:
  ~HeadlessEngine() { if (attached_) Engine::Instance().Detach(); }

  bool Start(const std::string& scriptDir, bool devMode,
             const std::string& anchorKey = std::string()) {
    // The CLI presents results itself (a test failure comes from the result
    // record, a script fault from `lastScriptError`), so the engine's own log
    // lines would duplicate them. Silence the sink while still keeping the last
    // message available.
    Log::SetSilent(true);
    Engine& eng = Engine::Instance();
    BerryConfigC cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.size = sizeof cfg;
    cfg.abi_version = BERRY_ABI_VERSION;
    cfg.log_level = BerryLogErrorC;   // a CLI should not print engine chatter
    cfg.auto_attach_modules = 1;
    cfg.enable_render = 1;
    cfg.enable_client = 1;
    cfg.dev_mode = devMode ? 1 : 0;
    cfg.script_directory = scriptDir.empty() ? nullptr : scriptDir.c_str();
    cfg.script_anchor_key = anchorKey.empty() ? nullptr : anchorKey.c_str();
    if (!eng.Bootstrap(cfg)) {
      fprintf(stderr, "berry-run: engine bootstrap failed\n");
      return false;
    }
    if (!eng.Attach()) {
      fprintf(stderr, "berry-run: engine attach failed\n");
      return false;
    }
    attached_ = true;
    return true;
  }

  void Stop() {
    if (attached_) {
      Engine::Instance().Detach();
      attached_ = false;
    }
  }

  BerryClient* client() { return Engine::Instance().Client(); }

 private:
  bool attached_ = false;
};

//----------------------------------------------------------------------------
// test mode
//----------------------------------------------------------------------------

struct TestArgs {
  std::string target;
  std::string filter;
  size_t maxTests = 4096;
  bool useReferenceVm = false;
  bool runFixtures = true;
  bool listOnly = false;
};

/// The VM factory the runner uses: Luau (the production VM) unless the caller
/// asked for the reference VM.
ScriptTestRunner::VmFactory MakeVmFactory(bool useReferenceVm) {
  if (useReferenceVm) {
    return []() -> std::unique_ptr<IScriptVm> {
      return std::unique_ptr<IScriptVm>(CreateReferenceVm());
    };
  }
  return []() -> std::unique_ptr<IScriptVm> {
    return std::unique_ptr<IScriptVm>(CreateLuauVm());
  };
}

int RunOneTestFile(const std::string& label, const std::string& source,
                   ScriptBindings* bindings, const TestArgs& args, bool* anyFail) {
  ScriptTestRunner runner(MakeVmFactory(args.useReferenceVm), AllCapabilities(),
                          bindings);
  ScriptTestOptions opts;
  opts.filter = args.filter;
  opts.maxTests = args.maxTests;
  opts.runFixtures = args.runFixtures;

  if (args.listOnly) {
    std::string err;
    const std::vector<std::string> names = runner.ListTests(source, label, &err);
    if (!err.empty()) {
      fprintf(stderr, "%s: discovery failed: %s\n", label.c_str(), err.c_str());
      *anyFail = true;
      return 1;
    }
    for (const std::string& n : names) printf("%s\n", n.c_str());
    return 0;
  }

  const ScriptTestReport r = runner.Run(source, label, opts);
  if (!r.discoveryOk) {
    fprintf(stderr, "%s: discovery failed: %s\n", label.c_str(), r.error.c_str());
    *anyFail = true;
    return 1;
  }
  for (const ScriptTestResult& res : r.results) {
    printf("%s %s", OutcomeGlyph(res.outcome), res.name.c_str());
    if (!res.message.empty()) printf(": %s", res.message.c_str());
    printf("\n");
  }
  printf("%s: %zu passed, %zu failed, %zu skipped, %zu expected-failures\n",
         label.c_str(), r.passed, r.failed, r.skipped, r.expectedFailures);
  if (!r.ok()) *anyFail = true;
  return r.ok() ? 0 : 1;
}

int RunTestMode(const TestArgs& args) {
  if (args.target.empty()) {
    fprintf(stderr, "berry-run test: needs a script file or a directory\n");
    return 2;
  }
  // The client is what carries the IDL bindings the tests call. A client that
  // fails to attach is a hard error: a test that silently ran with no bindings
  // would report passes for calls that should have been exercised.
  HeadlessEngine eng;
  if (!eng.Start("", /*devMode=*/true)) return 2;
  BerryClient* client = eng.client();
  if (client == nullptr) {
    fprintf(stderr, "berry-run test: the client did not attach (no bindings)\n");
    return 2;
  }
  ScriptBindings* bindings = &client->bindings();

  bool anyFail = false;
  std::vector<std::string> files;
  if (IsDirectory(args.target)) {
    const std::string dir = args.target;
    for (const std::string& name : ListLuauFiles(dir)) {
      files.push_back(dir + "/" + name);
    }
    if (files.empty()) {
      fprintf(stderr, "berry-run test: no *.luau files under %s\n",
              dir.c_str());
      return 2;
    }
  } else if (IsFile(args.target)) {
    files.push_back(args.target);
  } else {
    fprintf(stderr, "berry-run test: '%s' is not a file or directory\n",
            args.target.c_str());
    return 2;
  }

  for (const std::string& path : files) {
    std::string source;
    if (!ReadWholeFile(path, &source)) {
      fprintf(stderr, "%s: cannot read\n", path.c_str());
      anyFail = true;
      continue;
    }
    RunOneTestFile(path, source, bindings, args, &anyFail);
  }
  eng.Stop();
  return anyFail ? 1 : 0;
}

//----------------------------------------------------------------------------
// run mode
//----------------------------------------------------------------------------

struct RunArgs {
  std::string scriptDir;
  std::string anchorHex;
  bool devMode = false;
  u64 ticks = 20;
  u64 frames = 10;
  bool verbose = false;
};

int RunScriptMode(const RunArgs& args) {
  if (args.scriptDir.empty()) {
    fprintf(stderr, "berry-run run: needs a script directory\n");
    return 2;
  }
  if (!IsDirectory(args.scriptDir)) {
    fprintf(stderr, "berry-run run: '%s' is not a directory\n",
            args.scriptDir.c_str());
    return 2;
  }
  // Outside dev mode the loader requires a verified manifest signature, which
  // needs an anchor key. Say so up front rather than letting the loader refuse
  // with a less actionable reason.
  if (!args.devMode && args.anchorHex.empty()) {
    fprintf(stderr,
            "berry-run run: a verified manifest needs a trust anchor; pass\n"
            "  --anchor <hex-ed25519-public-key>  (production)\n"
            "  --dev                              (skip verification, local only)\n");
    return 2;
  }
  HeadlessEngine eng;
  if (!eng.Start(args.scriptDir, args.devMode, args.anchorHex)) return 2;
  BerryClient* client = eng.client();
  if (client == nullptr) {
    fprintf(stderr, "berry-run run: the client did not attach\n");
    return 2;
  }

  LoadedScript* script = client->script();
  if (script == nullptr || !script->valid()) {
    const std::string why = client->lastScriptError();
    fprintf(stderr, "berry-run run: script refused: %s\n",
            why.empty() ? "(no reason reported)" : why.c_str());
    eng.Stop();
    return 1;
  }

  const ScriptManifest& m = script->manifest();
  printf("script    : %s %s\n", m.name.c_str(), m.version.c_str());
  printf("entry     : %s\n", m.entry.c_str());
  if (!m.author.empty()) printf("author    : %s\n", m.author.c_str());
  printf("directory : %s\n", script->directory().c_str());
  printf("capabilities:\n");
  if (script->granted().empty()) {
    printf("  (none)\n");
  } else {
    for (const Capability& c : script->granted().All()) {
      printf("  %s\n", c.Canonical().c_str());
    }
  }

  // Drive the host the way the engine does: a game tick, then a frame, with a
  // short yield so the script worker can drain the posted work between calls.
  for (u64 i = 0; i < args.ticks; ++i) {
    Engine::Instance().OnGameTick((GameTick)i);
    SleepMs(1);
  }
  for (u64 i = 0; i < args.frames; ++i) {
    Engine::Instance().OnFrame();
    SleepMs(1);
  }

  const std::string err = client->lastScriptError();
  if (!err.empty()) {
    fprintf(stderr, "berry-run run: script faulted: %s\n", err.c_str());
    eng.Stop();
    return 1;
  }
  if (args.verbose) {
    printf("drove %llu tick(s) and %llu frame(s)\n",
           (unsigned long long)args.ticks, (unsigned long long)args.frames);
  } else {
    printf("ok: %llu tick(s), %llu frame(s)\n", (unsigned long long)args.ticks,
           (unsigned long long)args.frames);
  }
  eng.Stop();
  return 0;
}

//----------------------------------------------------------------------------
// replay mode
//----------------------------------------------------------------------------

struct ReplayArgs {
  std::string path;
  bool requireFingerprint = false;
  ReplayFingerprint expected;
};

int RunReplayMode(const ReplayArgs& args) {
  if (args.path.empty()) {
    fprintf(stderr, "berry-run replay: needs a file\n");
    return 2;
  }
  std::string data;
  if (!ReadWholeFile(args.path, &data)) {
    fprintf(stderr, "berry-run replay: cannot read '%s'\n", args.path.c_str());
    return 2;
  }
  ReplayFile file;
  ReplayReader reader;
  const ReplayError err = reader.Read(data, args.expected,
                                     args.requireFingerprint, &file);
  if (err != ReplayError::Ok) {
    fprintf(stderr, "berry-run replay: refused (%s)\n", ReplayErrorName(err));
    return 1;
  }

  const ReplayFingerprint& fp = file.fingerprint;
  printf("fingerprint : build=%u proto=%u label=%s\n", fp.buildId,
         fp.protocolVersion, fp.label.empty() ? "-" : fp.label.c_str());
  printf("sections    : %zu\n", file.sectionCount());
  printf("ticks       : %llu..%llu\n", (unsigned long long)file.firstTick(),
         (unsigned long long)file.lastTick());
  printf("payload     : %zu bytes\n", file.payloadBytes());
  printf("index       : %zu keyframe(s)\n", file.index.size());

  // A record histogram by kind, so a capture that only carried entity state is
  // visible at a glance.
  size_t byKind[(size_t)ReplayRecordKind::Count] = {0};
  for (const ReplaySection& s : file.sections) {
    for (const ReplayRecord& rec : s.records) {
      const size_t k = (size_t)rec.kind;
      if (k < (size_t)ReplayRecordKind::Count) byKind[k]++;
    }
  }
  printf("records     :\n");
  for (size_t k = 0; k < (size_t)ReplayRecordKind::Count; ++k) {
    if (byKind[k] == 0) continue;
    printf("  %-14s %zu\n",
           ReplayRecordKindName((ReplayRecordKind)k), byKind[k]);
  }
  return 0;
}

//----------------------------------------------------------------------------
// usage
//----------------------------------------------------------------------------

void Usage(const char* argv0) {
  printf(
      "berry-run — the BerryAPI scripting developer CLI\n"
      "\n"
      "usage:\n"
      "  %s test <file-or-dir> [--filter <substr>] [--max <n>] [--list]\n"
      "                        [--ref-vm] [--no-fixtures]\n"
      "  %s run <script-dir> [--ticks <n>] [--frames <n>] [--dev]\n"
      "                      [--anchor <hex-key>] [--verbose]\n"
      "  %s replay <file> [--expect-build <n>] [--expect-proto <n>]\n"
      "                   [--expect-label <s>]\n"
      "  %s version\n"
      "\n"
      "test    run a script's `test_*`/`xfail_*`/`skip_*` exports against the\n"
      "        real client bindings; exit 0 only when every test passes\n"
      "run     load a script directory through the verified loader and drive\n"
      "        it headlessly for N ticks and frames\n"
      "replay  read a replay container and print its structure\n"
      "\n"
      "Exit codes: 0 ok, 1 a test/load failed, 2 usage error.\n",
      argv0, argv0, argv0, argv0);
}

/// Parses `--expect-build/--expect-proto/--expect-label` into a fingerprint and
/// flips `requireFingerprint` on when any was given.
bool ParseFingerprint(const std::string& opt, const std::string& value,
                      ReplayArgs* args) {
  if (opt == "--expect-build") {
    args->expected.buildId = (u32)strtoul(value.c_str(), nullptr, 10);
  } else if (opt == "--expect-proto") {
    args->expected.protocolVersion = (u32)strtoul(value.c_str(), nullptr, 10);
  } else if (opt == "--expect-label") {
    args->expected.label = value;
  } else {
    return false;
  }
  args->requireFingerprint = true;
  return true;
}

int RunVersion() {
  printf("berry-run\n");
  printf("  library     : %s\n", BERRY_VERSION_STRING);
  printf("  abi         : %d\n", BERRY_ABI_VERSION);
  printf("  script api  : %s\n", api::ScriptApiVersion());
  printf("  replay fmt  : %u\n", kReplayFormatVersion);
  printf("  sigpack fmt : %u\n", SignaturePack::kFormatVersion);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    Usage(argv[0]);
    return 2;
  }
  const std::string mode = argv[1];
  if (mode == "-h" || mode == "--help" || mode == "help") {
    Usage(argv[0]);
    return 0;
  }
  if (mode == "version") return RunVersion();

  // A `--opt value` parser shared by the modes; `takesValue` decides whether an
  // option consumes the next argument.
  int i = 2;
  auto next = [&](const char* what) -> std::string {
    if (i + 1 >= argc) {
      fprintf(stderr, "berry-run: %s needs a value\n", what);
      exit(2);
    }
    return argv[++i];
  };

  if (mode == "test") {
    TestArgs args;
    for (; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--filter") args.filter = next("--filter");
      else if (a == "--max") args.maxTests = (size_t)strtoull(next("--max").c_str(), nullptr, 10);
      else if (a == "--list") args.listOnly = true;
      else if (a == "--ref-vm") args.useReferenceVm = true;
      else if (a == "--no-fixtures") args.runFixtures = false;
      else if (a == "-h" || a == "--help") { Usage(argv[0]); return 0; }
      else if (!a.empty() && a[0] == '-') {
        fprintf(stderr, "berry-run test: unknown option '%s'\n", a.c_str());
        return 2;
      } else if (args.target.empty()) {
        args.target = a;
      } else {
        fprintf(stderr, "berry-run test: unexpected argument '%s'\n", a.c_str());
        return 2;
      }
    }
    return RunTestMode(args);
  }

  if (mode == "run") {
    RunArgs args;
    for (; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--ticks") args.ticks = strtoull(next("--ticks").c_str(), nullptr, 10);
      else if (a == "--frames") args.frames = strtoull(next("--frames").c_str(), nullptr, 10);
      else if (a == "--dev") args.devMode = true;
      else if (a == "--anchor") args.anchorHex = next("--anchor");
      else if (a == "--verbose" || a == "-v") args.verbose = true;
      else if (a == "-h" || a == "--help") { Usage(argv[0]); return 0; }
      else if (!a.empty() && a[0] == '-') {
        fprintf(stderr, "berry-run run: unknown option '%s'\n", a.c_str());
        return 2;
      } else if (args.scriptDir.empty()) {
        args.scriptDir = a;
      } else {
        fprintf(stderr, "berry-run run: unexpected argument '%s'\n", a.c_str());
        return 2;
      }
    }
    return RunScriptMode(args);
  }

  if (mode == "replay") {
    ReplayArgs args;
    for (; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--expect-build" || a == "--expect-proto" || a == "--expect-label") {
        ParseFingerprint(a, next(a.c_str()), &args);
      } else if (a == "-h" || a == "--help") { Usage(argv[0]); return 0; }
      else if (!a.empty() && a[0] == '-') {
        fprintf(stderr, "berry-run replay: unknown option '%s'\n", a.c_str());
        return 2;
      } else if (args.path.empty()) {
        args.path = a;
      } else {
        fprintf(stderr, "berry-run replay: unexpected argument '%s'\n", a.c_str());
        return 2;
      }
    }
    return RunReplayMode(args);
  }

  fprintf(stderr, "berry-run: unknown mode '%s'\n", mode.c_str());
  Usage(argv[0]);
  return 2;
}
