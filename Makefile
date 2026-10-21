# BerryAPI host build (self-contained; no external deps).
# Produces tests/berry_tests binary that runs every module's test suite.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -g -Iinclude -DBERRY_THREAD_ASSERT
LDFLAGS  ?= -pthread

# The injectable shared library (android/linux target build).
# Compile with -fPIC and -fvisibility=hidden; only BERRY_API symbols escape.
SHARED_CXXFLAGS ?= -std=c++17 -O2 -fPIC -fvisibility=hidden -DBERRY_BUILD_SHARED -Iinclude
SHARED_LDFLAGS  ?= -shared -pthread -ldl
SHARED_NAME     ?= libBerryAPI.so

CORE_SRC := \
  core/events/event.cpp \
  core/hooks/sigscan.cpp \
  core/hooks/backend_shim.cpp \
  core/modules/module.cpp \
  core/modules/combat.cpp \
  core/modules/render.cpp \
  core/modules/render_backend.cpp \
  core/modules/services.cpp \
  core/modules/world.cpp \
  core/modules/hud.cpp \
  core/modules/input.cpp \
  core/modules/pvp.cpp \
  core/lifecycle/engine.cpp \
  core/lifecycle/thread.cpp \
  core/net/net.cpp \
  core/script/crypto.cpp \
  core/script/manifest.cpp \
  core/script/runtime.cpp \
  core/script/vm_ref.cpp \
  core/script/host.cpp \
  core/script/reload.cpp \
  core/script/async.cpp \
  util/logger.cpp \
  util/mem.cpp

TEST_SRC := \
  tests/test_math.cpp \
  tests/test_modules.cpp \
  tests/test_net.cpp \
  tests/test_render.cpp \
  tests/test_sample.cpp \
  tests/test_sigscan.cpp \
  tests/test_thread.cpp \
  tests/test_fuzz.cpp \
  tests/test_manifest.cpp \
  tests/test_script.cpp \
  tests/test_reload.cpp \
  tests/test_async.cpp \
  tests/test_idl.cpp \
  tests/test_hud.cpp

CORE_OBJ := $(CORE_SRC:.cpp=.o)
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

tests/%.o: tests/%.cpp
	$(CXX) $(CXXFLAGS) -Itests -c $< -o $@

samples/%.o: samples/%.cpp
	$(CXX) $(CXXFLAGS) -Iinclude -Isamples -c $< -o $@

# Compilability check for the sample module (host headers, no game needed).
check-samples: samples/sample_hitmarker.o
	@echo "== samples compile OK =="
	@rm -f samples/sample_hitmarker.o

test: idl-check $(TEST_BINS) build/$(SHARED_NAME) check-samples
	@for b in build/test_test_math build/test_test_modules build/test_test_net build/test_test_render build/test_test_sample build/test_test_sigscan build/test_test_thread build/test_test_fuzz build/test_test_manifest build/test_test_script build/test_test_reload build/test_test_async build/test_test_idl build/test_test_hud; do \
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
SAN_CXXFLAGS := -std=c++17 -Wall -Wextra -O1 -g -Iinclude -DBERRY_THREAD_ASSERT \
                -fsanitize=address,undefined -fno-omit-frame-pointer
SAN_LDFLAGS  := -pthread -fsanitize=address,undefined
SAN_TEST_OBJ := $(patsubst tests/%.cpp,build/san/tests/%.o,$(TEST_SRC))
SAN_BINS     := $(patsubst tests/%.cpp,build/san/test_%,$(TEST_SRC))
SAN_CORE_OBJ := $(patsubst core/%.cpp,build/san/core/%.o,$(filter core/%.cpp,$(CORE_SRC))) \
                $(patsubst util/%.cpp,build/san/util/%.o,$(filter util/%.cpp,$(CORE_SRC)))

build/san/tests/%.o: tests/%.cpp | build/san/tests
	$(CXX) $(SAN_CXXFLAGS) -Itests -c $< -o $@

build/san/core/%.o: core/%.cpp | build/san/core
	@mkdir -p $(dir $@)
	$(CXX) $(SAN_CXXFLAGS) -c $< -o $@

build/san/util/%.o: util/%.cpp | build/san/util
	@mkdir -p $(dir $@)
	$(CXX) $(SAN_CXXFLAGS) -c $< -o $@

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
SHARED_OBJ := $(patsubst %.cpp,build/shared/%.o,$(CORE_SRC))

build/shared/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(SHARED_CXXFLAGS) -c $< -o $@

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

clean:
	rm -rf build core/events/*.o core/hooks/*.o core/modules/*.o core/lifecycle/*.o \
	       core/net/*.o core/script/*.o util/*.o tests/*.o samples/*.o

.PHONY: all test lib clean