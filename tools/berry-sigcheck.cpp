// berry-sigcheck — validate a signature table against an extracted module
// offline, in CI, before release (improvement 30).
//
// Usage:
//   berry-sigcheck <module.so> --pattern "48 8D 0D ?? ??"
//   berry-sigcheck <module.so> --table sigs.txt
//   berry-sigcheck <module.so> --build-id
//
// A table file has one rule per line: "<id>  <pattern>  [from_end]". Lines
// starting with '#' and blank lines are ignored.
//
// Exit code 0 when every rule matches exactly once (or when --build-id prints
// the id), 1 when a rule is missing or ambiguous, 2 on a usage/setup error.
#include "berry/sigscan.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace Berry;

namespace {

struct Rule {
  std::string id;
  std::string pattern;
  i32 fromEnd = 0;
};

bool LoadTable(const std::string& path, std::vector<Rule>* out) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line;
  while (std::getline(f, line)) {
    size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos || line[b] == '#') continue;
    size_t e = line.find_last_not_of(" \t\r");
    line = line.substr(b, e - b + 1);
    size_t sp = line.find_first_of(" \t");
    if (sp == std::string::npos) continue;
    Rule r;
    r.id = line.substr(0, sp);
    std::string rest = line.substr(sp);
    size_t rb = rest.find_first_not_of(" \t");
    rest = rb == std::string::npos ? std::string() : rest.substr(rb);
    // A trailing integer is the from_end offset; the rest is the pattern.
    size_t lastSp = rest.find_last_of(" \t");
    if (lastSp != std::string::npos) {
      std::string tail = rest.substr(lastSp + 1);
      if (!tail.empty()) {
        bool numeric = true;
        for (char c : tail) if (c != '-' && (c < '0' || c > '9')) numeric = false;
        if (numeric) {
          r.fromEnd = (i32)strtol(tail.c_str(), nullptr, 10);
          rest = rest.substr(0, lastSp);
        }
      }
    }
    r.pattern = rest;
    out->push_back(r);
  }
  return true;
}

int CheckRules(const std::string& modulePath, const std::vector<Rule>& rules) {
  std::ifstream f(modulePath, std::ios::binary);
  if (!f) {
    fprintf(stderr, "berry-sigcheck: cannot open %s\n", modulePath.c_str());
    return 2;
  }
  std::string data((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
  if (data.empty()) {
    fprintf(stderr, "berry-sigcheck: %s is empty\n", modulePath.c_str());
    return 2;
  }
  MemRange range;
  range.base = (uptr)data.data();
  range.size = data.size();
  range.name = modulePath;
  SigScanner scanner(std::vector<MemRange>{range});

  int failures = 0;
  for (const Rule& r : rules) {
    Signature sig(r.id, r.pattern.c_str(), r.fromEnd);
    if (sig.bytes.empty()) {
      printf("BAD   %-24s pattern does not parse: %s\n", r.id.c_str(),
             r.pattern.c_str());
      ++failures;
      continue;
    }
    std::vector<uptr> hits = scanner.FindAll(sig, 8);
    if (hits.size() == 1) {
      printf("OK    %-24s 1 match\n", r.id.c_str());
    } else if (hits.empty()) {
      size_t diff = 0;
      uptr near = scanner.ClosestMatch(sig, &diff);
      printf("MISS  %-24s 0 matches (closest at +0x%llx, %zu bits differ)\n",
             r.id.c_str(), (unsigned long long)(near ? near - range.base : 0),
             diff);
      ++failures;
    } else {
      printf("AMBIG %-24s %zu matches\n", r.id.c_str(), hits.size());
      ++failures;
    }
  }
  printf("== %zu rules, %d failed ==\n", rules.size(), failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr,
            "usage: berry-sigcheck <module.so> --pattern <hex> | --table <file>"
            " | --build-id\n");
    return 2;
  }
  const std::string modulePath = argv[1];
  std::string mode = argv[2];

  if (mode == "--build-id") {
    std::string id = BuildLibrary::ReadBuildId(modulePath.c_str());
    if (id.empty()) {
      fprintf(stderr, "berry-sigcheck: no build-id in %s\n", modulePath.c_str());
      return 1;
    }
    printf("%s\n", id.c_str());
    return 0;
  }
  if (mode == "--pattern") {
    if (argc < 4) {
      fprintf(stderr, "berry-sigcheck: --pattern needs a pattern\n");
      return 2;
    }
    std::vector<Rule> rules{{"pattern", argv[3], 0}};
    return CheckRules(modulePath, rules);
  }
  if (mode == "--table") {
    if (argc < 4) {
      fprintf(stderr, "berry-sigcheck: --table needs a file\n");
      return 2;
    }
    std::vector<Rule> rules;
    if (!LoadTable(argv[3], &rules)) {
      fprintf(stderr, "berry-sigcheck: cannot read table %s\n", argv[3]);
      return 2;
    }
    return CheckRules(modulePath, rules);
  }
  fprintf(stderr, "berry-sigcheck: unknown mode %s\n", mode.c_str());
  return 2;
}
