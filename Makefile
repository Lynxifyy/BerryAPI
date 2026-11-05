# BerryAPI host build (self-contained; no external deps).
# Produces tests/berry_tests binary that runs every module's test suite.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -g -Iinclude -Ithird_party/ed25519 $(MBEDTLS_INCLUDES) $(LZ4_INCLUDES) $(LUAU_INCLUDES) -DBERRY_THREAD_ASSERT
LDFLAGS  ?= -pthread

# Header dependency tracking. Without it, editing a header leaves every object
# that included it stale, and the link then mixes two incompatible layouts of
# the same class — a crash that looks like a logic bug. Every compile rule
# below emits a .d file next to its object; the `-include` of those files lives
# after `tools/sources.mk` (which defines CORE_SRC/TEST_SRC), because a
# `-include` before the variable is set expands to nothing and silently does
# no tracking at all.
DEPFLAGS := -MMD -MP

# The injectable shared library (android/linux target build).
# Compile with -fPIC and -fvisibility=hidden; only BERRY_API symbols escape.
SHARED_CXXFLAGS ?= -std=c++17 -O2 -fPIC -fvisibility=hidden -DBERRY_BUILD_SHARED -Iinclude -Ithird_party/ed25519 $(MBEDTLS_INCLUDES) $(LZ4_INCLUDES) $(LUAU_INCLUDES)
SHARED_LDFLAGS  ?= -shared -pthread -ldl -Wl,--version-script=tools/berry.exports.map
SHARED_NAME     ?= libBerryAPI.so

include tools/sources.mk

# Pulled in after sources.mk so CORE_SRC/TEST_SRC are populated. Each is
# optional: on a fresh checkout the .d files do not exist yet.
-include $(CORE_SRC:.cpp=.d)
-include $(TEST_SRC:.cpp=.d)
-include $(patsubst %.cpp,build/san/%.d,$(CORE_SRC))
-include $(patsubst %.cpp,build/san/tests/%.d,$(TEST_SRC))
-include $(patsubst %.cpp,build/shared/%.d,$(CORE_SRC))
# The tools/ binaries (berry-run, berry-relay) compile with -MMD too, but their
# .d files live under build/tools/. Without these two lines a header change
# leaves build/tools/berry-run.o stale, and it then links against core objects
# with a different layout — the exact silent-mismatch failure the header
# tracking exists to prevent.
-include build/tools/berry-run.d
-include build/tools/berry-relay.d
-include build/tools/berry-sigcheck.d
-include $(MBEDTLS_SRC:.c=.d)
-include $(LZ4_SRC:.c=.d)
-include $(patsubst %.c,build/san/%.d,$(LZ4_SRC))
-include $(patsubst %.c,build/shared/%.d,$(LZ4_SRC))

# Vendored C (Ed25519) is compiled as C99 into the same object set as the C++
# core, so both the host tests and the injectable .so carry it.
ED25519_OBJ := $(ED25519_SRC:.c=.o)
MBEDTLS_OBJ := $(MBEDTLS_SRC:.c=.o)
LZ4_OBJ := $(LZ4_SRC:.c=.o)
# Test-only mbedTLS object set: the same sources compiled with the test config
# (tests/tls_test_config) that adds the TLS server role. Every mbedTLS source is
# rebuilt, because a client-only ssl_tls.o will not link with the server TUs;
# the objects land under build/mbedtls-test/ so they never collide with the
# shipping ones. Defined here (before TEST_CORE_OBJ) because := is immediate.
# The server handshake lives in two TUs the shipping (client-only) build does
# not compile; the test build needs them for the in-test TLS server.
MBEDTLS_TEST_EXTRA_SRC := third_party/mbedtls/library/ssl_tls12_server.c \
                          third_party/mbedtls/library/ssl_tls13_server.c \
                          tests/tls_test_config/debug.c
MBEDTLS_TEST_OBJ := $(patsubst %.c,build/mbedtls-test/%.o,$(MBEDTLS_SRC)) \
                    $(patsubst %.c,build/mbedtls-test/%.o,$(MBEDTLS_TEST_EXTRA_SRC))
MBEDTLS_TEST_INCLUDES := -Itests/tls_test_config -Ithird_party/mbedtls/include \
                         -Ithird_party/mbedtls/library \
                         -DMBEDTLS_CONFIG_FILE='"mbedtls/mbedtls_config_test.h"'
LUAU_OBJ := $(LUAU_SRC:.cpp=.o)
CORE_OBJ := $(CORE_SRC:.cpp=.o) $(ED25519_OBJ) $(MBEDTLS_OBJ) $(LZ4_OBJ) $(LUAU_OBJ)
TEST_OBJ := $(TEST_SRC:.cpp=.o)

# Each suite links against the core into its own binary.
TEST_BINS := $(TEST_SRC:tests/%.cpp=build/test_%)
# Host tests link the server-capable mbedTLS build (see MBEDTLS_TEST_OBJ);
# the shipping client-only objects are used by the .so and by check-sources.
# tls.cpp is the only core TU that includes mbedTLS headers, so it MUST be
# compiled with the same config as the mbedTLS objects it links against (the
# test build's server-capable config); a mismatch changes struct layouts and
# fails at runtime. So the test build recompiles that one file too.
TEST_TLS_OBJ := build/testcfg/core/net/tls.o
TEST_CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -g -Iinclude -Ithird_party/ed25519 $(MBEDTLS_TEST_INCLUDES) $(LUAU_INCLUDES) -DBERRY_THREAD_ASSERT
TEST_CORE_OBJ := $(filter-out $(MBEDTLS_OBJ) core/net/tls.o,$(CORE_OBJ)) $(MBEDTLS_TEST_OBJ) $(TEST_TLS_OBJ)

all: test

build:
	mkdir -p build

build/test_%: tests/%.o $(TEST_CORE_OBJ) | build
	$(CXX) $(TEST_CXXFLAGS) $(TEST_CORE_OBJ) tests/$*.o -o $@ $(LDFLAGS)

# Compile rules
core/%.o: core/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

util/%.o: util/%.cpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Vendored Luau: upstream is warning-clean only under its own flag set, so it
# is compiled with -w. Our own units keep -Wall -Wextra.
third_party/luau/%.o: third_party/luau/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -fPIC -w $(LUAU_INCLUDES) -c $< -o $@

# Vendored Ed25519: C99, and no seed source (key generation is
# caller-supplied, so nothing reads /dev/urandom inside the library).
third_party/ed25519/%.o: third_party/ed25519/%.c
	$(CC) -std=c99 -O2 -Wall -Wextra -DED25519_NO_SEED -c $< -o $@

# Vendored LZ4: C99, compiled with its own -w (upstream is warning-clean only
# under its own flag set). One rule covers both lz4.c and lz4hc.c.
third_party/lz4/lib/%.o: third_party/lz4/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -O2 -w -Ithird_party/lz4/lib -c $< -o $@

# Vendored mbedTLS: C99, warning-clean only under its own flag set (upstream
# builds it with -w), compiled against the vendored config and include roots.
third_party/mbedtls/library/%.o: third_party/mbedtls/library/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -O2 -w $(MBEDTLS_INCLUDES) -c $< -o $@

# Test-only mbedTLS: same sources, but compiled with the test config that adds
# the TLS server role so the host tests can complete a real handshake. Every
# mbedTLS object is rebuilt here (a client-only ssl_tls.o would not link with
# the server TUs), and the objects land under build/mbedtls-test/ so they never
# collide with the shipping ones.
build/mbedtls-test/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -O2 -w $(MBEDTLS_TEST_INCLUDES) -c $< -o $@

# The one core TU that must share the test mbedTLS config (see TEST_TLS_OBJ).
build/testcfg/core/net/tls.o: core/net/tls.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(TEST_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# Sanitizer twin, same reason.
build/san/testcfg/core/net/tls.o: core/net/tls.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(subst -O2,-O1,$(TEST_CXXFLAGS)) -fsanitize=address,undefined -fno-omit-frame-pointer $(DEPFLAGS) -c $< -o $@

build/san/mbedtls-test/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -O1 -g -w $(MBEDTLS_TEST_INCLUDES) \
	  -fsanitize=address,undefined -fno-omit-frame-pointer -c $< -o $@

tests/%.o: tests/%.cpp
	$(CXX) $(TEST_CXXFLAGS) $(DEPFLAGS) -Itests -c $< -o $@

samples/%.o: samples/%.cpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -Iinclude -Isamples -c $< -o $@

# Compilability check for the sample module (host headers, no game needed).
check-samples: samples/sample_hitmarker.o
	@echo "== samples compile OK =="
	@rm -f samples/sample_hitmarker.o

test: idl-check check-signer check-sigpack-signer check-sources check-android-sources check-mbedtls check-run check-sigcheck check-warnings check-werror check-examples $(TEST_BINS) build/$(SHARED_NAME) check-samples
	@for b in $(TEST_BINS); do \
	  echo "-- $$b --"; ./$$b "$$b" || exit 1; \
	done
	@echo "-- dlopen ABI --"
	@$(CC) -std=c11 -Wall -O2 -Iinclude tests/dlopen_test.c -ldl -o build/dlopen_test
	@./build/dlopen_test build/$(SHARED_NAME) || exit 1
	@echo "== all tests passed =="

# Sanitizer build: ASan+UBSan host tests. Everything is built into its own
# directory so it never collides with the normal build/, and every binary runs
# Sanitizer build: ASan+UBSan host tests. Everything is built into its own
# directory so it never collides with the normal build/, and every binary runs
# with leak + stack checks enabled.
# `-fno-sanitize=function`: UBSan's function sanitizer (clang only) reads a type
# descriptor 8 bytes BEFORE an indirect-call target. The shim backend's
# trampolines are JIT pages with no such descriptor, so the check reads the page
# before the pool and faults. The check is a C++-type-confusion detector, not a
# memory-safety one, and this library calls raw machine code by design, so it is
# disabled for the sanitizer build. gcc has no equivalent, which is why the g++
# job passed without it.
# The flag is clang-only (gcc rejects the name), so add it only for clang.
SAN_FUNCTION_FLAG := $(if $(findstring clang,$(CXX)),-fno-sanitize=function,)
SAN_CXXFLAGS := -std=c++17 -Wall -Wextra -O1 -g -Iinclude -Ithird_party/ed25519 $(MBEDTLS_INCLUDES) $(LZ4_INCLUDES) $(LUAU_INCLUDES) \
                -DBERRY_THREAD_ASSERT \
                -fsanitize=address,undefined $(SAN_FUNCTION_FLAG) -fno-omit-frame-pointer
SAN_LDFLAGS  := -pthread -fsanitize=address,undefined
SAN_TEST_OBJ := $(patsubst tests/%.cpp,build/san/tests/%.o,$(TEST_SRC))
SAN_BINS     := $(patsubst tests/%.cpp,build/san/test_%,$(TEST_SRC))
SAN_CORE_OBJ := $(filter-out build/san/core/net/tls.o,$(patsubst core/%.cpp,build/san/core/%.o,$(filter core/%.cpp,$(CORE_SRC)))) \
                $(patsubst util/%.cpp,build/san/util/%.o,$(filter util/%.cpp,$(CORE_SRC))) \
                $(patsubst third_party/ed25519/%.c,build/san/third_party/ed25519/%.o,$(ED25519_SRC)) \
                $(patsubst %.cpp,build/san/%.o,$(LUAU_SRC)) \
                $(patsubst %.c,build/san/%.o,$(LZ4_SRC)) \
                $(patsubst %.c,build/san/mbedtls-test/%.o,$(MBEDTLS_SRC)) \
                $(patsubst %.c,build/san/mbedtls-test/%.o,$(MBEDTLS_TEST_EXTRA_SRC)) \
                build/san/testcfg/core/net/tls.o

SAN_TEST_CXXFLAGS := $(subst $(MBEDTLS_INCLUDES),$(MBEDTLS_TEST_INCLUDES),$(SAN_CXXFLAGS))
build/san/tests/%.o: tests/%.cpp | build/san/tests
	$(CXX) $(SAN_TEST_CXXFLAGS) $(DEPFLAGS) -Itests -c $< -o $@

build/san/core/%.o: core/%.cpp | build/san/core
	@mkdir -p $(dir $@)
	$(CXX) $(SAN_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

build/san/util/%.o: util/%.cpp | build/san/util
	@mkdir -p $(dir $@)
	$(CXX) $(SAN_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

build/san/third_party/luau/%.o: third_party/luau/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O1 -g -w $(LUAU_INCLUDES) -fsanitize=address,undefined -fno-omit-frame-pointer -c $< -o $@

build/san/third_party/ed25519/%.o: third_party/ed25519/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -O1 -g -DED25519_NO_SEED -fsanitize=address,undefined -fno-omit-frame-pointer -c $< -o $@

build/san/third_party/lz4/lib/%.o: third_party/lz4/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -O1 -g -w -Ithird_party/lz4/lib -fsanitize=address,undefined -fno-omit-frame-pointer -c $< -o $@

build/san/tests:
	mkdir -p $@
build/san/core:
	mkdir -p $@
build/san/util:
	mkdir -p $@

build/san/test_%: build/san/tests/%.o $(SAN_CORE_OBJ)
	$(CXX) $(SAN_LDFLAGS) $^ -o $@

.PHONY: test-sanitize
test-sanitize: $(SAN_BINS)
	@for b in $(SAN_BINS); do \
	  echo "-- $$b --"; ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./$$b "$$b" || exit 1; \
	done
	@echo "== sanitizer tests passed =="

# Shared library build (targets: the injectable .so that host/dlopen).
# Object files land in build/shared/ so tests and .so don't collide.
SHARED_OBJ := $(patsubst %.cpp,build/shared/%.o,$(CORE_SRC)) \
              $(patsubst %.c,build/shared/%.o,$(ED25519_SRC)) \
              $(patsubst %.c,build/shared/%.o,$(MBEDTLS_SRC)) \
              $(patsubst %.c,build/shared/%.o,$(LZ4_SRC)) \
              $(patsubst %.cpp,build/shared/%.o,$(LUAU_SRC))

build/shared/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(SHARED_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

build/shared/third_party/luau/%.o: third_party/luau/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -fPIC -fvisibility=hidden -w $(LUAU_INCLUDES) -c $< -o $@

build/shared/third_party/ed25519/%.o: third_party/ed25519/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -O2 -fPIC -fvisibility=hidden -DED25519_NO_SEED -c $< -o $@

build/shared/third_party/mbedtls/library/%.o: third_party/mbedtls/library/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -O2 -fPIC -fvisibility=hidden -w $(MBEDTLS_INCLUDES) -c $< -o $@

build/shared/third_party/lz4/%.o: third_party/lz4/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -O2 -fPIC -fvisibility=hidden -w -Ithird_party/lz4/lib -c $< -o $@

build/$(SHARED_NAME): $(SHARED_OBJ)
	$(CXX) $(SHARED_LDFLAGS) $(SHARED_OBJ) -o $@

lib: build/$(SHARED_NAME)
	@echo "== built $(SHARED_NAME) =="

# The standalone voice-relay host (tools/berry-relay.cpp). It links the portable
# core, so it can host the UDP forwarder with no game and no Android. Run
# `make berry-relay` then `./build/berry-relay --help`.
RELAY_OBJ := build/tools/berry-relay.o
build/tools/berry-relay.o: tools/berry-relay.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -Itools -c $< -o $@

build/berry-relay: $(RELAY_OBJ) $(CORE_OBJ)
	$(CXX) $(LDFLAGS) $(RELAY_OBJ) $(CORE_OBJ) -ldl -o $@

.PHONY: berry-relay
berry-relay: build/berry-relay
	@echo "== built build/berry-relay =="

# The developer CLI (tools/berry-run.cpp): `test`, `run` and `replay` modes over
# the portable core. It links the shipping (client-only) mbedTLS objects, so it
# is a plain host binary with no game and no Android. Run `make berry-run` then
# `./build/berry-run --help`.
RUN_OBJ := build/tools/berry-run.o
build/tools/berry-run.o: tools/berry-run.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -Itools -c $< -o $@

build/berry-run: $(RUN_OBJ) $(CORE_OBJ)
	$(CXX) $(LDFLAGS) $(RUN_OBJ) $(CORE_OBJ) -ldl -o $@

.PHONY: berry-run
berry-run: build/berry-run
	@echo "== built build/berry-run =="

# The offline signature-table validator (tools/berry-sigcheck.cpp): proves a
# signature table against an extracted module in CI, before release. It links
# the portable core so it uses the same parser/scanner the runtime uses.
SIGCHECK_OBJ := build/tools/berry-sigcheck.o
build/tools/berry-sigcheck.o: tools/berry-sigcheck.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -Itools -c $< -o $@

build/berry-sigcheck: $(SIGCHECK_OBJ) $(CORE_OBJ)
	$(CXX) $(LDFLAGS) $(SIGCHECK_OBJ) $(CORE_OBJ) -ldl -o $@

.PHONY: berry-sigcheck
berry-sigcheck: build/berry-sigcheck
	@echo "== built build/berry-sigcheck =="

# Self-check: the tool must read a build-id from a real ELF and match a pattern
# against it. Uses the tool's own binary as the ELF.
.PHONY: check-sigcheck
check-sigcheck: build/berry-sigcheck
	@id=$$(./build/berry-sigcheck build/berry-sigcheck --build-id) || { echo "check-sigcheck: no build-id"; exit 1; }; \
	[ -n "$$id" ] || { echo "check-sigcheck: empty build-id"; exit 1; }; \
	./build/berry-sigcheck build/berry-sigcheck --pattern "7F 45 4C 46" >/dev/null || { echo "check-sigcheck: ELF magic did not match"; exit 1; }; \
	echo "== berry-sigcheck OK =="

# End-to-end check for the developer CLI. It drives the real binary through
# every mode and asserts the exit code, so the argument parsing, the verified
# loader path, the test runner and the bounded replay reader are all exercised
# as a user would exercise them. A refusal (wrong anchor, tampered manifest,
# malformed replay) must exit non-zero, which is what makes the fail-closed
# claims in docs/RUN.md testable.
.PHONY: check-run
check-run: build/berry-run
	@tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT; \
	./build/berry-run version >/dev/null || { echo "check-run: version failed"; exit 1; }; \
	./build/berry-run --help >/dev/null || { echo "check-run: --help failed"; exit 1; }; \
	printf 'return { test_ok = function() test.eq(1, 1) end,\n test_bad = function() test.eq(1, 2) end }\n' > "$$tmp/spec.luau"; \
	./build/berry-run test "$$tmp/spec.luau" >/dev/null 2>&1 && { echo "check-run: a failing suite exited 0"; exit 1; }; \
	./build/berry-run test "$$tmp/spec.luau" --filter test_ok >/dev/null 2>&1 || { echo "check-run: a passing filter exited non-zero"; exit 1; }; \
	./build/berry-run replay tests/fixtures/berry-run/sample.replay >/dev/null 2>&1 || { echo "check-run: replay read failed"; exit 1; }; \
	./build/berry-run replay tests/fixtures/berry-run/sample.replay --expect-build 1 >/dev/null 2>&1 && { echo "check-run: a wrong fingerprint was accepted"; exit 1; }; \
	head -c 64 /dev/urandom > "$$tmp/bad.replay"; \
	./build/berry-run replay "$$tmp/bad.replay" >/dev/null 2>&1 && { echo "check-run: a garbage replay was accepted"; exit 1; }; \
	echo "== berry-run CLI OK =="

# IDL codegen: one description generates the C++ binding table, the TS/Luau
# type definitions and the API docs. `idl-check` fails when they drift.
.PHONY: idl idl-check
idl:
	@python3 tools/idlgen.py

idl-check:
	@python3 tools/idlgen.py --self-test
	@python3 tools/idlgen.py --golden-test
	@python3 tools/idlgen.py --check

# Breaking-change lint (improvement 77): diff the current IDL against the last
# released copy (api/berry.idl.prev, kept by the release job). A binding removed
# without a `removed` version, or an arity/return/capability change, fails. A
# missing .prev (a fresh checkout) is not an error.
.PHONY: idl-breaking
idl-breaking:
	@if [ -f api/berry.idl.prev ]; then \
	  python3 tools/idlgen.py --breaking-diff api/berry.idl.prev api/berry.idl \
	    && echo "== no breaking script-API changes ==" \
	    || { echo "idl-breaking: breaking script-API change; bump the major version and mark it"; exit 1; }; \
	else \
	  echo "idl-breaking: no api/berry.idl.prev to diff against; skipped"; \
	fi

# The Python signer and the C++ verifier must agree on the canonical payload
# byte for byte, or every published script would be rejected at load time.
.PHONY: check-signer
check-signer:
	@python3 tools/check-signer.py

# The signature-pack signer and the C++ pack verifier must agree on the
# canonical signed payload byte for byte, or every published pack would
# be rejected at load time.
.PHONY: check-sigpack-signer
check-sigpack-signer:
	@python3 tools/check-sigpack-signer.py

# libFuzzer targets (improvement 97). Needs clang; on a machine without it this
# is a no-op, so `make fuzz` never breaks the host build. The targets share the
# pure parsers the deterministic tests already cover.
.PHONY: fuzz
fuzz:
	@if command -v clang++ >/dev/null 2>&1; then \
	  clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined \
	    -Iinclude -Ithird_party/ed25519 -Ithird_party/mbedtls/include \
	    -DMBEDTLS_CONFIG_FILE='"mbedtls/mbedtls_config.h"' -Ithird_party/lz4/lib \
	    -Ithird_party/luau/Common/include -Ithird_party/luau/Ast/include \
	    -Ithird_party/luau/Compiler/include -Ithird_party/luau/VM/include \
	    fuzz/fuzz_targets.cpp $(CORE_OBJ) -o build/berry_fuzz \
	    && ./build/berry_fuzz -max_total_time=60 -rss_limit_mb=2048 \
	  || echo "fuzz: clang++ not available; skipped"; \
	else \
	  echo "fuzz: clang++ not available; skipped"; \
	fi

# A CycloneDX SBOM (improvement 100).
.PHONY: sbom
sbom:
	@python3 tools/gen-sbom.py > build/sbom.cdx.json && echo "== wrote build/sbom.cdx.json =="

# Script lint/format rule pack (improvement 75). Runs the project's .luaurc lint
# config through luau-analyze and the stylua config through stylua when either
# tool is present; a machine without them skips rather than failing. The .luaurc
# itself is always validated (it must be valid JSON), so a typo in the config is
# caught even without the tools.
# Cross-build a few portable suites for arm64 and run them under qemu-user in
# CI (improvement 96), so the arm64 decode paths are exercised on a real arm64
# ABI. The cross compiler is passed in so the vendored C is cross-built too:
# overriding only CXX leaves the C objects host-format and the link then fails
# with "file in wrong format". Run from a clean tree (CI checks out fresh), or
# the host objects are reused.
ARM64_CXX ?= aarch64-linux-gnu-g++
ARM64_CC  ?= aarch64-linux-gnu-gcc
ARM64_TEST_BINS := build/test_test_sigscan build/test_test_x86_length \
                   build/test_test_url build/test_test_metrics \
                   build/test_test_inflate build/test_test_netquota \
                   build/test_test_conn_pool build/test_test_require
.PHONY: arm64-portable-tests
arm64-portable-tests:
	$(MAKE) -j"$(shell nproc)" CXX=$(ARM64_CXX) CC=$(ARM64_CC) $(ARM64_TEST_BINS)

# A strict warning pass over first-party code (improvement 98): -Wshadow and
# -Wconversion catch real bugs (a shadowed name, a narrowing conversion) that
# the default flag set hides. Vendored code is excluded (it is not ours to
# change). A clean pass is the baseline; a new warning fails.
.PHONY: check-warnings
check-warnings:
	@fail=0; \
	for f in core/hooks/*.cpp core/net/net.cpp core/net/room_token.cpp \
	         core/net/lan_transport.cpp core/script/async.cpp core/script/timers.cpp \
	         core/events/metrics.cpp core/lifecycle/crash.cpp \
	         core/lifecycle/configfile.cpp util/*.cpp; do \
	  out=$$($(CXX) -std=c++17 -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -fsyntax-only \
	    -Iinclude -Ithird_party/ed25519 -Ithird_party/mbedtls/include \
	    -DMBEDTLS_CONFIG_FILE='"mbedtls/mbedtls_config.h"' -Ithird_party/lz4/lib \
	    -Ithird_party/luau/Common/include -Ithird_party/luau/Ast/include \
	    -Ithird_party/luau/Compiler/include -Ithird_party/luau/VM/include "$$f" 2>&1 \
	    | grep -E "warning:" || true); \
	  if [ -n "$$out" ]; then echo "check-warnings: $$f"; echo "$$out"; fail=1; fi; \
	done; \
	[ $$fail -eq 0 ] && echo "== warning baseline clean ==" || exit 1

# -Werror over the first-party files whose warning baseline is clean
# (improvement 98). Vendored code and the few files still carrying a third-party
# header warning are excluded; this is the gate that keeps a new warning from
# landing in the files already clean.
.PHONY: check-werror
check-werror:
	@fail=0; \
	for f in core/hooks/backend_shim.cpp core/hooks/x86_length.cpp \
	         core/hooks/sigscan.cpp core/hooks/hook_chain.cpp \
	         core/net/net.cpp core/net/inflate.cpp core/net/netquota.cpp \
	         core/net/room_token.cpp core/net/lan_transport.cpp \
	         core/net/tls.cpp core/events/metrics.cpp \
	         core/lifecycle/crash.cpp core/lifecycle/configfile.cpp \
	         core/script/async.cpp core/script/timers.cpp util/logger.cpp \
	         util/mem.cpp; do \
	  $(CXX) -std=c++17 -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Werror -fsyntax-only \
	    -Iinclude -Ithird_party/ed25519 -Ithird_party/mbedtls/include \
	    -DMBEDTLS_CONFIG_FILE='"mbedtls/mbedtls_config.h"' -Ithird_party/lz4/lib \
	    -Ithird_party/luau/Common/include -Ithird_party/luau/Ast/include \
	    -Ithird_party/luau/Compiler/include -Ithird_party/luau/VM/include "$$f" \
	    || { echo "check-werror: $$f"; fail=1; }; \
	done; \
	[ $$fail -eq 0 ] && echo "== -Werror clean ==" || exit 1

# Every example in examples/*.luau is a runnable berry-run test suite, so CI
# proves the documentation still works (improvement 76).
.PHONY: check-examples
check-examples: build/berry-run
	@for f in examples/*.luau; do \
	  [ -e "$$f" ] || continue; \
	  echo "-- $$f --"; \
	  ./build/berry-run test "$$f" || { echo "check-examples: $$f failed"; exit 1; }; \
	done
	@echo "== examples OK =="

.PHONY: lint-scripts
lint-scripts:
	@python3 -c "import json,sys; json.load(open('.luaurc')); print('== .luaurc is valid JSON ==')"
	@if command -v luau-analyze >/dev/null 2>&1; then \
	  echo "== luau-analyze =="; \
	  find samples -name '*.luau' -exec luau-analyze {} + || true; \
	else echo "lint-scripts: luau-analyze not installed; skipped"; fi
	@if command -v stylua >/dev/null 2>&1; then \
	  echo "== stylua --check =="; stylua --check . || true; \
	else echo "lint-scripts: stylua not installed; skipped"; fi

# Every .cpp under core/ and util/ must be in tools/sources.mk, so the host
# build and the Android build cannot silently drift apart.
.PHONY: check-sources
check-sources:
	@missing=0; \
	for f in $$(find core util -name '*.cpp' | sort); do \
	  grep -qE "(^| )$$f( \\\\|$$)" tools/sources.mk || { echo "check-sources: $$f is not in tools/sources.mk"; missing=1; }; \
	done; \
	for f in $(ED25519_SRC) $(MBEDTLS_SRC) $(LUAU_SRC); do \
	  [ -f "$$f" ] || { echo "check-sources: $$f is missing"; missing=1; }; \
	done; \
	[ $$missing -eq 0 ] || exit 1; \
	echo "== source manifest complete =="

# The Android build reads the same tools/sources.mk as the host build, so the
# two cannot drift; this target proves it, and proves the device-only units and
# the new subsystems are actually in the list. It needs no NDK (it uses
# build-android.sh --sources-only), so it runs as part of `make test`. A full
# `tools/build-android.sh <NDK>` is what produces the .so; that needs an NDK and
# is left to the release job.
.PHONY: check-android-sources
check-android-sources:
	@python3 tools/check-android-sources.py

# Keeps the vendored mbedTLS honest: the version pin (script, build_info.h,
# PROVENANCE.md), the trimmed client-only config, and the compiled-vs-excluded
# unit set. `--upstream` additionally asks GitHub for the newest 3.6 release;
# that needs the network, so it is not part of `make test`.
.PHONY: check-mbedtls
check-mbedtls:
	@python3 tools/check-mbedtls.py

.PHONY: check-mbedtls-upstream
check-mbedtls-upstream:
	@python3 tools/check-mbedtls.py --upstream

# Convenience: build the on-device .so when an NDK is present. Not part of
# `make test` (it needs the NDK and a cross toolchain); `make check-android`
# builds it and reports the artefact.
.PHONY: check-android
check-android:
	@bash tools/build-android.sh && ls -l build-android/*/libBerryAPI.so

clean:
	rm -rf build core/events/*.o core/hooks/*.o core/modules/*.o core/lifecycle/*.o \
	       core/net/*.o core/script/*.o util/*.o tests/*.o samples/*.o \
	       third_party/ed25519/*.o third_party/luau/*.o \
	       third_party/mbedtls/library/*.o
	rm -f core/events/*.d core/hooks/*.d core/modules/*.d core/lifecycle/*.d \
	      core/net/*.d core/script/*.d util/*.d tests/*.d samples/*.d \
	      third_party/ed25519/*.d third_party/luau/*.d \
	      third_party/mbedtls/library/*.d

.PHONY: all test lib clean