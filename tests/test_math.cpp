#include "berry/math.h"
#include <cstdio>
#include <cmath>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECK_NEAR(a, b, eps) do { \
  double da = (double)(a), db = (double)(b); \
  if (std::fabs(da - db) > (eps)) { \
    printf("FAIL %s:%d: %s == %s (got %.6f, want %.6f)\n", __FILE__, __LINE__, #a, #b, da, db); \
    ++g_fail; } } while (0)

int main() {
  // Vec3
  Vec3 a(1, 2, 3), b(4, 5, 6);
  CHECK_NEAR((a + b).x, 5, 1e-6);
  CHECK_NEAR((a - b).z, -3, 1e-6);
  CHECK_NEAR(a.Dot(b), 32, 1e-6);
  CHECK_NEAR((a * 2.0f).y, 4, 1e-6);
  Vec3 n = Vec3(3, 0, 4).Normalized();
  CHECK_NEAR(n.Length(), 1, 1e-6);
  CHECK_NEAR(Vec3(1, 0, 0).Cross(Vec3(0, 1, 0)).z, 1, 1e-6);

  // Ray-AABB (slab/amy method — bool hit + optional distance)
  Aabb box(Vec3(0, 0, 0), Vec3(2, 2, 2));
  Ray ray(Vec3(-5, 1, 1), Vec3(1, 0, 0));
  f32 d = 0;
  CHECK(ray.IntersectAabb(box, &d));
  CHECK_NEAR(d, 5, 1e-6);
  CHECK(!ray.IntersectAabb(Aabb(Vec3(5, 5, 5), Vec3(6, 6, 6))));

  // Entity AABB approx for ESP
  CHECK(box.Contains(Vec3(1, 1, 1)));
  CHECK(!box.Contains(Vec3(3, 3, 3)));

  if (g_fail == 0) printf("test_math: ALL OK\n");
  return g_fail;
}