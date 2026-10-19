# BerryAPI host build (self-contained; no external deps).
# Produces tests/berry_tests binary that runs every module's test suite.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -g -Iinclude
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
  core/modules/services.cpp \
  core/modules/world.cpp \
  core/lifecycle/engine.cpp \
  core/net/net.cpp \
  util/logger.cpp \
  util/mem.cpp

TEST_SRC := \
  tests/test_math.cpp \
  tests/test_modules.cpp \
  tests/test_net.cpp

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

test: $(TEST_BINS) build/$(SHARED_NAME)
	@for b in build/test_test_math build/test_test_modules build/test_test_net; do \
	  echo "-- $$b --"; ./$$b "$$b" || exit 1; \
	done
	@echo "-- dlopen ABI --"
	@$(CC) -std=c11 -Wall -O2 tests/dlopen_test.c -ldl -o build/dlopen_test
	@./build/dlopen_test build/$(SHARED_NAME) || exit 1
	@echo "== all tests passed =="

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

clean:
	rm -rf build core/events/*.o core/hooks/*.o core/modules/*.o core/lifecycle/*.o \
	       core/net/*.o util/*.o tests/*.o

.PHONY: all test lib clean