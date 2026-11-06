// Host tests for the sandboxed module resolver (improvement 63).
#include "berry/require.h"
#include <cstdio>
#include <map>
#include <string>
using namespace Berry;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct MemFs : IScriptFileSystem {
  std::map<std::string, std::string> files;
  bool ReadFile(const std::string& p, std::string* out) override {
    auto it = files.find(p);
    if (it == files.end()) return false;
    *out = it->second;
    return true;
  }
};

int main() {
  // Name safety.
  CHECK(ModuleResolver::SafeName("helpers"));
  CHECK(ModuleResolver::SafeName("a/b/c"));
  CHECK(!ModuleResolver::SafeName(""));
  CHECK(!ModuleResolver::SafeName("/abs"));
  CHECK(!ModuleResolver::SafeName("../escape"));
  CHECK(!ModuleResolver::SafeName("a/../../b"));
  CHECK(!ModuleResolver::SafeName("a\\b"));
  CHECK(ModuleResolver::SafeName("a..b"));   // '..' only as a whole component

  MemFs fs;
  fs.files["mod/helpers.luau"] = "-- version: 1.2.0\nreturn { add = function(a,b) return a+b end }\n";
  fs.files["mod/plain.luau"] = "return {}\n";
  ModuleResolver r(&fs, "mod");

  std::string src, ver;
  CHECK(r.Resolve("helpers", "", &src, &ver) == RequireError::Ok);
  CHECK(src.find("add") != std::string::npos);
  CHECK(ver == "1.2.0");
  CHECK(r.Resolved().size() == 1);

  // Cached: a second resolve does not add to the order.
  CHECK(r.Resolve("helpers", "", &src) == RequireError::Ok);
  CHECK(r.Resolved().size() == 1);

  // Version constraints.
  CHECK(r.Resolve("helpers", "^1.0.0", &src) == RequireError::Ok);
  CHECK(r.Resolve("helpers", "^2.0.0", &src) == RequireError::VersionMismatch);
  // A module with no declared version fails a constrained require.
  CHECK(r.Resolve("plain", "^1.0.0", &src) == RequireError::VersionMismatch);
  CHECK(r.Resolve("plain", "", &src) == RequireError::Ok);
  CHECK(r.Version("plain").empty());

  // Escapes and missing modules.
  CHECK(r.Resolve("../secret", "", &src) == RequireError::BadName);
  CHECK(r.Resolve("/etc/passwd", "", &src) == RequireError::BadName);
  CHECK(r.Resolve("nope", "", &src) == RequireError::NotFound);

  // Reset clears the cache.
  r.Reset();
  CHECK(r.Resolved().empty());

  if (g_fail == 0) printf("test_require: ALL OK\n");
  return g_fail;
}
