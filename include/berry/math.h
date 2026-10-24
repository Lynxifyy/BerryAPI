#pragma once
#include "berry/export.h"
#include "berry/types.h"
#include <cmath>
#include <algorithm>
#include <limits>

namespace Berry {

//----------------------------------------------------------------------------
// Vec3 / Vec2 / Vec4 — tiny, allocation-free arithmetic.
//----------------------------------------------------------------------------

struct BERRY_API Vec3 {
  f32 x, y, z;

  Vec3() : x(0), y(0), z(0) {}
  Vec3(f32 x_, f32 y_, f32 z_) : x(x_), y(y_), z(z_) {}

  static Vec3 Zero() { return Vec3(0, 0, 0); }
  static Vec3 One()  { return Vec3(1, 1, 1); }
  static Vec3 UnitX() { return Vec3(1, 0, 0); }
  static Vec3 UnitY() { return Vec3(0, 1, 0); }
  static Vec3 UnitZ() { return Vec3(0, 0, 1); }
  static Vec3 Up()    { return Vec3(0, 1, 0); }
  static Vec3 Forward() { return Vec3(0, 0, 1); }

  Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
  Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
  Vec3 operator*(const Vec3& o) const { return Vec3(x * o.x, y * o.y, z * o.z); }
  Vec3 operator*(f32 s) const { return Vec3(x * s, y * s, z * s); }
  Vec3 operator/(f32 s) const { return Vec3(x / s, y / s, z / s); }
  Vec3 operator-() const { return Vec3(-x, -y, -z); }

  Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  Vec3& operator*=(f32 s) { x *= s; y *= s; z *= s; return *this; }

  bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
  bool operator!=(const Vec3& o) const { return !(*this == o); }

  f32 Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
  Vec3 Cross(const Vec3& o) const {
    return Vec3(y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x);
  }
  f32 LengthSq() const { return Dot(*this); }
  f32 Length() const { return std::sqrt(LengthSq()); }
  f32 Distance(const Vec3& o) const { return (*this - o).Length(); }
  f32 DistanceSq(const Vec3& o) const { return (*this - o).LengthSq(); }

  Vec3 Normalized() const {
    f32 l = Length();
    if (l < 1e-8f) return Vec3(0, 0, 0);
    return *this * (1.0f / l);
  }
  Vec3 ClampedLength(f32 maxLen) const {
    f32 l = Length();
    if (l > maxLen) return (*this) / l * maxLen;
    return *this;
  }

  Vec3 Lerp(const Vec3& o, f32 t) const {
    return Vec3(x + (o.x - x) * t, y + (o.y - y) * t, z + (o.z - z) * t);
  }
  Vec3 Floor() const {
    return Vec3(std::floor(x), std::floor(y), std::floor(z));
  }
  Vec3 Ceil() const {
    return Vec3(std::ceil(x), std::ceil(y), std::ceil(z));
  }

  f32 MinComponent() const { return std::min(x, std::min(y, z)); }
  f32 MaxComponent() const { return std::max(x, std::max(y, z)); }
  /// Component-wise minimum.
  Vec3 Min(const Vec3& o) const {
    return Vec3(std::min(x, o.x), std::min(y, o.y), std::min(z, o.z));
  }
  /// Component-wise maximum.
  Vec3 Max(const Vec3& o) const {
    return Vec3(std::max(x, o.x), std::max(y, o.y), std::max(z, o.z));
  }

  Vec3 Angle() const {  // pitch / yaw in degrees (Minecraft convention)
    f32 pitch = -std::asin(y / std::max(Length(), 1e-6f)) * 180.0f / M_PI;
    f32 yaw   = std::atan2(-x, z) * 180.0f / M_PI;
    return Vec3(pitch, yaw, 0);
  }
};

struct BERRY_API Vec2 {
  f32 x, y;
  Vec2() : x(0), y(0) {}
  Vec2(f32 x_, f32 y_) : x(x_), y(y_) {}
  Vec2 operator+(const Vec2& o) const { return Vec2(x + o.x, y + o.y); }
  Vec2 operator-(const Vec2& o) const { return Vec2(x - o.x, y - o.y); }
  Vec2 operator*(f32 s) const { return Vec2(x * s, y * s); }
  f32 Length() const { return std::sqrt(x * x + y * y); }
  f32 Dot(const Vec2& o) const { return x * o.x + y * o.y; }
  Vec2 Normalized() const {
    f32 l = Length();
    if (l < 1e-8f) return *this;
    return Vec2(x / l, y / l);
  }
};

struct BERRY_API Vec4 {
  f32 x, y, z, w;
  Vec4() : x(0), y(0), z(0), w(1) {}
  Vec4(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
  static Vec4 Color(f32 r, f32 g, f32 b, f32 a = 1.0f) { return Vec4(r, g, b, a); }
  Vec4 operator*(f32 s) const { return Vec4(x * s, y * s, z * s, w * s); }
};

//----------------------------------------------------------------------------
// AABB — axis aligned bounding box.
//----------------------------------------------------------------------------

struct BERRY_API Aabb {
  Vec3 min, max;

  Aabb() : min(), max() {}
  Aabb(const Vec3& min_, const Vec3& max_) : min(min_), max(max_) {}

  static Aabb FromMinMax(const Vec3& mn, const Vec3& mx) { return Aabb(mn, mx); }
  static Aabb Around(const Vec3& center, const Vec3& halfExtents) {
    return Aabb(center - halfExtents, center + halfExtents);
  }
  static Aabb FromPoints(const Vec3* pts, size_t n) {
    if (n == 0) return Aabb();
    Vec3 mn = pts[0], mx = pts[0];
    for (size_t i = 1; i < n; ++i) {
      mn = mn.Min(pts[i]);
      mx = mx.Max(pts[i]);
    }
    return Aabb(mn, mx);
  }

  Vec3 Center() const { return (min + max) * 0.5f; }
  Vec3 Size() const { return max - min; }
  f32   Volume() const { Vec3 s = Size(); return s.x * s.y * s.z; }

  bool Contains(const Vec3& p) const {
    return p.x >= min.x && p.x <= max.x &&
           p.y >= min.y && p.y <= max.y &&
           p.z >= min.z && p.z <= max.z;
  }
  bool Intersects(const Aabb& o) const {
    return !(max.x < o.min.x || o.max.x < min.x ||
             max.y < o.min.y || o.max.y < min.y ||
             max.z < o.min.z || o.max.z < min.z);
  }

  /// Grows the box to include point p.
  void Encapsulate(const Vec3& p) {
    min = min.Min(p);
    max = max.Max(p);
  }

  Aabb Expanded(f32 m) const {
    return Aabb(min - Vec3(m, m, m), max + Vec3(m, m, m));
  }
};

//----------------------------------------------------------------------------
// Misc.
//----------------------------------------------------------------------------

namespace Mathf {
constexpr f32 PI = 3.14159265358979323846f;
constexpr f32 TWO_PI = 6.2831853071795864769f;
constexpr f32 HALF_PI = 1.5707963267948966192f;
constexpr f32 DEG2RAD = PI / 180.0f;
constexpr f32 RAD2DEG = 180.0f / PI;

inline f32 Clamp(f32 v, f32 lo, f32 hi) {
  return std::max(lo, std::min(v, hi));
}
inline f32 Lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
inline f32 SmoothStep(f32 t) { return t * t * (3.0f - 2.0f * t); }
inline f32 SinDeg(f32 deg) { return std::sin(deg * DEG2RAD); }
inline f32 CosDeg(f32 deg) { return std::cos(deg * DEG2RAD); }
inline u32 HashCoords(i32 x, i32 y, i32 z) {
  u32 h = (u32)(x * 73856093) ^ (u32)(y * 19349663) ^ (u32)(z * 83492791);
  h ^= h >> 13;
  h *= 0x5bd1e995;
  return h ^ (h >> 15);
}

}  // namespace Mathf

}  // namespace Berry