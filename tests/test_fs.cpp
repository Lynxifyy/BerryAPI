// Host tests for the sandboxed script file store (Phase D).
//
// The store is the whole sandbox: if the path rules or the size caps are wrong,
// a script can reach outside its own directory or exhaust memory. These tests
// pin both, and pin that the rules are the same on every operation.
#include "berry/fs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static void TestPathValidation() {
  // A plain relative path is fine.
  CHECK(ValidateScriptPath("config.json").valid);
  CHECK(ValidateScriptPath("saves/slot1.dat").valid);
  CHECK(ValidateScriptPath("a/b/c/d.txt").valid);

  // Absolute paths, traversal, backslashes, drive letters and control chars are
  // all refused — however they are spelled.
  CHECK(!ValidateScriptPath("/etc/passwd").valid);
  CHECK(!ValidateScriptPath("../../etc/passwd").valid);
  CHECK(!ValidateScriptPath("a/../../b").valid);
  CHECK(!ValidateScriptPath("./x").valid);
  CHECK(!ValidateScriptPath("a/./b").valid);
  CHECK(!ValidateScriptPath("..").valid);
  CHECK(!ValidateScriptPath(".").valid);
  CHECK(!ValidateScriptPath("a//b").valid);
  CHECK(!ValidateScriptPath("a/").valid);
  CHECK(!ValidateScriptPath("").valid);
  CHECK(!ValidateScriptPath("a\\b").valid);
  CHECK(!ValidateScriptPath("C:/x").valid);
  CHECK(!ValidateScriptPath("a\x01z").valid);
  CHECK(!ValidateScriptPath("a\tb").valid);

  // Length and depth are bounded.
  CHECK(!ValidateScriptPath(std::string(300, 'a')).valid);
  std::string deep;
  for (int i = 0; i < 20; ++i) { if (i) deep += '/'; deep += 'x'; }
  CHECK(!ValidateScriptPath(deep).valid);

  // A too-long segment is refused too.
  CHECK(!ValidateScriptPath(std::string(200, 'a')).valid);
}

static void TestWriteReadRoundTrip() {
  ScriptFileStore fs;
  CHECK(fs.Write("a.txt", "hello"));
  CHECK(fs.Exists("a.txt"));
  CHECK(fs.Size("a.txt") == 5);

  std::string out;
  CHECK(fs.Read("a.txt", &out));
  CHECK(out == "hello");

  // Overwrite replaces; size and total track the delta.
  CHECK(fs.Write("a.txt", "hi"));
  CHECK(fs.Size("a.txt") == 2);
  CHECK(fs.TotalBytes() == 2);

  // Append grows.
  CHECK(fs.Append("a.txt", "!!!"));
  CHECK(fs.Read("a.txt", &out));
  CHECK(out == "hi!!!");
  CHECK(fs.Size("a.txt") == 5);

  // A missing file reads empty and reports false.
  CHECK(!fs.Read("nope.txt", &out));
  CHECK(!fs.Exists("nope.txt"));

  // A bad path fails closed on every operation.
  CHECK(!fs.Write("../escape", "x"));
  CHECK(!fs.Read("/abs", &out));
  CHECK(!fs.Exists("a/../b"));
  CHECK(!fs.Remove(".."));
  CHECK(fs.Size("../escape") == 0);
}

static void TestCapsFailClosed() {
  ScriptFileStore fs;
  // A file over the per-file cap is refused.
  CHECK(!fs.Write("big", std::string(ScriptFileStore::kMaxFileBytes + 1, 'x')));
  CHECK(!fs.Exists("big"));
  // Exactly at the cap is allowed.
  CHECK(fs.Write("exact", std::string(ScriptFileStore::kMaxFileBytes, 'x')));
  CHECK(fs.Size("exact") == ScriptFileStore::kMaxFileBytes);

  // An append that would cross the per-file cap is refused, leaving the file
  // unchanged.
  CHECK(!fs.Append("exact", "y"));
  CHECK(fs.Size("exact") == ScriptFileStore::kMaxFileBytes);

  // The total cap is enforced across files.
  ScriptFileStore fs2;
  size_t written = 0;
  bool refused = false;
  for (size_t i = 0; i < 64 && !refused; ++i) {
    const std::string name = "f" + std::to_string(i);
    if (fs2.Write(name, std::string(ScriptFileStore::kMaxFileBytes, 'z'))) {
      written += ScriptFileStore::kMaxFileBytes;
    } else {
      refused = true;
    }
  }
  CHECK(refused);
  CHECK(fs2.TotalBytes() <= ScriptFileStore::kMaxTotalBytes);

  // The file-count cap is enforced.
  ScriptFileStore fs3;
  size_t created = 0;
  for (size_t i = 0; i < ScriptFileStore::kMaxFiles + 10; ++i) {
    if (fs3.Write("g" + std::to_string(i), "x")) ++created;
  }
  CHECK(created == ScriptFileStore::kMaxFiles);
  CHECK(fs3.FileCount() == ScriptFileStore::kMaxFiles);
}

static void TestListRemoveClear() {
  ScriptFileStore fs;
  CHECK(fs.Write("b.txt", "1"));
  CHECK(fs.Write("a.txt", "2"));
  CHECK(fs.Write("sub/c.txt", "3"));

  // Sorted, newline-separated.
  const std::string all = fs.List();
  CHECK(all == "a.txt\nb.txt\nsub/c.txt");
  // A prefix filters.
  CHECK(fs.List("sub/") == "sub/c.txt");
  CHECK(fs.List("zzz") == "");

  CHECK(fs.Remove("a.txt"));
  CHECK(!fs.Remove("a.txt"));
  CHECK(fs.FileCount() == 2);
  CHECK(fs.TotalBytes() == 2);

  fs.Clear();
  CHECK(fs.FileCount() == 0);
  CHECK(fs.TotalBytes() == 0);
  CHECK(fs.List().empty());
}

//----------------------------------------------------------------------------
// Disk persistence (Phase E).
//----------------------------------------------------------------------------

/// A unique temp directory for one test, removed by the caller.
static std::string MakeTempDir(const char* tag) {
  char tmpl[128];
  std::snprintf(tmpl, sizeof tmpl, "/tmp/berry-fs-%s-XXXXXX", tag);
  const char* d = mkdtemp(tmpl);
  return d ? std::string(d) : std::string();
}

static void RemoveTree(const std::string& dir) {
  // Best-effort cleanup; the test asserts on behaviour, not on this.
  std::string cmd = "rm -rf '" + dir + "'";
  if (system(cmd.c_str()) != 0) { /* ignore */ }
}

static void TestDiskRoundTripSurvivesRestart() {
  const std::string root = MakeTempDir("persist");
  CHECK(!root.empty());
  if (root.empty()) return;

  {
    ScriptFileStore fs;
    CHECK(fs.Open(root));
    CHECK(fs.persistent());
    CHECK(fs.root() == root);
    CHECK(fs.Write("config.json", "{\"volume\":7}"));
    CHECK(fs.Write("saves/slot1.dat", "level-3"));
    CHECK(fs.Exists("config.json"));
    CHECK(fs.Size("saves/slot1.dat") == 7);
    CHECK(fs.FileCount() == 2);
    CHECK(fs.TotalBytes() == 12 + 7);

    std::string out;
    CHECK(fs.Read("saves/slot1.dat", &out));
    CHECK(out == "level-3");
  }

  // A fresh store over the same directory sees the data: this is the restart.
  {
    ScriptFileStore fs;
    CHECK(fs.Open(root));
    std::string out;
    CHECK(fs.Read("config.json", &out));
    CHECK(out == "{\"volume\":7}");
    CHECK(fs.Exists("saves/slot1.dat"));
    // The counters were seeded from disk, not reset to zero.
    CHECK(fs.FileCount() == 2);
    CHECK(fs.TotalBytes() == 12 + 7);
    CHECK(fs.List() == "config.json\nsaves/slot1.dat");
    CHECK(fs.List("saves/") == "saves/slot1.dat");
  }

  RemoveTree(root);
}

static void TestDiskAppendAcrossRestart() {
  const std::string root = MakeTempDir("append");
  CHECK(!root.empty());
  if (root.empty()) return;

  {
    ScriptFileStore fs;
    CHECK(fs.Open(root));
    CHECK(fs.Write("log.txt", "one\n"));
  }
  {
    ScriptFileStore fs;
    CHECK(fs.Open(root));
    CHECK(fs.Append("log.txt", "two\n"));
    CHECK(fs.Append("log.txt", "three\n"));
    std::string out;
    CHECK(fs.Read("log.txt", &out));
    CHECK(out == "one\ntwo\nthree\n");
  }

  RemoveTree(root);
}

static void TestDiskQuotaCountsExistingFiles() {
  const std::string root = MakeTempDir("quota");
  CHECK(!root.empty());
  if (root.empty()) return;

  // Pre-fill the directory with more files than the cap allows, outside the
  // store, then prove Open refuses it rather than starting over-limit.
  for (size_t i = 0; i < ScriptFileStore::kMaxFiles + 1; ++i) {
    const std::string p = root + "/f" + std::to_string(i);
    FILE* f = std::fopen(p.c_str(), "wb");
    if (f) {
      std::fputc('x', f);
      std::fclose(f);
    }
  }
  {
    ScriptFileStore fs;
    CHECK(!fs.Open(root));   // over the file cap: refuse, do not reset
    CHECK(!fs.persistent());
  }
  RemoveTree(root);

  // A directory with a few files opens and seeds the counters.
  const std::string root2 = MakeTempDir("quota2");
  CHECK(!root2.empty());
  if (root2.empty()) return;
  {
    FILE* f = std::fopen((root2 + "/pre.txt").c_str(), "wb");
    CHECK(f != nullptr);
    if (f) {
      std::fputs("hello", f);
      std::fclose(f);
    }
  }
  {
    ScriptFileStore fs;
    CHECK(fs.Open(root2));
    CHECK(fs.FileCount() == 1);
    CHECK(fs.TotalBytes() == 5);
    // A read with no out pointer still reports existence.
    CHECK(fs.Read("pre.txt", nullptr));
    std::string out;
    CHECK(fs.Read("pre.txt", &out));
    CHECK(out == "hello");
    // Growing past the file cap still fails closed.
    CHECK(!fs.Write("pre.txt", std::string(ScriptFileStore::kMaxFileBytes + 1, 'z')));
  }
  RemoveTree(root2);
}

static void TestDiskSymlinkRefused() {
  const std::string root = MakeTempDir("symlink");
  CHECK(!root.empty());
  if (root.empty()) return;

  // A secret outside the sandbox that a symlink would try to reach.
  const std::string secret = root + "-secret.txt";
  {
    FILE* f = std::fopen(secret.c_str(), "wb");
    CHECK(f != nullptr);
    if (f) {
      std::fputs("TOPSECRET", f);
      std::fclose(f);
    }
  }

  ScriptFileStore fs;
  CHECK(fs.Open(root));

  // A symlink *inside* the sandbox pointing at the secret: writing through it
  // must be refused, not followed. (The path validator already rejects a
  // traversal spelling, so this is the residual attack: a link planted in the
  // tree.)
  const std::string link = root + "/escape";
  CHECK(symlink(secret.c_str(), link.c_str()) == 0);
  CHECK(!fs.Write("escape", "pwned"));
  CHECK(!fs.Exists("escape"));

  // The secret was not modified.
  {
    FILE* f = std::fopen(secret.c_str(), "rb");
    CHECK(f != nullptr);
    char buf[32] = {0};
    size_t n = f ? std::fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) std::fclose(f);
    CHECK(n == 9);
    CHECK(std::strcmp(buf, "TOPSECRET") == 0);
  }

  // A symlinked *directory* component is refused too: a link named "sub"
  // pointing outside cannot be used as a directory to write under.
  const std::string dirlink = root + "/sub";
  CHECK(symlink(root.c_str(), dirlink.c_str()) == 0);
  CHECK(!fs.Write("sub/evil.txt", "x"));

  // A real file in the sandbox still works alongside the links.
  CHECK(fs.Write("good.txt", "ok"));
  std::string out;
  CHECK(fs.Read("good.txt", &out));
  CHECK(out == "ok");

  RemoveTree(root);
  unlink(secret.c_str());
}

static void TestDiskTraversalRefused() {
  const std::string root = MakeTempDir("traversal");
  CHECK(!root.empty());
  if (root.empty()) return;

  ScriptFileStore fs;
  CHECK(fs.Open(root));
  // Every traversal spelling is refused by the pure validator before any disk
  // call, so nothing is created outside.
  CHECK(!fs.Write("../escape", "x"));
  CHECK(!fs.Write("a/../../escape", "x"));
  CHECK(!fs.Write("/etc/passwd", "x"));
  CHECK(!fs.Write("a\\b", "x"));
  CHECK(!fs.Exists("../escape"));
  CHECK(fs.FileCount() == 0);

  RemoveTree(root);
}

static void TestDiskWriteIsAtomic() {
  const std::string root = MakeTempDir("atomic");
  CHECK(!root.empty());
  if (root.empty()) return;

  ScriptFileStore fs;
  CHECK(fs.Open(root));
  CHECK(fs.Write("a.txt", "first"));
  CHECK(fs.Write("a.txt", "second"));
  std::string out;
  CHECK(fs.Read("a.txt", &out));
  CHECK(out == "second");
  // No staging file is left behind after a successful write.
  CHECK(fs.FileCount() == 1);
  CHECK(fs.List() == "a.txt");

  CHECK(fs.Remove("a.txt"));
  CHECK(!fs.Exists("a.txt"));
  CHECK(fs.FileCount() == 0);

  CHECK(fs.Write("b.txt", "1"));
  fs.Clear();
  CHECK(fs.FileCount() == 0);
  CHECK(fs.List().empty());

  RemoveTree(root);
}

static void TestDiskClearAndRenameDir() {
  const std::string root = MakeTempDir("clear");
  CHECK(!root.empty());
  if (root.empty()) return;
  ScriptFileStore fs;
  CHECK(fs.Open(root));
  CHECK(fs.Write("x/y/z.txt", "deep"));
  CHECK(fs.FileCount() == 1);
  CHECK(fs.Exists("x/y/z.txt"));
  fs.Clear();
  CHECK(fs.FileCount() == 0);
  CHECK(!fs.Exists("x/y/z.txt"));
  RemoveTree(root);
}

int main() {
  TestPathValidation();
  TestWriteReadRoundTrip();
  TestCapsFailClosed();
  TestListRemoveClear();
  TestDiskRoundTripSurvivesRestart();
  TestDiskAppendAcrossRestart();
  TestDiskQuotaCountsExistingFiles();
  TestDiskSymlinkRefused();
  TestDiskTraversalRefused();
  TestDiskWriteIsAtomic();
  TestDiskClearAndRenameDir();
  if (g_fail == 0) printf("test_fs: ALL OK\n");
  return g_fail;
}
