# BerryAPI CMake build (improvement 99).
#
# An alternative to the Makefile, for a developer who wants CMake + Ninja +
# ccache. It builds the same artefacts: the host test binaries, the injectable
# shared library, and the tools. The source lists come from tools/sources.mk
# (the single source of truth the Makefile and the Android build also read), so
# the two builds cannot drift.
#
#   cmake -S . -B build-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-cmake
#   ctest --test-dir build-cmake
#
# `make test` remains the canonical gate (it also runs the IDL, signer and
# provenance checks); this target is for a fast incremental edit/build loop.

cmake_minimum_required(VERSION 3.16)
project(BerryAPI CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- parse tools/sources.mk for the canonical source lists ------------------
# A tiny CMake script extracts CORE_SRC / TEST_SRC and the vendored lists, so
# adding a file to sources.mk is picked up here too.
function(load_sources_mk)
  file(READ "${CMAKE_SOURCE_DIR}/tools/sources.mk" _mk)
  # Collapse line continuations, then pull each VAR := ... block.
  string(REGEX REPLACE "\\\\\n" " " _mk "${_mk}")
  foreach(_var CORE_SRC TEST_SRC ED25519_SRC LZ4_SRC MBEDTLS_SRC)
    string(REGEX MATCH "${_var} := ([^\n]*)" _m "${_mk}")
    set(_list "${CMAKE_MATCH_1}")
    separate_arguments(_list)
    set(${_var} "${_list}" PARENT_SCOPE)
  endforeach()
endfunction()
load_sources_mk()

set(BERRY_INCLUDES
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/ed25519
    ${CMAKE_SOURCE_DIR}/third_party/lz4/lib
    ${CMAKE_SOURCE_DIR}/third_party/luau/Common/include
    ${CMAKE_SOURCE_DIR}/third_party/luau/Ast/include
    ${CMAKE_SOURCE_DIR}/third_party/luau/Compiler/include
    ${CMAKE_SOURCE_DIR}/third_party/luau/VM/include
    ${CMAKE_SOURCE_DIR}/third_party/mbedtls/include)
add_compile_definitions(MBEDTLS_CONFIG_FILE="mbedtls/mbedtls_config.h")

# Vendored Luau sources (the VM + compiler + AST).
file(GLOB_RECURSE LUAU_SRC CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/third_party/luau/Ast/src/*.cpp
     ${CMAKE_SOURCE_DIR}/third_party/luau/Compiler/src/*.cpp
     ${CMAKE_SOURCE_DIR}/third_party/luau/VM/src/*.cpp)

# The core library (everything in CORE_SRC plus the vendored C).
add_library(berry_core STATIC
    ${CORE_SRC} ${ED25519_SRC} ${LZ4_SRC} ${MBEDTLS_SRC} ${LUAU_SRC})
target_include_directories(berry_core PUBLIC ${BERRY_INCLUDES})
target_compile_options(berry_core PRIVATE -Wall -Wextra)
# Vendored code is not warning-clean under our flags; silence it there only.
set_source_files_properties(${LUAU_SRC} ${LZ4_SRC} ${MBEDTLS_SRC}
    PROPERTIES COMPILE_OPTIONS "-w")
find_package(Threads REQUIRED)
target_link_libraries(berry_core PUBLIC Threads::Threads dl)

# The injectable shared library.
add_library(BerryAPI SHARED ${CORE_SRC} ${ED25519_SRC} ${LZ4_SRC}
            ${MBEDTLS_SRC} ${LUAU_SRC})
target_include_directories(BerryAPI PUBLIC ${BERRY_INCLUDES})
target_compile_options(BerryAPI PRIVATE -fPIC -fvisibility=hidden -DBERRY_BUILD_SHARED)
set_target_properties(BerryAPI PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VERSION ${PROJECT_VERSION})

# One test binary per tests/test_*.cpp, mirroring the Makefile.
foreach(_test_src ${TEST_SRC})
  get_filename_component(_name ${_test_src} NAME_WE)
  add_executable(${_name} ${_test_src})
  target_include_directories(${_name} PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(${_name} PRIVATE berry_core)
  add_test(NAME ${_name} COMMAND ${_name} ${_name})
endforeach()

# The tools.
foreach(_tool berry-run berry-relay berry-sigcheck)
  if(EXISTS ${CMAKE_SOURCE_DIR}/tools/${_tool}.cpp)
    add_executable(${_tool} tools/${_tool}.cpp)
    target_include_directories(${_tool} PRIVATE ${CMAKE_SOURCE_DIR}/tools)
    target_link_libraries(${_tool} PRIVATE berry_core)
  endif()
endforeach()
