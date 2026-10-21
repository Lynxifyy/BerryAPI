# BerryAPI host build (self-contained; no external deps).
# Produces tests/berry_tests binary that runs every module's test suite.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -g -Iinclude -Ithird_party/ed25519 -DBERRY_THREAD_ASSERT
LDFLAGS  ?= -pthread

# The injectable shared library (android/linux target build).
# Compile with -fPIC and -fvisibility=hidden; only BERRY_API symbols escape.
SHARED_CXXFLAGS ?= -std=c++17 -O2 -fPIC -fvisibility=hidden -DBERRY_BUILD_SHARED -Iinclude -Ithird_party/ed25519
SHARED_LDFLAGS  ?= -shared -pthread -ldl
SHARED_NAME     ?= libBerryAPI.so

include tools/sources.mk

# Vendored C (Ed25519) is compiled as C99 into the same object set as the C++
# core, so both the host tests and the injectable .so carry it.
ED25519_OBJ := $(ED25519_SRC:.c=.o)
CORE_OBJ := $(CORE_SRC:.cpp=.o) $(ED25519_OBJ)
TEST_OBJ := $(TEST_SRC:.cpp=.o)

# Each suite links against the core into its own binary.
TEST_BINS := $(TEST_SRC:tests/%.cpp=build/test_%)

all: test

build:
	mkdir -p build

build/test_%: tests/%.o $(CORE_OBJ) | build
	$(CXX) $(CXXFLAGS) $(CORE_OBJ) tests/$*.o -o $@ $(LDFLAGS)

# Compile rules
core/%.o: core/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

util/%.o: util/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Vendored Ed25519: C99, and no seed source (key generation is
# caller-supplied, so nothing reads /dev/urandom inside the library).
third_party/ed25519/%.o: third_party/ed25519/%.c
	$(CC) -std=c99 -O2 -Wall -Wextra -DED25519_NO_SEED -c $< -o $@

tests/%.o: tests/%.cpp
	$(CXX) $(CXXFLAGS) -Itests -c $< -o $@

samples/%.o: samples/%.cpp
	$(CXX) $(CXXFLAGS) -Iinclude -Isamples -c $< -o $@

# Compilability check for the sample module (host headers, no game needed).
check-samples: samples/sample_hitmarker.o
	@echo "== samples compile OK =="
	@rm -f samples/sample_hitmarker.o

test: idl-check check-signer check-sources $(TEST_BINS) build/$(SHARED_NAME) check-samples
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
SAN_CXXFLAGS := -std=c++17 -Wall -Wextra -O1 -g -Iinclude -Ithird_party/ed25519 \
                -DBERRY_THREAD_ASSERT \
                -fsanitize=address,undefined -fno-omit-frame-pointer
SAN_LDFLAGS  := -pthread -fsanitize=address,undefined
SAN_TEST_OBJ := $(patsubst tests/%.cpp,build/san/tests/%.o,$(TEST_SRC))
SAN_BINS     := $(patsubst tests/%.cpp,build/san/test_%,$(TEST_SRC))
SAN_CORE_OBJ := $(patsubst core/%.cpp,build/san/core/%.o,$(filter core/%.cpp,$(CORE_SRC))) \
                $(patsubst util/%.cpp,build/san/util/%.o,$(filter util/%.cpp,$(CORE_SRC))) \
                $(patsubst third_party/ed25519/%.c,build/san/third_party/ed25519/%.o,$(ED25519_SRC))

build/san/tests/%.o: tests/%.cpp | build/san/tests
	$(CXX) $(SAN_CXXFLAGS) -Itests -c $< -o $@

build/san/core/%.o: core/%.cpp | build/san/core
	@mkdir -p $(dir $@)
	$(CXX) $(SAN_CXXFLAGS) -c $< -o $@

build/san/util/%.o: util/%.cpp | build/san/util
	@mkdir -p $(dir $@)
	$(CXX) $(SAN_CXXFLAGS) -c $< -o $@

build/san/third_party/ed25519/%.o: third_party/ed25519/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -O1 -g -DED25519_NO_SEED -fsanitize=address,undefined -fno-omit-frame-pointer -c $< -o $@

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
              $(patsubst %.c,build/shared/%.o,$(ED25519_SRC))

build/shared/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(SHARED_CXXFLAGS) -c $< -o $@

build/shared/third_party/ed25519/%.o: third_party/ed25519/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -O2 -fPIC -fvisibility=hidden -DED25519_NO_SEED -c $< -o $@

build/$(SHARED_NAME): $(SHARED_OBJ)
	$(CXX) $(SHARED_LDFLAGS) $(SHARED_OBJ) -o $@

lib: build/$(SHARED_NAME)
	@echo "== built $(SHARED_NAME) =="

# IDL codegen: one description generates the C++ binding table, the TS/Luau
# type definitions and the API docs. `idl-check` fails when they drift.
.PHONY: idl idl-check
idl:
	@python3 tools/idlgen.py

idl-check:
	@python3 tools/idlgen.py --self-test
	@python3 tools/idlgen.py --check

# The Python signer and the C++ verifier must agree on the canonical payload
# byte for byte, or every published script would be rejected at load time.
.PHONY: check-signer
check-signer:
	@python3 tools/check-signer.py

# Every .cpp under core/ and util/ must be in tools/sources.mk, so the host
# build and the Android build cannot silently drift apart.
.PHONY: check-sources
check-sources:
	@missing=0; \
	for f in $$(find core util -name '*.cpp' | sort); do \
	  grep -qE "(^| )$$f( \\\\|$$)" tools/sources.mk || { echo "check-sources: $$f is not in tools/sources.mk"; missing=1; }; \
	done; \
	for f in $(ED25519_SRC); do \
	  [ -f "$$f" ] || { echo "check-sources: $$f is missing"; missing=1; }; \
	done; \
	[ $$missing -eq 0 ] || exit 1; \
	echo "== source manifest complete =="

clean:
	rm -rf build core/events/*.o core/hooks/*.o core/modules/*.o core/lifecycle/*.o \
	       core/net/*.o core/script/*.o util/*.o tests/*.o samples/*.o \
	       third_party/ed25519/*.o

.PHONY: all test lib clean