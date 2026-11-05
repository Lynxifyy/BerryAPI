// Host tests for the per-script secret store (improvement 65).
#include "berry/secret_store.h"
#include <cstdio>
#include <string>
using namespace Berry;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
int main() {
  MemorySecretBackend backend;
  SecretStore store(&backend);

  CHECK(store.Put("s1", "token", "abc123"));
  std::string v;
  CHECK(store.Get("s1", "token", &v));
  CHECK(v == "abc123");
  // Namespaced: another script cannot read it.
  CHECK(!store.Get("s2", "token", &v));
  CHECK(store.Keys("s2").empty());
  CHECK(store.Keys("s1").size() == 1);

  // Erase.
  CHECK(store.Erase("s1", "token"));
  CHECK(!store.Erase("s1", "token"));
  CHECK(!store.Get("s1", "token", &v));

  // Refusals.
  CHECK(!store.Put("", "k", "v"));                       // no script
  CHECK(!store.Put("s1", "", "v"));                      // no key
  CHECK(!store.Put("s1", std::string(100, 'k'), "v"));   // key too long
  CHECK(!store.Put("s1", "k", std::string(5000, 'v')));  // value too long

  // Per-script entry cap.
  backend.Clear();
  for (size_t i = 0; i < SecretStore::kMaxEntriesPerScript; ++i) {
    CHECK(store.Put("cap", "k" + std::to_string(i), "v"));
  }
  CHECK(!store.Put("cap", "one-too-many", "v"));
  // Overwriting an existing key is still allowed at the cap.
  CHECK(store.Put("cap", "k0", "v2"));

  // Forget wipes a script's secrets but not another's.
  CHECK(store.Put("keep", "k", "v"));
  CHECK(store.Forget("cap") == SecretStore::kMaxEntriesPerScript);
  CHECK(store.Keys("cap").empty());
  CHECK(store.Get("keep", "k", &v));

  if (g_fail == 0) printf("test_secret_store: ALL OK\n");
  return g_fail;
}
