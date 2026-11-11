// Simple shadow, renderer side (fast/oot3d/player_shadow.h): the mod's opacity
// and light direction, the projection onto the floor, the view-row patch of a
// PICA vertex uniform block and the frame bridge.
#include "fast/oot3d/player_shadow.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using namespace Fast::Oot3d;
using Vec3 = std::array<float, 3>;
using Mat4 = std::array<float, 16>;

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

bool Near(float a, float b, float tolerance = 1.0e-4F) { return std::abs(a - b) <= tolerance; }
bool Near(const Vec3 &a, const Vec3 &b, float tolerance = 1.0e-4F) {
  return Near(a[0], b[0], tolerance) && Near(a[1], b[1], tolerance) && Near(a[2], b[2], tolerance);
}

Vec3 Apply(const Mat4 &m, const Vec3 &v) {
  Vec3 out{};
  for (size_t row = 0; row < 3U; ++row)
    out[row] = m[row * 4U] * v[0] + m[row * 4U + 1U] * v[1] + m[row * 4U + 2U] * v[2] + m[row * 4U + 3U];
  return out;
}

void TestOpacity() {
  Require(Near(PlayerShadowOpacity(255U, 0.0F), 144.0F / 255.0F), "on the floor: 180 * 0.8");
  Require(Near(PlayerShadowOpacity(255U, -20.0F), 144.0F / 255.0F), "below the floor: as on it");
  Require(Near(PlayerShadowOpacity(255U, 30.0F), 65.0F / 255.0F), "30 units: both fades");
  Require(Near(PlayerShadowOpacity(255U, 60.0F), 12.0F / 255.0F), "60 units: the near fade's bottom");
  Require(Near(PlayerShadowOpacity(255U, 200.0F), 144.0F / 255.0F), "high: the rise takes over");
  Require(PlayerShadowOpacity(0U, 0.0F) == 0.0F, "no actor alpha, no shadow");
  Require(PlayerShadowOpacity(128U, 0.0F) < PlayerShadowOpacity(255U, 0.0F), "the actor's alpha scales it");
}

Fast::Renderer3ds::PicaSceneDirectionalLights Lights(std::initializer_list<std::pair<Vec3, float>> lights) {
  Fast::Renderer3ds::PicaSceneDirectionalLights out;
  for (const auto &[direction, energy] : lights) {
    auto &light = out.Lights[out.Count++];
    light.DirectionTowardSource = direction;
    light.Color = {energy, energy, energy};
  }
  return out;
}

void TestLightDirection() {
  const Vec3 up{0.0F, 1.0F, 0.0F};
  const float s = std::sqrt(0.5F);
  Require(Near(PlayerShadowLightDirection(nullptr, up, 0.0F), up), "no lights: straight down");
  const auto strong = Lights({{{1.0F, 1.0F, 0.0F}, 1.0F}});
  Require(Near(PlayerShadowLightDirection(&strong, up, 0.0F), {s, s, 0.0F}), "a strong light");
  const auto weak = Lights({{{1.0F, 1.0F, 0.0F}, 0.1F}});
  const auto tilted = PlayerShadowLightDirection(&weak, up, 0.0F);
  Require(tilted[1] > s && tilted[0] > 0.0F && Near(tilted[0] * tilted[0] + tilted[1] * tilted[1], 1.0F),
          "a weak light tilts toward the vertical");
  Require(Near(PlayerShadowLightDirection(&strong, up, 140.0F), up), "risen 140 units: vertical");
  const auto midway = PlayerShadowLightDirection(&strong, up, 90.0F);
  Require(midway[1] > s && midway[1] < 1.0F, "risen 90 units: between");
  const auto ignored = Lights({{{1.0F, 0.0F, 0.0F}, 0.01F}});
  Require(Near(PlayerShadowLightDirection(&ignored, up, 0.0F), up), "a light under 0.02 does not count");
  const auto low = Lights({{{1.0F, 0.05F, 0.0F}, 1.0F}});
  const auto clamped = PlayerShadowLightDirection(&low, up, 0.0F);
  Require(Near(clamped[1], kPlayerShadowMinimumElevation) && clamped[0] > 0.0F, "a grazing light is raised");
  const auto twoLights = Lights({{{1.0F, 1.0F, 0.0F}, 1.0F}, {{-1.0F, 1.0F, 0.0F}, 1.0F}});
  Require(Near(PlayerShadowLightDirection(&twoLights, up, 0.0F), up), "opposed lights of one energy");
}

void TestProjection() {
  PlayerShadowFrame frame;
  frame.FloorPoint = {10.0F, 5.0F, -3.0F};
  const float s = std::sqrt(0.5F);
  const Vec3 light{s, s, 0.0F};
  const Mat4 projection = PlayerShadowProjection(frame, light);
  const float plane = 5.0F + kPlayerShadowLift;
  Require(Near(Apply(projection, {0.0F, plane, 7.0F}), {0.0F, plane, 7.0F}), "a point of the plane stays");
  Require(Near(Apply(projection, {4.0F, plane + 4.0F, 1.0F}), {0.0F, plane, 1.0F}), "along the light to the plane");
  PlayerShadowFrame sloped = frame;
  sloped.FloorNormal = {0.0F, 0.8F, 0.6F};
  const Mat4 onSlope = PlayerShadowProjection(sloped, light);
  const Vec3 landed = Apply(onSlope, {12.0F, 30.0F, 2.0F});
  const Vec3 lifted{10.0F, 5.0F + 0.8F * kPlayerShadowLift, -3.0F + 0.6F * kPlayerShadowLift};
  Require(Near(0.8F * (landed[1] - lifted[1]) + 0.6F * (landed[2] - lifted[2]), 0.0F, 1.0e-3F),
          "onto a sloped floor");
}

// Column-major world-to-view: a turn about y and a translation.
Mat4 ViewColumnMajor() {
  const float c = std::cos(0.7F), s = std::sin(0.7F);
  const Mat4 rows{c, 0.0F, s, 12.0F, 0.0F, 1.0F, 0.0F, -40.0F, -s, 0.0F, c, -300.0F, 0.0F, 0.0F, 0.0F, 1.0F};
  Mat4 columns{};
  for (size_t row = 0; row < 4U; ++row)
    for (size_t column = 0; column < 4U; ++column) columns[column * 4U + row] = rows[row * 4U + column];
  return columns;
}

Mat4 RowsOf(const Mat4 &columnMajor) {
  Mat4 rows{};
  for (size_t row = 0; row < 4U; ++row)
    for (size_t column = 0; column < 4U; ++column) rows[row * 4U + column] = columnMajor[column * 4U + row];
  return rows;
}

std::vector<uint8_t> Block(const Mat4 &viewRows) {
  std::vector<uint8_t> block(kPicaPackedVertexUniformSize, 0U);
  std::memcpy(block.data() + kPicaPackedVertexFloatOffset + kPlayerShadowViewRow * kPicaPackedVec4Bytes,
              viewRows.data(), 12U * sizeof(float));
  return block;
}

Mat4 ViewRowsOf(const std::vector<uint8_t> &block) {
  Mat4 rows{};
  std::memcpy(rows.data(), block.data() + kPicaPackedVertexFloatOffset + kPlayerShadowViewRow * kPicaPackedVec4Bytes,
              12U * sizeof(float));
  rows[15] = 1.0F;
  return rows;
}

void TestPatch() {
  const Mat4 viewColumns = ViewColumnMajor();
  const Mat4 view = RowsOf(viewColumns);
  PlayerShadowFrame frame;
  frame.FloorPoint = {0.0F, 2.0F, 0.0F};
  const float s = std::sqrt(0.5F);
  const Mat4 projection = PlayerShadowProjection(frame, {s, s, 0.0F});
  const Vec3 world{3.0F, 20.0F, -8.0F};
  const Vec3 expected = Apply(view, Apply(projection, world));

  auto viewInRows = Block(view); // bones give world positions, the rows are the camera
  Require(PatchPlayerShadowViewRows(viewInRows, viewColumns, projection), "camera in the view rows");
  Require(Near(Apply(ViewRowsOf(viewInRows), world), expected, 1.0e-3F), "flattened, camera in the rows");

  Mat4 identity{};
  identity[0] = identity[5] = identity[10] = identity[15] = 1.0F;
  auto viewInBones = Block(identity); // bones give view positions, the rows are the identity
  Require(PatchPlayerShadowViewRows(viewInBones, viewColumns, projection), "camera in the bones");
  Require(Near(Apply(ViewRowsOf(viewInBones), Apply(view, world)), expected, 1.0e-3F), "flattened, camera in the bones");

  std::vector<uint8_t> shortBlock(kPicaPackedVertexFloatOffset + 5U * kPicaPackedVec4Bytes, 7U);
  const auto before = shortBlock;
  Require(!PatchPlayerShadowViewRows(shortBlock, viewColumns, projection) && shortBlock == before, "a short block");
  Mat4 projective = viewColumns;
  projective[3] = 0.5F; // row 3 column 0, column-major
  auto untouched = Block(view);
  const auto copy = untouched;
  Require(!PatchPlayerShadowViewRows(untouched, projective, projection) && untouched == copy, "a projective view");
}

void TestBridge() {
  PlayerShadowBridge bridge;
  Require(!bridge.RendererReady(), "no renderer draws it until one says so");
  bridge.SetRendererReady(true);
  Require(bridge.RendererReady(), "deko3d with its program loaded");
  PlayerShadowFrame first;
  first.Actor = 0x08123450U;
  first.FloorPoint = {0.0F, 0.0F, 0.0F};
  first.Height = 0.0F;
  first.Opacity = 0.5F;
  PlayerShadowFrame second = first;
  second.FloorPoint = {10.0F, 2.0F, 0.0F};
  second.Height = 20.0F;
  second.Opacity = 0.3F;
  bridge.PublishFrame(1U, first);
  bridge.PublishFrame(2U, second);
  Fast::Renderer3ds::PicaFrameTemporalSample sample;
  sample.CurrentSourceFrameId = 2U;
  sample.PreviousSourceFrameId = 1U;
  sample.Alpha = 1.0F;
  bridge.SelectSample(sample);
  Require(bridge.Selected() == second, "the current frame");
  sample.Alpha = 0.5F;
  bridge.SelectSample(sample);
  const auto mixed = bridge.Selected();
  Require(mixed && Near(mixed->FloorPoint, {5.0F, 1.0F, 0.0F}) && Near(mixed->Height, 10.0F) &&
              Near(mixed->Opacity, 0.4F),
          "an in-between frame blends");
  sample.HistoryReset = true;
  bridge.SelectSample(sample);
  Require(bridge.Selected() == second, "no blend across a reset");
  sample.HistoryReset = false;
  PlayerShadowFrame other = second;
  other.Actor = 0x08200000U;
  bridge.PublishFrame(3U, other);
  sample.CurrentSourceFrameId = 3U;
  sample.PreviousSourceFrameId = 2U;
  bridge.SelectSample(sample);
  Require(bridge.Selected() == other, "no blend between two actors");
  bridge.PublishFrame(4U, std::nullopt);
  sample.CurrentSourceFrameId = 4U;
  bridge.SelectSample(sample);
  Require(!bridge.Selected().has_value(), "a frame without a shadow");
  sample.CurrentSourceFrameId = 9U;
  bridge.SelectSample(sample);
  Require(!bridge.Selected().has_value(), "an unknown frame");
  bridge.PublishFrame(2U, first); // an older id: a reset
  sample.CurrentSourceFrameId = 3U;
  bridge.SelectSample(sample);
  Require(!bridge.Selected().has_value(), "frames before a reset are gone");
}

} // namespace

int main() {
  TestOpacity();
  TestLightDirection();
  TestProjection();
  TestPatch();
  TestBridge();
  std::puts("oot3d_player_shadow_tests: ok");
  return 0;
}
