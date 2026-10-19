#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Berry {

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using f32 = float;
using f64 = double;

// Address widths for pointer arithmetic on the target (aarch64).
using ptr_t   = uintptr_t;
using uptr    = uintptr_t;
using ptrdiff = intptr_t;
using page_t  = uintptr_t;

template <typename T>
using TS = std::shared_ptr<T>;
using String  = std::string;

// GameTick = one simulation step (20/s by default).
using GameTick = i64;

}  // namespace Berry