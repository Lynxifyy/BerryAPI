// Host tests for the sandboxed script file store (Phase D).
//
// The store is the whole sandbox: if the path rules or the size caps are wrong,
// a script can reach outside its own directory or exhaust memory. These tests
// pin both, and pin that the rules are the same on every operation.
#include "berry/fs.h"

#include <cstdio>
#include <string>

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

int main() {
  TestPathValidation();
  TestWriteReadRoundTrip();
  TestCapsFailClosed();
  TestListRemoveClear();
  if (g_fail == 0) printf("test_fs: ALL OK\n");
  return g_fail;
}
