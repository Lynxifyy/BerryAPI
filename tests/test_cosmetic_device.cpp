// Host tests for the on-device cosmetic render path: the bone source, the
// player-model hook (through the shim backend) and the render seam that binds
// resolved items to uploaded geometry.
#include "berry/cosmetic.h"
#include "berry/cosmetic_assets.h"
#include "berry/cosmetic_device.h"
#include "berry/hooks.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

/// A mesh backend that records what it was asked to draw, so the seam's
/// resolve-and-submit path is asserted without GL.
class RecordingMeshBackend final : public ICosmeticMeshBackend {
 public:
  bool init = false;
  bool isReady = true;
  int draws = 0;
  int resets = 0;
  CameraState lastCam;
  bool gotFrame = false;
  std::vector<GeometryId> geometries;
  std::vector<TextureId> textures;

  bool Init() override { init = true; return isReady; }
  bool ready() const override { return isReady; }
  const char* name() const override { return "recording"; }
  void SetFrame(const CameraState& cam, const Vec2&) override {
    lastCam = cam;
    gotFrame = true;
  }
  void DrawMesh(const CosmeticDrawItem& item, const MeshData&,
                const TextureData* tex) override {
    ++draws;
    geometries.push_back(item.geometry);
    textures.push_back(tex ? item.texture : 0);
  }
  void Reset() override { ++resets; }
};

/// A reader whose bone transforms are a fixed rig, bumped by the hook.
struct FakeReader {
  u64 revision = 1;
  int calls = 0;
  PlayerModelBoneReader Make() {
    PlayerModelBoneReader r;
    r.boneTransform = [this](EntityId, Bone bone, CosmeticBoneTransform* out) {
      ++calls;
      if (bone == Bone::None) return false;
      out->position = Vec3(1.0f, 2.0f, 3.0f);
      out->rotation = Vec3(0, 90, 0);
      out->scale = 1.0f;
      out->valid = true;
      return true;
    };
    r.poseRevision = [this](EntityId, u64* out) {
      if (out) *out = revision;
      return true;
    };
    return r;
  }
};

/// A camera source that reports a fixed camera.
class FixedCameraSource final : public CosmeticCameraSource {
 public:
  CameraState cam;
  bool known = true;
  bool Camera(CameraState* out) const override {
    if (!known) return false;
    if (out) *out = cam;
    return true;
  }
};

MeshData OneTriangle() {
  MeshData m;
  m.name = "tri";
  MeshVertex a, b, c;
  a.position = Vec3(0, 0, 0);
  a.normal = Vec3(0, 0, 1);
  b.position = Vec3(1, 0, 0);
  b.normal = Vec3(0, 0, 1);
  c.position = Vec3(0, 1, 0);
  c.normal = Vec3(0, 0, 1);
  m.vertices = {a, b, c};
  m.indices = {0, 1, 2};
  m.bounds = Aabb(Vec3(0, 0, 0), Vec3(1, 1, 0));
  return m;
}

}  // namespace

//----------------------------------------------------------------------------
// Bone source
//----------------------------------------------------------------------------

static void TestSkeletonSourceFailClosed() {
  PlayerModelSkeletonSource src;
  CosmeticBoneTransform bt;
  // No reader: nothing resolves.
  CHECK(!src.hasReader());
  CHECK(!src.BoneTransform(EntityId{1, 1}, Bone::Root, &bt));
  u64 rev = 0;
  CHECK(!src.PoseRevision(EntityId{1, 1}, &rev));

  FakeReader reader;
  src.SetReader(reader.Make());
  CHECK(src.hasReader());
  CHECK(src.BoneTransform(EntityId{1, 1}, Bone::Head, &bt));
  CHECK(bt.valid);
  CHECK(bt.position.x == 1.0f && bt.position.y == 2.0f);
  CHECK(src.PoseRevision(EntityId{1, 1}, &rev));
  CHECK(rev == 1);

  // Clearing the reader fails closed again.
  src.SetReader(PlayerModelBoneReader{});
  CHECK(!src.BoneTransform(EntityId{1, 1}, Bone::Head, &bt));
}

//----------------------------------------------------------------------------
// Player-model hook (through the shim backend)
//----------------------------------------------------------------------------

static void ModelRender(void* self, void* entity, void* ctx) {
  (void)self;
  (void)entity;
  (void)ctx;
  static volatile int sink = 0;
  ++sink;
}

static void TestPlayerModelHook() {
  HookManager::Instance().BackendEmulateShim();
  IHook* backend = HookManager::Instance().Backend();
  CHECK(backend != nullptr);
  CHECK(backend->CanHook());

  PlayerModelHook hook;
  // No reader: install is refused rather than a no-op detour.
  CHECK(!hook.Install((void*)&ModelRender, backend));

  FakeReader reader;
  hook.SetReader(reader.Make());
  CHECK(hook.Install((void*)&ModelRender, backend));
  CHECK(hook.installed());

  // A second hook cannot claim the same singleton target.
  PlayerModelHook second;
  second.SetReader(reader.Make());
  CHECK(!second.Install((void*)&ModelRender, backend));

  hook.Uninstall();
  CHECK(!hook.installed());

  // After the first is released, a new one may install.
  CHECK(second.Install((void*)&ModelRender, backend));
  second.Uninstall();
}

//----------------------------------------------------------------------------
// Render seam
//----------------------------------------------------------------------------

static void TestRenderSeamResolvesAndDraws() {
  CosmeticAssetStore::Instance().Clear();
  const GeometryId g = CosmeticAssetStore::Instance().AddMesh(OneTriangle());
  CHECK(g != 0);

  RecordingMeshBackend backend;
  DeviceCosmeticRenderSeam seam(&CosmeticAssetStore::Instance(), &backend);

  CosmeticDrawItem item;
  item.geometry = g;
  item.texture = 0;
  item.position = Vec3(0, 0, 0);
  item.scale = 1.0f;
  seam.SubmitGeometry(item);
  CHECK(backend.draws == 1);
  CHECK(seam.drawn() == 1 && seam.dropped() == 0);

  // An unresolved geometry handle is dropped, not drawn.
  CosmeticDrawItem missing = item;
  missing.geometry = g + 1000;
  seam.SubmitGeometry(missing);
  CHECK(backend.draws == 1);
  CHECK(seam.dropped() == 1);

  // A null geometry handle is dropped too.
  CosmeticDrawItem none = item;
  none.geometry = 0;
  seam.SubmitGeometry(none);
  CHECK(seam.dropped() == 2);

  // A backend that is not ready draws nothing.
  backend.isReady = false;
  seam.SubmitGeometry(item);
  CHECK(backend.draws == 1);
  CHECK(seam.dropped() == 3);
  backend.isReady = true;

  seam.Reset();
  CHECK(backend.resets == 1);
  CHECK(seam.drawn() == 0 && seam.dropped() == 0);
}

static void TestRenderSeamFrameCamera() {
  CosmeticAssetStore::Instance().Clear();
  const GeometryId g = CosmeticAssetStore::Instance().AddMesh(OneTriangle());

  RecordingMeshBackend backend;
  DeviceCosmeticRenderSeam seam(&CosmeticAssetStore::Instance(), &backend);
  CameraState cam;
  cam.valid = true;
  cam.width = 1280;
  cam.height = 720;
  seam.BeginFrame(cam, Vec2(1280, 720));
  CHECK(backend.gotFrame);
  CHECK(backend.lastCam.valid && backend.lastCam.width == 1280);

  CosmeticDrawItem item;
  item.geometry = g;
  item.scale = 1.0f;
  seam.SubmitGeometry(item);
  CHECK(backend.draws == 1);
}

// The service brackets a present with the seam frame when a camera source is
// bound, and passes the frame's camera through.
static void TestServiceDrivesSeamFrame() {
  CosmeticRegistry::Instance().Clear();
  CosmeticRegistry::Instance().RegisterBuiltins();
  CosmeticAssetStore::Instance().Clear();

  RecordingMeshBackend backend;
  DeviceCosmeticRenderSeam seam(&CosmeticAssetStore::Instance(), &backend);
  FixedCameraSource cam;
  cam.cam.valid = true;
  cam.cam.width = 800;
  cam.cam.height = 600;

  CosmeticService svc;
  FakeReader reader;
  PlayerModelSkeletonSource skel;
  skel.SetReader(reader.Make());
  svc.SetSkeletonSource(&skel);
  svc.SetRenderSeam(&seam);
  svc.SetCameraSource(&cam);

  // The built-in cape has no geometry handle, so it resolves but the seam drops
  // it — the point here is that the frame camera reached the backend.
  CHECK(svc.Equip("builtin.cape.red"));
  svc.PresentLocal(EntityId{1, 1});
  CHECK(backend.gotFrame);
  CHECK(backend.lastCam.width == 800);

  // With no camera source the seam still opens a frame, but with an invalid
  // camera (a real present that simply cannot project).
  svc.SetCameraSource(nullptr);
  backend.gotFrame = false;
  svc.PresentLocal(EntityId{1, 1});
  CHECK(backend.gotFrame);
  CHECK(!backend.lastCam.valid);
}

int main() {
  TestSkeletonSourceFailClosed();
  TestPlayerModelHook();
  TestRenderSeamResolvesAndDraws();
  TestRenderSeamFrameCamera();
  TestServiceDrivesSeamFrame();
  if (g_fail == 0) printf("test_cosmetic_device: ALL OK\n");
  return g_fail;
}
