// Host tests for the capability consent store (improvement 64).
#include "berry/consent.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace Berry;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
int main() {
  ConsentStore s;
  // Undecided initially.
  CHECK(s.Get("mod", "network") == ConsentStore::Decision::Undecided);
  // A grantable capability can be decided; an unknown one cannot.
  CHECK(s.Decide("mod", "network", true));
  CHECK(!s.Decide("mod", "teleport", true));   // not a real capability
  CHECK(s.Get("mod", "network") == ConsentStore::Decision::Granted);
  CHECK(s.Decide("mod", "world", false));
  CHECK(s.Get("mod", "world") == ConsentStore::Decision::Denied);

  // Prompt reports the still-undecided ones and the decisions.
  std::vector<std::pair<std::string, std::string>> requested = {
      {"network", "fetch the leaderboard"},
      {"world", "read your position"},
      {"hud", "draw a timer"}};
  auto prompt = s.Prompt("mod", requested);
  CHECK(prompt.size() == 3);
  CHECK(prompt[0].decided && prompt[0].granted);
  CHECK(prompt[0].reason == "fetch the leaderboard");
  CHECK(prompt[1].decided && !prompt[1].granted);
  CHECK(!prompt[2].decided);

  // AllGranted fails closed while any is undecided or denied.
  CHECK(!s.AllGranted("mod", requested));
  s.Decide("mod", "hud", true);
  CHECK(!s.AllGranted("mod", requested));   // world is still denied
  s.Decide("mod", "world", true);
  CHECK(s.AllGranted("mod", requested));

  // Decisions are per script.
  CHECK(s.Get("other", "network") == ConsentStore::Decision::Undecided);

  // Revoke one / all.
  CHECK(s.RevokeOne("mod", "hud"));
  CHECK(s.Get("mod", "hud") == ConsentStore::Decision::Undecided);
  CHECK(s.Revoke("mod") == 2);   // network, world remain
  CHECK(s.Get("mod", "network") == ConsentStore::Decision::Undecided);

  if (g_fail == 0) printf("test_consent: ALL OK\n");
  return g_fail;
}
