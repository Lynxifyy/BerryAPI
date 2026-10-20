#pragma once
// BerryAPI C ABI configuration structs.
//
// The injector entry points are plain C so any loader tool (dlopen, JNI,
// preloader, or a C host) can build a config value without dragging in C++
// headers or relying on the C++ ABI. Every public config struct starts with
//   uint32_t size;        // sizeof(*this), verified by the library
//   uint32_t abi_version; // BERRY_ABI_VERSION, verified by the library
//
// A caller initializes the struct, sets `size = sizeof(cfg)`,
// `abi_version = BERRY_ABI_VERSION`, fills the fields it understands, and
// passes the pointer. The library rejects mismatches (fail closed).

#include "berry/version.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 0 = default (keeps the library's compiled-in level); 1..5 explicit levels.
enum BerryLogLevelC {
  BerryLogDefaultC = 0,
  BerryLogTraceC = 1,
  BerryLogDebugC = 2,
  BerryLogInfoC = 3,
  BerryLogWarnC = 4,
  BerryLogErrorC = 5
};

// Canonical injector configuration. C-compatible; every field has a safe
// default so zero-initializing the struct is equivalent to defaults.
typedef struct BerryConfigC {
  uint32_t size;         // must be sizeof(BerryConfigC)
  uint32_t abi_version;  // must be BERRY_ABI_VERSION
  int log_level;         // BerryLogLevelC; -1 keeps the default (info)
  int capture_log;       // 0/1 keep a capture buffer for the log viewer
  int auto_attach_modules;  // 0/1 register + attach the four strict modules
  int enable_render;        // 0/1 allow the render overlay
} BerryConfigC;

#ifdef __cplusplus
}
#endif