// Pure parts of the deko3d backend (docs/DEKO3D_PROGRESS.md): what runs
// without a GPU is tested here on the host.
#include "fast/backends/gfx_deko3d.h"
#include "fast/backends/gfx_deko3d_pica_pipeline.h"
#include "fast/backends/gfx_deko3d_pica_plan.h"
#include "fast/oot3d/scene_presentation_policy.h"
#include "fast/backends/gfx_deko3d_programs.h"
#include "fast/backends/gfx_deko3d_shader_pack.h"
#include "fast/backends/gfx_deko3d_ui_plan.h"

#include <array>
#include <cstring>
#include <string_view>
#include <vector>

#include <cstdio>
#include <cstdlib>

namespace {

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

void TestRendererConfigSelection() {
  using Fast::Deko3dRendererConfigSelects;
  Require(Deko3dRendererConfigSelects("deko3d"), "deko3d selects");
  Require(Deko3dRendererConfigSelects("deko3d\n"), "with line break");
  Require(Deko3dRendererConfigSelects("  DEKO3D \r\n"), "uppercase and spaces");
  Require(Deko3dRendererConfigSelects("deko3d\ngl"), "only the first line counts");
  Require(!Deko3dRendererConfigSelects(""), "empty file = GL");
  Require(!Deko3dRendererConfigSelects("gl"), "gl = GL");
  Require(!Deko3dRendererConfigSelects("opengl"), "opengl = GL");
  Require(!Deko3dRendererConfigSelects("deko3d2"), "name with suffix = GL");
  Require(!Deko3dRendererConfigSelects("\ndeko3d"), "empty first line = GL");
  // Off the Switch it never selects (the file isn't even read).
  Require(!Fast::Deko3dRendererSelected(), "host: always GL");
}

void Put32(std::vector<uint8_t> &bytes, size_t at, uint32_t value) {
  std::memcpy(bytes.data() + at, &value, sizeof(value));
}

std::vector<uint8_t> MakePack(uint64_t keyA, uint64_t keyB) {
  // header 16 + 2 entries 24 + two 8-byte DKSH stubs
  std::vector<uint8_t> bytes(16 + 48 + 16, 0);
  Put32(bytes, 0, Fast::Deko3d::kShaderPackMagic);
  Put32(bytes, 4, Fast::Deko3d::kShaderPackVersion);
  Put32(bytes, 8, 2);
  Fast::Deko3d::ShaderPackEntry entries[2] = {{keyA, 64, 8, 0, 0}, {keyB, 72, 8, 1, 0}};
  std::memcpy(bytes.data() + 16, entries, sizeof(entries));
  Put32(bytes, 64, Fast::Deko3d::kDkshMagic);
  Put32(bytes, 72, Fast::Deko3d::kDkshMagic);
  bytes[76] = 0x42;
  return bytes;
}

void TestShaderPack() {
  using namespace Fast::Deko3d;
  // Same vector as tools/oot3d/deko3d_shader_pack.py --selftest.
  Require(ShaderTextKey(ShaderStage::Fragment, "abc") == 0x593522965D44E8B2ULL,
          "FNV key same as the pack tool's");
  Require(ShaderTextKey(ShaderStage::Vertex, "abc") != ShaderTextKey(ShaderStage::Fragment, "abc"),
          "the stage is part of the key");
  ShaderPackIndex index;
  std::string error;
  Require(index.Parse(MakePack(10, 20), &error), "valid pack");
  Require(index.Count() == 2, "two entries");
  ShaderStage stage = ShaderStage::Vertex;
  const auto blob = index.Find(20, &stage);
  Require(blob.size() == 8 && blob[4] == 0x42 && stage == ShaderStage::Fragment, "finds entry 20");
  Require(index.Find(15).empty(), "missing key");
  Require(!index.Parse(MakePack(20, 10), &error) && index.Count() == 0, "out of order fails");
  auto bad = MakePack(10, 20);
  Put32(bad, 72, 0);
  Require(!index.Parse(bad, &error), "blob without DKSH fails");
  auto shortPack = MakePack(10, 20);
  shortPack.resize(70);
  Require(!index.Parse(shortPack, &error), "blob past the end fails");
  Require(!index.Parse({}, &error), "empty fails");
}

// p6f on the console: lighting shaders with 4096 bytes of scratch and the queue with the
// default of 2048 crashed the GPU when leaving Link's house.
void TestDkshScratch() {
  using namespace Fast::Deko3d;
  std::vector<uint8_t> dksh(0x80, 0);
  Put32(dksh, 0, kDkshMagic);
  Put32(dksh, 16, 0x40); // programs_off
  Put32(dksh, 20, 1);    // num_programs
  Put32(dksh, 0x40 + 8, 105);
  Put32(dksh, 0x40 + 20, 4096);
  Require(DkshPerWarpScratchBytes(dksh) == 4096U, "reads per_warp_scratch_sz");
  Require(kPerWarpScratchBytes >= 4096U && kPerWarpScratchBytes % 0x200U == 0U,
          "the queue covers the pack's shaders (multiple of 0x200)");
  Put32(dksh, 20, 0);
  Require(!DkshPerWarpScratchBytes(dksh).has_value(), "no programs fails");
  Put32(dksh, 20, 1);
  Put32(dksh, 16, 0x7C);
  Require(!DkshPerWarpScratchBytes(dksh).has_value(), "header past the end fails");
  Put32(dksh, 0, 0);
  Require(!DkshPerWarpScratchBytes(dksh).has_value(), "no DKSH fails");
}

// p6d-p6j: binding the 16 vertex buffers wrote past the command block
// (deko3d 0.5.0 reserves 5 words per buffer and writes 6) and the next block
// overwrote it: "GPU rejected command list" when leaving Link's house.
void TestCommandGuard() {
  using namespace Fast::Deko3d;
  Require(DekoBindVtxBuffersOverrunWords(kPicaVertexAttributeCount) == 16U, "16 buffers overrun 16 words");
  Require(kCommandGuardBytes >= 16U * 4U && kCommandGuardBytes % 4U == 0U, "the slack covers the overrun");
}

void TestProgramPick() {
  using namespace Fast::Deko3d;
  Require(PickPicaProgram(true, true, true) == PicaProgramPick::Specialized, "specialized beats uber");
  Require(PickPicaProgram(true, true, false) == PicaProgramPick::Specialized, "specialized without uber");
  Require(PickPicaProgram(true, false, true) == PicaProgramPick::Uber, "specialized missing: uber");
  Require(PickPicaProgram(true, false, false) == PicaProgramPick::NoFragment, "no fragment at all: skip");
  Require(PickPicaProgram(false, true, true) == PicaProgramPick::NoVertex, "no vertex: skip even with fragment");
  PicaProgramPickCounters counters;
  counters.Count(PicaProgramPick::Uber);
  counters.Count(PicaProgramPick::Uber);
  counters.Count(PicaProgramPick::NoVertex);
  Require(counters.Uber == 2 && counters.NoVertex == 1 && counters.Specialized == 0, "counters per pick");
}

// Drawing regression: each text is translated/looked up in the pack only once
// per identity, including when missing (otherwise each draw without DKSH would redo the
// ~31 KB translation on the render thread).
void TestStageCacheResolvesOnce() {
  using namespace Fast::Deko3d;
  struct FakeShader {
    int Id;
  };
  const FakeShader shader{7};
  PicaStageCache<FakeShader> cache;
  const auto present = Fast::Renderer3ds::IdentifyPicaShaderSource("void main() {}");
  const auto absent = Fast::Renderer3ds::IdentifyPicaShaderSource("void main() { discard; }");
  int calls = 0;
  for (int draw = 0; draw < 5; ++draw) {
    Require(cache.Get(present, [&] { ++calls; return &shader; }) == &shader, "finds the shader");
    Require(cache.Get(absent, [&]() -> const FakeShader* { ++calls; return nullptr; }) == nullptr,
            "missing stays missing");
  }
  Require(calls == 2 && cache.Resolutions() == 2 && cache.Size() == 2, "one resolution per identity");
  cache.Clear();
  Require(cache.Size() == 0, "Clear empties");
}

// Package 4: the same rules as the GL backend (ToPicaGlWrap, PicaGlMinFilter,
// LOD range) so deko3d samples the same.
void TestSamplerAndTextureKeys() {
  using namespace Fast::Deko3d;
  Require(MapPicaWrap(0) == PicaWrap::ClampToEdge, "wrap 0 = clamp to edge");
  Require(MapPicaWrap(2) == PicaWrap::Repeat && MapPicaWrap(6) == PicaWrap::Repeat &&
              MapPicaWrap(7) == PicaWrap::Repeat, "wrap 2/6/7 = repeat");
  Require(MapPicaWrap(3) == PicaWrap::MirroredRepeat, "wrap 3 = mirrored");
  Require(MapPicaWrap(1) == PicaWrap::Unsupported && MapPicaWrap(4) == PicaWrap::Unsupported &&
              MapPicaWrap(9) == PicaWrap::Unsupported, "border/asymmetric/invalid = unsupported");
  Fast::Renderer3ds::PicaTextureView view;
  view.Width = 128;
  view.Height = 64;
  view.MinLinear = true;
  view.MagLinear = true;
  view.MipLinear = true;
  view.MinMipLevel = 9;
  view.MaxMipLevel = 7;
  view.NativeWrapS = 2;
  view.NativeWrapT = 3;
  view.LodBiasRaw = -128;
  auto key = MakePicaSamplerKey(view, 4);
  Require(key.Mip == PicaMipFilter::Linear, "linear mip with chain");
  Require(key.MaxLevel == 3, "MAX_LEVEL limited to the uploaded chain (min(7, 4-1))");
  Require(key.MinLevel == 3, "BASE_LEVEL doesn't exceed MAX_LEVEL");
  Require(key.WrapS == PicaWrap::Repeat && key.WrapT == PicaWrap::MirroredRepeat && key.LodBiasRaw == -128,
          "wrap and bias");
  Require(MakePicaSamplerKey(view, 1).Mip == PicaMipFilter::None, "no mip: mip filter off");
  PicaSamplerKeyHash samplerHash;
  Require(samplerHash(key) == samplerHash(MakePicaSamplerKey(view, 4)), "stable hash");
  const auto textureKey = MakePicaTextureKey(view, 0x1234);
  auto other = view;
  other.NativeWrapS = 0;
  Require(!(textureKey == MakePicaTextureKey(other, 0x1234)), "sampler state is part of the key (as in GL)");
  PicaTextureKeyHash textureHash;
  Require(textureHash(textureKey) == textureHash(MakePicaTextureKey(view, 0x1234)), "stable texture hash");
  Require(ClassifyPicaTexture(view) == PicaTextureUse::Color, "type 0 = color");
  view.NativeType = 2;
  Require(ClassifyPicaTexture(view) == PicaTextureUse::Shadow2D, "type 2 in slot 0 = Shadow2D");
  view.Slot = 1;
  Require(ClassifyPicaTexture(view) == PicaTextureUse::Unsupported, "type 2 outside slot 0 = rejected");
  view.Slot = 0;
  view.NativeType = 3;
  Require(ClassifyPicaTexture(view) == PicaTextureUse::Color, "type 3 in slot 0 = color");
}

void TestMipPlanAndSlots() {
  using namespace Fast::Deko3d;
  std::array<PicaMipPlan, kMaxPicaMipLevels> mips{};
  const uint32_t total = PlanPicaRgba8Mips(64, 32, 3, 256, mips);
  Require(mips[0].Bytes == 64 * 32 * 4 && mips[0].Offset == 0, "level 0");
  Require(mips[1].Width == 32 && mips[1].Height == 16 && mips[1].Offset == 8192, "level 1 aligned");
  Require(mips[2].Width == 16 && mips[2].Height == 8 && mips[2].Offset == 8192 + 2048, "level 2");
  Require(total == 8192 + 2048 + 512, "chain total");
  Require(PlanPicaRgba8Mips(0, 8, 1, 256, mips) == 0 && PlanPicaRgba8Mips(8, 8, 17, 256, mips) == 0,
          "empty/too long chain = 0");
  DescriptorSlots<4> slots(2);
  Require(slots.Allocate() == 2 && slots.Allocate() == 3, "reserved ones skipped");
  Require(slots.Allocate() == 4, "full returns the capacity");
  slots.Free(2);
  Require(slots.Live() == 3 && slots.Allocate() == 2, "free slot reused");
  slots.Free(9);
  Require(slots.Live() == 4, "out-of-range slot ignored");
}

void TestVertexPlan() {
  using namespace Fast::Deko3d;
  using Fast::Renderer3ds::PicaVertexAttributeView;
  using Fast::Renderer3ds::PicaVertexBindingView;
  using Fast::Renderer3ds::PicaVertexFormat;
  const std::vector<uint8_t> a(36, 1), b(10, 2);
  const PicaVertexBindingView bindings[] = {{0, 12, false, a}, {3, 2, false, b}};
  const PicaVertexAttributeView attributes[] = {{0, 0, PicaVertexFormat::Float, 3, 0},
                                                {5, 3, PicaVertexFormat::SignedShort, 1, 0}};
  PicaVertexPlan plan;
  Require(PlanPicaVertices(bindings, attributes, plan), "valid plan");
  Require(plan.BindingOffset[0] == 0 && plan.BindingOffset[3] == 48 && plan.StreamBytes == 58,
          "bindings aligned to 16 in a single block");
  Require(plan.Attributes[0].Kind == PicaAttributeKind::Float && plan.Attributes[0].Components == 3,
          "position float x3");
  Require(plan.Attributes[5].Kind == PicaAttributeKind::SignedShort && plan.Attributes[5].Binding == 3,
          "attribute 5 on binding 3");
  // Drawing regression: a missing attribute reads (0,0,0,1) like a disabled GL array.
  Require(plan.Attributes[1].Kind == PicaAttributeKind::Constant &&
              plan.Attributes[1].Binding == kPicaConstantVertexBinding, "missing = constant buffer");
  Require(plan.AttributeMask == 0x21 && plan.BindingMask == 0x9, "masks");
  const PicaVertexAttributeView orphan[] = {{2, 7, PicaVertexFormat::Float, 4, 0}};
  Require(!PlanPicaVertices(bindings, orphan, plan) && plan.Error != nullptr, "attribute without binding fails");
  const PicaVertexBindingView reserved[] = {{15, 4, false, a}};
  Require(!PlanPicaVertices(reserved, {}, plan), "binding 15 belongs to the constant buffer");
  const PicaVertexAttributeView five[] = {{0, 0, PicaVertexFormat::Float, 5, 0}};
  Require(!PlanPicaVertices(bindings, five, plan), "5 components fails");
  Require(PicaUniformBindBytes(1) == 256 && PicaUniformBindBytes(256) == 256 && PicaUniformBindBytes(6112) == 6144 &&
              PicaUniformBindBytes(1 << 20) == 0x10000, "UBO size aligned and limited to 64 KiB");
}

} // namespace

// Off the Switch the selection is constant (gfx_deko3d_selection.cpp is not
// linked into this test).
namespace Fast {
bool Deko3dRendererSelected() { return false; }
} // namespace Fast

// ---- package 5: targets, state, transfer, scanout ----

void TestPipelineEnums() {
  using namespace Fast::Deko3d;
  using C = Fast::Renderer3ds::PicaCompareFunction;
  // Values from deko3d.h (DkCompareOp_*): Never=1 ... Always=8.
  Require(DekoCompareOp(C::Never) == 1 && DekoCompareOp(C::Less) == 2 && DekoCompareOp(C::Equal) == 3 &&
              DekoCompareOp(C::LessOrEqual) == 4 && DekoCompareOp(C::Greater) == 5 &&
              DekoCompareOp(C::NotEqual) == 6 && DekoCompareOp(C::GreaterOrEqual) == 7 &&
              DekoCompareOp(C::Always) == 8,
          "comparisons at the DkCompareOp value");
  using L = Fast::Renderer3ds::PicaLogicOperation;
  // The PICA order is not GL/deko3d's: Set (PICA 4) is DkLogicOp_Set 15.
  Require(DekoLogicOp(L::Copy) == kDekoLogicOpCopy && DekoLogicOp(L::Set) == 15 && DekoLogicOp(L::Xor) == 6 &&
              DekoLogicOp(L::Invert) == 10 && DekoLogicOp(L::NoOp) == 5 && DekoLogicOp(L::Nand) == 14 &&
              DekoLogicOp(L::CopyInverted) == 12 && DekoLogicOp(L::AndInverted) == 4,
          "PICA logic op -> DkLogicOp");
  using F = Fast::Renderer3ds::NativeBlendFactor;
  Require(DekoBlendFactor(F::DestColor) == 9 && DekoBlendFactor(F::SourceAlpha) == 5 &&
              DekoBlendFactor(F::OneMinusConstantAlpha) == (4 | 0x20) && DekoBlendFactor(F::SourceAlphaSaturate) == 11,
          "blend factors at the DkBlendFactor value");
  using T = Fast::Renderer3ds::PicaTopology;
  Require(DekoPrimitive(T::TriangleList) == 4 && DekoPrimitive(T::GeometryShader) == 4 &&
              DekoPrimitive(T::TriangleStrip) == 5 && DekoPrimitive(T::TriangleFan) == 6,
          "topologies as in GL");
}

void TestCullPlan() {
  using namespace Fast::Deko3d;
  using M = Fast::Renderer3ds::NativeCullMode;
  Require(!PlanPicaCull(M::KeepAll, false, false).Enabled, "KeepAll without cull");
  // Origin at the top (like Vulkan): the face winding is inverted relative to GL.
  const auto ccw = PlanPicaCull(M::KeepCounterClockwise, false, false);
  Require(ccw.Enabled && !ccw.CullFront && !ccw.FrontCcw, "KeepCCW -> front CW, cull back");
  const auto cw = PlanPicaCull(M::KeepClockwise, true, false);
  Require(cw.Enabled && cw.CullFront && cw.FrontCcw, "flipped framebuffer culls the front");
  Require(PlanPicaCull(M::KeepCounterClockwise, false, true).FrontCcw, "config/deko3d_cull_invert goes back to GL");
}

void TestTargetsAndRects() {
  using namespace Fast::Deko3d;
  Require(kTopScreenPhysicalWidth == Fast::Oot3d::kNativeTopScreenPhysicalWidth &&
              kTopScreenPhysicalHeight == Fast::Oot3d::kNativeTopScreenPhysicalHeight,
          "top screen = GL's");
  const auto top = PlanPicaTargetExtent(480, 400, 2.0F, 1.0666F, 480, 400);
  Require(top.Width == 960 && top.Wide && top.Height == 853 && top.ScaleY > 2.13F, "top target widened");
  const auto bottom = PlanPicaTargetExtent(320, 240, 2.0F, 1.0666F, 480, 400);
  Require(bottom.Width == 640 && bottom.Height == 480 && !bottom.Wide, "other targets don't widen");
  Require(!PlanPicaTargetExtent(480, 400, 3.0F, 1.0F, 480, 400).Wide, "widescreen off");
  Require(ScaledEdge(7, 1.5F) == 11 && ScaledExtent(0, 2.0F) == 0 && ScaledExtent(1, 0.1F) == 1,
          "edges same as PicaGlScale's");
  const PicaRect viewport = PlanPicaViewport(10.7F, 0.0F, 240.9F, 400.0F, 2.0F);
  Require(viewport == PicaRect{20, 0, 480, 800}, "viewport truncates and scales both edges");
  Require(PlanPicaScissor(3, 5, 6, 14, 6, 2.0F, 960, 800) == PicaRect{10, 12, 20, 2}, "inclusive scissor");
  Require(PlanPicaScissor(0, 5, 6, 14, 6, 2.0F, 960, 800) == PicaRect{0, 0, 960, 800}, "no scissor = target");
  Require(PlanPicaScissor(3, 9, 0, 3, 0, 1.0F, 64, 64).Width == 0, "inverted scissor empty");
  Require(ClipPicaRect({-4, 10, 20, 100}, 64, 64) == PicaRect{0, 10, 16, 54}, "clip to the target");
  Require(ClipPicaRect({70, 0, 5, 5}, 64, 64) == PicaRect{}, "outside the target = empty");
  PicaTargetKey a{1, 0x18000000, 0x18100000, 240, 400, 0, 3};
  PicaTargetKey b = a;
  Require(a == b && PicaTargetKeyHash{}(a) == PicaTargetKeyHash{}(b), "equal key");
  b.DepthFormat = 2;
  Require(!(a == b), "depth format distinguishes");
}

void TestFills() {
  using namespace Fast::Deko3d;
  Fast::Renderer3ds::PicaMemoryFillView fill;
  fill.RenderTargetNamespace = 1;
  fill.StartPhysicalAddress = 0x1000;
  fill.EndPhysicalAddress = 0x1000 + 240 * 400 * 4;
  fill.Value = 0x11223344;
  fill.Control = 1U | (1U << 9U); // 32 bits
  const PicaTargetKey key{1, 0x1000, 0, 240, 400, 0, 0};
  const auto coverage = PicaFillCovers(fill, key);
  Require(coverage.Color && !coverage.Depth, "fill covers color; no depth (address 0)");
  const auto color = DecodePicaClearColor(fill, 0);
  Require(color[0] == 0x11 / 255.0F && color[3] == 0x44 / 255.0F, "fill RGBA8 (byte 3 = R)");
  PicaTargetKey shortKey = key;
  shortKey.Height = 401;
  Require(!PicaFillCovers(fill, shortKey).Color, "fill smaller than the target doesn't clear");
  Fast::Renderer3ds::PicaMemoryFillView depth;
  depth.Value = 0x00FFFFFF;
  depth.Control = 1U | (1U << 9U);
  const auto [d, s] = DecodePicaClearDepthStencil(depth, 3);
  Require(d == 1.0F && s == 0, "D24S8 1.0 / stencil 0");
}

void TestTransferAndScanout() {
  using namespace Fast::Deko3d;
  Fast::Renderer3ds::PicaDisplayTransferView transfer;
  transfer.InputPhysicalAddress = 0x1000;
  transfer.OutputPhysicalAddress = 0x2000;
  transfer.InputWidth = 240;
  transfer.InputHeight = 400;
  transfer.OutputWidth = 240;
  transfer.OutputHeight = 400;
  transfer.Flags = 0;
  auto plan = PlanPicaTransfer(transfer, 240, 400, 2.0F, 2.0F, 480, 800);
  Require(plan.Valid && !plan.Linear && plan.SourceWidth == 480 && plan.ContentHeight == 800, "1:1 copy");
  transfer.Flags = 1U << 24U; // 2:1 horizontal
  transfer.OutputWidth = 120;
  plan = PlanPicaTransfer(transfer, 240, 400, 2.0F, 2.0F, 480, 800);
  Require(plan.Valid && plan.Linear && plan.OutputWidth == 120 && plan.SourceWidth == 480 && plan.AllocWidth == 240,
          "2:1 linear reads double");
  transfer.Flags = 8U;
  Require(!PlanPicaTransfer(transfer, 240, 400, 2.0F, 2.0F, 480, 800).Valid, "unsupported mode rejects");
  transfer.Flags = 0;
  transfer.InputWidth = 300;
  Require(!PlanPicaTransfer(transfer, 240, 400, 2.0F, 2.0F, 480, 800).Valid, "larger than the source rejects");
  // Flip: non-linear input without dont-swizzle and without bit 0 = flipped (GL).
  transfer.InputWidth = 240;
  transfer.OutputWidth = 240;
  Require(PlanPicaTransfer(transfer, 240, 400, 1.0F, 1.0F, 240, 400).Flip, "default flip as in GL");
  transfer.Flags = 1U;
  Require(!PlanPicaTransfer(transfer, 240, 400, 1.0F, 1.0F, 240, 400).Flip, "bit 0 undoes the flip");
  // Dynamic resolution (p6m): target allocated at 3x drawn at 2x — the display
  // image stays at the allocation size and the content is the 2x corner.
  transfer.Flags = 0;
  plan = PlanPicaTransfer(transfer, 240, 400, 2.0F, 2.0F, 480, 800, 3.0F, 3.0F);
  Require(plan.Valid && plan.AllocWidth == 720 && plan.AllocHeight == 1200 && plan.ContentWidth == 480 &&
              plan.ContentHeight == 800 && plan.SourceWidth == 480 && plan.SourceHeight == 800,
          "DRS: content smaller than the allocation");
  plan = PlanPicaTransfer(transfer, 240, 400, 3.0F, 3.0F, 720, 1200);
  Require(plan.AllocWidth == plan.ContentWidth && plan.AllocHeight == plan.ContentHeight, "no DRS: content = allocation");
  // 400x240 top screen presented at 1280x720: 5:3 with bars.
  const auto placement = PlanPicaScanout(240, 400, 800, 2.0F, 1280, 720);
  Require(placement.Viewport == PicaRect{40, 0, 1200, 720} && !placement.CoversWindow, "5:3 centered");
  // Widened (16:9): covers the window without a 1-pixel bar.
  const auto wide = PlanPicaScanout(240, 400, 853, 2.0F, 1280, 720);
  Require(wide.CoversWindow && wide.Viewport.Width == 1280, "16:9 covers the window");
}

void TestLogSchedule() {
  // Lines that show whether p5 reached the console (24/10: the log didn't say
  // which NRO it came from nor whether PICA reached the backend before frame 1800).
  // 25/10: the tag changed to p5c without updating this test. Only the shape is
  // checked (pN...), not each build's value.
  const std::string_view tag(Fast::kDeko3dBuildTag);
  Require(tag.size() >= 2U && tag[0] == 'p' && tag[1] >= '0' && tag[1] <= '9', "build tag at the top of the log");
  Require(Fast::Deko3dFrameLogDue(1) && Fast::Deko3dFrameLogDue(60) && Fast::Deko3dFrameLogDue(300) &&
              Fast::Deko3dFrameLogDue(1800) && Fast::Deko3dFrameLogDue(7200) && !Fast::Deko3dFrameLogDue(61) &&
              !Fast::Deko3dFrameLogDue(0),
          "frames with log");
  Require(Fast::Deko3dPicaLogDue(300) && Fast::Deko3dPicaLogDue(1800) && Fast::Deko3dPicaLogDue(3600) &&
              !Fast::Deko3dPicaLogDue(60) && !Fast::Deko3dPicaLogDue(0),
          "PICA counters early (300) and every 1800");
}

void TestUiPlan() {
  using namespace Fast::Deko3d;
  // Package 6: the UI and ImGui in the window (origin at the top, NDC y = -1 at the top).
  // glViewport of the GL window (origin at the bottom) becomes a rectangle with origin at the top.
  Require(GlRectToUpperLeft(160, 0, 960, 720, 1280, 720, false) == UiRect{160, 0, 960, 720}, "full viewport");
  Require(GlRectToUpperLeft(0, 600, 100, 120, 1280, 720, false) == UiRect{0, 0, 100, 120},
          "top rectangle (GL y=600) goes to row 0");
  Require(GlRectToUpperLeft(0, 600, 100, 120, 1280, 720, true) == UiRect{0, 600, 100, 120},
          "flipped framebuffer keeps y");
  Require(GlRectToUpperLeft(-50, -10, 100, 40, 1280, 720, false) == UiRect{0, 690, 50, 30},
          "clips at the edges");
  Require(UiClipYScale(false) == -1.0F && UiClipYScale(true) == 1.0F, "UI y negated in the window");
  // Layouts of the two UI programs (oot3d_legacy_ui_adapters.cpp).
  Require(ClassifyUiVertices(2U * 3U * 8U, 2U) == UiVertexLayout::Solid, "solid: xyzw + rgba");
  Require(ClassifyUiVertices(2U * 3U * 10U, 2U) == UiVertexLayout::Textured, "textured: xyzw + uv + rgba");
  Require(ClassifyUiVertices(2U * 3U * 9U, 2U) == UiVertexLayout::Unsupported, "other layout is rejected");
  Require(ClassifyUiVertices(0U, 0U) == UiVertexLayout::Unsupported, "empty list is rejected");
  Require(ClassifyUiVertices(25U, 1U) == UiVertexLayout::Unsupported, "broken size is rejected");
  // ImGui: pixels (origin at the top) to GL clip; with UiClipYScale(false) the top becomes -1.
  const auto topLeft = ImGuiVertexToUi(0.0F, 0.0F, 0.25F, 0.5F, 0xFF3366FFU, 0.0F, 0.0F, 1280.0F, 720.0F);
  Require(topLeft[0] == -1.0F && topLeft[1] == 1.0F && topLeft[3] == 1.0F, "top-left corner");
  Require(topLeft[1] * UiClipYScale(false) == -1.0F, "top of the screen = NDC -1 on deko3d");
  Require(topLeft[4] == 0.25F && topLeft[5] == 0.5F, "uv");
  Require(topLeft[6] == 1.0F && topLeft[7] == 0x66 / 255.0F && topLeft[8] == 0x33 / 255.0F && topLeft[9] == 1.0F,
          "IM_COL32 is ABGR: red in the low byte");
  const auto bottomRight = ImGuiVertexToUi(1280.0F, 720.0F, 0.0F, 0.0F, 0U, 0.0F, 0.0F, 1280.0F, 720.0F);
  Require(bottomRight[0] == 1.0F && bottomRight[1] == -1.0F, "bottom-right corner");
  Require(ImGuiClipToScissor(10.0F, 20.0F, 110.0F, 70.0F, 0.0F, 0.0F, 1280U, 720U) == UiRect{10, 20, 100, 50},
          "ImGui clip becomes the scissor directly");
  Require(ImGuiClipToScissor(-5.0F, 700.0F, 2000.0F, 900.0F, 0.0F, 0.0F, 1280U, 720U) == UiRect{0, 700, 1280, 20},
          "clip clipped to the window");
  Require(ImGuiClipToScissor(50.0F, 50.0F, 40.0F, 60.0F, 0.0F, 0.0F, 1280U, 720U).Width == 0U, "empty clip");
  // Samplers: the UI contract (linear + clamp) that p5c didn't accept.
  Fast::Renderer3ds::NativeSamplerState ui;
  ui.MinFilter = Fast::Renderer3ds::NativeTextureFilter::Linear;
  ui.MagFilter = Fast::Renderer3ds::NativeTextureFilter::Linear;
  ui.WrapS = Fast::Renderer3ds::NativeTextureWrap::ClampToEdge;
  ui.WrapT = Fast::Renderer3ds::NativeTextureWrap::ClampToEdge;
  const PicaSamplerKey key = UiSamplerKey(ui);
  Require(key.MinLinear && key.MagLinear && key.WrapS == PicaWrap::ClampToEdge &&
              key.WrapT == PicaWrap::ClampToEdge && key.Mip == PicaMipFilter::None,
          "UI sampler: linear, clamp, no mips");
  ui.WrapS = Fast::Renderer3ds::NativeTextureWrap::MirroredRepeat;
  Require(UiSamplerKey(ui).WrapS == PicaWrap::MirroredRepeat, "mirrored");
  const PicaSamplerKey legacy = UiLegacySamplerKey(false, 2U, 1U);
  Require(!legacy.MinLinear && legacy.WrapS == PicaWrap::ClampToEdge && legacy.WrapT == PicaWrap::MirroredRepeat,
          "N64 cms/cmt: 2 clamp, 1 mirror");
  Require(UiLegacySamplerKey(true, 0U, 0U).WrapS == PicaWrap::Repeat, "0 = repeats");
  // p6 on the console: NDC -1 landed on the last row (UI upside down, scene rotated 180°).
  // p6b (negative height) gave a black screen: the flip is the NegativeY swizzle.
  Require(ClipYSwizzleNegates(true) && !ClipYSwizzleNegates(false), "NegativeY swizzle only with deko3d_flip_y");
}

int main() {
  TestRendererConfigSelection();
  TestShaderPack();
  TestDkshScratch();
  TestCommandGuard();
  TestProgramPick();
  TestStageCacheResolvesOnce();
  TestSamplerAndTextureKeys();
  TestMipPlanAndSlots();
  TestVertexPlan();
  TestPipelineEnums();
  TestCullPlan();
  TestTargetsAndRects();
  TestFills();
  TestTransferAndScanout();
  TestLogSchedule();
  TestUiPlan();
  std::puts("oot3d_deko3d_backend_tests: ok");
  return 0;
}
