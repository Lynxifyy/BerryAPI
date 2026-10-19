#pragma once
// BerryAPI shared library / visibility handling.
// BerryAPI is a shared library (libBerryAPI.so) loaded into the target
// process (Minecraft Bedrock native executable) via dlopen.

#if defined(_WIN32)
  #if defined(BERRY_BUILD_SHARED)
    #define BERRY_API __declspec(dllexport)
  #elif defined(BERRY_USE_SHARED)
    #define BERRY_API __declspec(dllimport)
  #else
    #define BERRY_API
  #endif
#else // GCC / Clang (Android NDK, Linux desktop harness)
  #define BERRY_EXPORT __attribute__((visibility("default")))
  #define BERRY_HIDDEN __attribute__((visibility("hidden")))
  #if defined(BERRY_BUILD_SHARED)
    #define BERRY_API BERRY_EXPORT
  #else
    #define BERRY_API
  #endif
#endif

#define BERRY_INLINE inline
#define BERRY_UNUSED(x) (void)(x)
#define BERRY_PACKED __attribute__((packed))
#define BERRY_NOINLINE __attribute__((noinline))
#define BERRY_USED __attribute__((used))

// ABI helpers.
#define BERRY_FASTCALL
#define BERRY_CDECL
#define BERRY_STDCALL

// Location tags for library location lookups.
#define BERRY_LIB_SELF "libBerryAPI.so"
#define BERRY_LIB_BEDROCK "libminecraftpe.so"