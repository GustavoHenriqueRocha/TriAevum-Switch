// Pure parts of the deko3d backend (docs/DEKO3D_PROGRESS.md): what runs
// without a GPU is tested here on the host.
#include "fast/backends/gfx_deko3d.h"
#include "fast/backends/gfx_deko3d_pica_pipeline.h"
#include "fast/backends/gfx_deko3d_pica_snapshot.h"
#include "fast/backends/gfx_deko3d_pica_plan.h"
#include "fast/oot3d/scene_presentation_policy.h"
#include "fast/backends/gfx_deko3d_programs.h"
#include "fast/backends/gfx_deko3d_shader_pack.h"
#include "fast/backends/gfx_deko3d_ui_plan.h"
#include "fast/renderer3ds/pica_framebuffer_encode.h"

#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <string>
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
  Require(DkshGprCount(dksh) == 105U, "reads num_gprs");
  Require(kPerWarpScratchBytes >= 4096U && kPerWarpScratchBytes % 0x200U == 0U,
          "the queue covers the pack's shaders (multiple of 0x200)");
  Put32(dksh, 20, 0);
  Require(!DkshPerWarpScratchBytes(dksh).has_value(), "no programs fails");
  Put32(dksh, 20, 1);
  Put32(dksh, 16, 0x7C);
  Require(!DkshPerWarpScratchBytes(dksh).has_value(), "header past the end fails");
  Put32(dksh, 0, 0);
  Require(!DkshPerWarpScratchBytes(dksh).has_value() && !DkshGprCount(dksh).has_value(), "no DKSH fails");
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

// lighting3: the per-frame breakdown of logs/deko3d.txt (programs and
// lighting lines) by base fragment program class.
void TestFrameBreakdown() {
  using namespace Fast::Deko3d;
  Require(ClassifyPicaBase(false, 0U) == PicaBaseClass::Normal, "specialized without scratch: normal");
  Require(ClassifyPicaBase(true, 0U) == PicaBaseClass::Parametric, "uber without scratch: parametric");
  Require(ClassifyPicaBase(true, 4096U) == PicaBaseClass::Spilling &&
              ClassifyPicaBase(false, 512U) == PicaBaseClass::Spilling,
          "any scratch: spilling");
  PicaFrameDrawWindow window;
  window.Current.Count(PicaBaseClass::Parametric, true, false, 1000000U);
  window.Current.Count(PicaBaseClass::Parametric, false, true, 500000U);
  window.Current.Count(PicaBaseClass::Normal, true, true, 250000U);
  window.Current.CountSurfaceLighting(2U, true);
  window.Current.CountSurfaceLighting(9U, false); // above the trim: the last bucket
  window.EndFrame();
  Require(window.Current.Classes[1].Draws == 0U && window.Last.Classes[1].Draws == 2U &&
              window.Last.Classes[1].Lit == 1U && window.Last.Classes[1].Pixels == 1500000U &&
              window.Last.Classes[1].LitPixels == 1000000U,
          "per class: draws, lit draws, pixels");
  Require(window.Last.LitOpaque == 1U && window.Last.LitTranslucent == 1U, "lit opaque vs translucent");
  Require(window.Last.LitPointLights[2] == 1U && window.Last.LitPointLights[4] == 1U && window.Last.LitRim == 1U,
          "points per lit draw and rim");
  window.Current.Count(PicaBaseClass::Spilling, false, false, 100U);
  window.EndFrame();
  Require(window.Frames == 2U && window.Sum.Classes[1].Draws == 2U && window.Sum.Classes[0].Draws == 1U &&
              window.Last.Classes[1].Draws == 0U,
          "window sums frames, last is the last frame");
  Require(FormatPicaClassDraws(window.Sum, 1U) ==
              "spill=1/0 px=0.00/0.00M parametric=2/1 px=1.50/1.00M normal=1/1 px=0.25/0.25M",
          FormatPicaClassDraws(window.Sum, 1U).c_str());
  Require(FormatPicaClassDraws(window.Sum, 2U).find("parametric=1/") != std::string::npos &&
              FormatPicaClassDraws(window.Sum, 2U).find("px=0.75/0.50M") != std::string::npos,
          "window average per frame");
  Require(FormatPicaLitDraws(window.Sum, 1U) == "opaque=1 translucent=1 points 0/1/2/3/4=0/0/1/0/1 rim=1",
          FormatPicaLitDraws(window.Sum, 1U).c_str());
  window.StartWindow();
  Require(window.Frames == 0U && window.Sum.Classes[1].Draws == 0U && window.Last.Classes[0].Draws == 1U,
          "a log line starts a new window");
}

// ZCULL study: the depth line of logs/deko3d.txt (how the game resets depth,
// draws whose program writes gl_FragDepth).
void TestDepthBreakdown() {
  using namespace Fast::Deko3d;
  // The generator's raster-depth case: z-buffering, scale -1, offset 0.
  Require(!PicaDrawWritesFragDepth(false, false, -1.0F, 0.0F), "raster depth: fixed-function depth");
  Require(PicaDrawWritesFragDepth(false, false, -1.0F, 0.001F) && PicaDrawWritesFragDepth(false, false, -0.5F, 0.0F) &&
              PicaDrawWritesFragDepth(false, true, -1.0F, 0.0F),
          "depth offset, scale or w-buffering: gl_FragDepth");
  Require(PicaDrawWritesFragDepth(true, false, -1.0F, 0.0F), "parametric programs always write it");
  PicaFrameDrawWindow window;
  // Frame 1: a full-target ALWAYS write first, then a partial one, a tested
  // gl_FragDepth draw and a tested ALWAYS draw that writes nothing.
  window.Current.CountDepth(true, true, true, true, false);
  window.Current.CountDepth(true, true, true, false, false);
  window.Current.CountDepth(true, true, false, false, true);
  window.Current.CountDepth(true, false, true, true, true);
  window.EndFrame();
  Require(window.Last.DepthAlwaysWrites == 2U && window.Last.DepthAlwaysFull == 1U && window.Last.FragDepthTested == 1U,
          "always writes, full ones, tested gl_FragDepth draws");
  Require(window.Last.FirstDepthWrite[static_cast<size_t>(PicaFirstDepthWrite::AlwaysFull)] == 1U,
          "first depth write: ALWAYS over the whole target");
  // Frame 2: nothing writes depth. Frame 3: a tested draw writes first.
  window.Current.CountDepth(true, false, false, true, false);
  window.EndFrame();
  window.Current.CountDepth(true, true, false, true, false);
  window.Current.CountDepth(true, true, true, true, false);
  window.EndFrame();
  Require(FormatPicaDepthDraws(window.Sum, 1U) == "always=3 full=2 fragdepth=1 first none/full/partial/tested=1.00/1.00/0.00/1.00",
          FormatPicaDepthDraws(window.Sum, 1U).c_str());
  Require(FormatPicaDepthDraws(window.Last, 1U) == "always=1 full=1 fragdepth=0 first none/full/partial/tested=0.00/0.00/0.00/1.00",
          FormatPicaDepthDraws(window.Last, 1U).c_str());
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
  Require(key.WrapS == PicaWrap::Repeat && key.WrapT == PicaWrap::MirroredRepeat, "wrap");
  Require(MakePicaSamplerKey(view, 1).Mip == PicaMipFilter::None, "no mip: mip filter off");
  PicaSamplerKeyHash samplerHash;
  Require(samplerHash(key) == samplerHash(MakePicaSamplerKey(view, 4)), "stable hash");
  const auto textureKey = MakePicaTextureKey(view, 0x1234);
  auto other = view;
  other.NativeWrapS = 0;
  Require(!(textureKey == MakePicaTextureKey(other, 0x1234)), "sampler state is part of the key (as in GL)");
  PicaTextureKeyHash textureHash;
  Require(textureHash(textureKey) == textureHash(MakePicaTextureKey(view, 0x1234)), "stable texture hash");
  // The PICA LOD bias is applied once, by the fragment shader (textureLod(...,
  // lod + texture_lod_bias)): the sampler carries none, or a -1.7 bias (the
  // Forest Temple's floors) sampled at -3.4 and shimmered. So the bias is in
  // neither key: one sampler, one texture and one pack upload for any bias.
  auto biased = view;
  biased.LodBiasRaw = -435; // -1.7
  Require(MakePicaSamplerKey(biased, 4) == key && samplerHash(MakePicaSamplerKey(biased, 4)) == samplerHash(key),
          "LOD bias is not sampler state (the shader applies it)");
  Require(MakePicaTextureKey(biased, 0x1234) == textureKey &&
              textureHash(MakePicaTextureKey(biased, 0x1234)) == textureHash(textureKey),
          "one texture for every LOD bias of the same image");
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
  const PicaRect viewport = PlanPicaViewport(10.7F, 0.0F, 240.9F, 400.0F, 2.0F, 2.0F);
  Require(viewport == PicaRect{20, 0, 480, 800}, "viewport truncates and scales both edges");
  Require(PlanPicaScissor(3, 5, 6, 14, 6, 2.0F, 2.0F, 960, 800) == PicaRect{10, 12, 20, 2}, "inclusive scissor");
  Require(PlanPicaScissor(0, 5, 6, 14, 6, 2.0F, 2.0F, 960, 800) == PicaRect{0, 0, 960, 800}, "no scissor = target");
  Require(PlanPicaScissor(3, 9, 0, 3, 0, 1.0F, 1.0F, 64, 64).Width == 0, "inverted scissor empty");
  Require(PlanPicaViewport(10.7F, 0.0F, 240.9F, 400.0F, 1.5F, 3.0F) == PicaRect{15, 0, 360, 1200},
          "viewport: X and Y scaled apart");
  Require(PlanPicaScissor(3, 5, 6, 14, 6, 1.5F, 3.0F, 720, 1200) == PicaRect{8, 18, 15, 3},
          "scissor: X and Y scaled apart, edges rounded");
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
  const auto at1 = PlanPicaTargetExtent(240, 400, 1.0F, 1.0F, 480, 400);
  const auto at2 = PlanPicaTargetExtent(240, 400, 2.0F, 1.0F, 480, 400);
  const auto at3 = PlanPicaTargetExtent(240, 400, 3.0F, 1.0F, 480, 400);
  auto plan = PlanPicaTransfer(transfer, 240, 400, at2, at2);
  Require(plan.Valid && !plan.Linear && plan.SourceWidth == 480 && plan.ContentHeight == 800, "1:1 copy");
  transfer.Flags = 1U << 24U; // 2:1 horizontal
  transfer.OutputWidth = 120;
  plan = PlanPicaTransfer(transfer, 240, 400, at2, at2);
  Require(plan.Valid && plan.Linear && plan.OutputWidth == 120 && plan.SourceWidth == 480 && plan.AllocWidth == 240,
          "2:1 linear reads double");
  transfer.Flags = 8U;
  Require(!PlanPicaTransfer(transfer, 240, 400, at2, at2).Valid, "unsupported mode rejects");
  transfer.Flags = 0;
  transfer.InputWidth = 300;
  Require(!PlanPicaTransfer(transfer, 240, 400, at2, at2).Valid, "larger than the source rejects");
  // Flip: non-linear input without dont-swizzle and without bit 0 = flipped (GL).
  transfer.InputWidth = 240;
  transfer.OutputWidth = 240;
  Require(PlanPicaTransfer(transfer, 240, 400, at1, at1).Flip, "default flip as in GL");
  transfer.Flags = 1U;
  Require(!PlanPicaTransfer(transfer, 240, 400, at1, at1).Flip, "bit 0 undoes the flip");
  // Dynamic resolution (p6m): target allocated at 3x drawn at 2x — the display
  // image stays at the allocation size and the content is the 2x corner.
  transfer.Flags = 0;
  const auto at3drawn2 = PlanPicaDynamicExtent(240, 400, 2.0F, at3);
  Require(at3drawn2.Width == 480 && at3drawn2.Height == 800 && at3drawn2.Scale == 2.0F &&
              PlanPicaDynamicExtent(240, 400, 3.0F, at3).Width == 720,
          "DRS: the drawn corner at 2x, the allocation at its own scale");
  plan = PlanPicaTransfer(transfer, 240, 400, at3drawn2, at3);
  Require(plan.Valid && plan.AllocWidth == 720 && plan.AllocHeight == 1200 && plan.ContentWidth == 480 &&
              plan.ContentHeight == 800 && plan.SourceWidth == 480 && plan.SourceHeight == 800,
          "DRS: content smaller than the allocation");
  plan = PlanPicaTransfer(transfer, 240, 400, at3, at3);
  Require(plan.AllocWidth == plan.ContentWidth && plan.AllocHeight == plan.ContentHeight, "no DRS: content = allocation");
  // 400x240 top screen presented at 1280x720: 5:3 with bars.
  const auto placement = PlanPicaScanout(240, 400, 800, 2.0F, 1280, 720);
  Require(placement.Viewport == PicaRect{40, 0, 1200, 720} && !placement.CoversWindow, "5:3 centered");
  // Widened (16:9): covers the window without a 1-pixel bar.
  const auto wide = PlanPicaScanout(240, 400, 853, 2.0F, 1280, 720);
  Require(wide.CoversWindow && wide.Viewport.Width == 1280, "16:9 covers the window");
}

// config/deko3d_line_samples: the top screen at one sample per presented
// line (default) or the game's two (test.7), at every dynamic-resolution
// level of the Switch (3 down to the 2.25 floor and below), widened or not.
void TestTopScreenLineSamples() {
  using namespace Fast::Deko3d;
  using Lines = TopScreenLineSamples;
  Require(ParseTopScreenLineSamples("2") == Lines::Two && ParseTopScreenLineSamples(" 2\r\n") == Lines::Two,
          "\"2\" keeps the game's two samples");
  Require(ParseTopScreenLineSamples("") == Lines::One && ParseTopScreenLineSamples("1") == Lines::One &&
              ParseTopScreenLineSamples("x2") == Lines::One,
          "absent, 1 or anything else: one sample");
  const auto presented = PresentedTopScreenExtent(3.0F);
  Require(presented.Width == 1200 && presented.Height == 720, "3x presents 1200x720 before widescreen");
  // The Vulkan policy (render_resolution_policy.h) decides: one sample once
  // the allocation presents the game's 480 samples (scale >= 2).
  Require(PicaTargetSamplingX(480, 400, 3.0F, Lines::One) == 0.5F &&
              PicaTargetSamplingX(480, 400, 2.0F, Lines::One) == 0.5F &&
              PicaTargetSamplingX(480, 400, 3.0F, Lines::One) ==
                  Fast::Oot3d::NativeTopScreenStorageSamplingX({480, 400}, {1280, 720}),
          "top screen from 2x: one sample per line, as Vulkan in handheld");
  Require(PicaTargetSamplingX(480, 400, 1.75F, Lines::One) == 1.0F, "below 480 presented lines: the game's sampling");
  Require(PicaTargetSamplingX(480, 400, 3.0F, Lines::Two) == 1.0F, "config 2 keeps the game's sampling");
  Require(PicaTargetSamplingX(320, 240, 3.0F, Lines::One) == 1.0F && PicaTargetSamplingX(240, 400, 3.0F, Lines::One) == 1.0F,
          "the bottom screen and other targets keep their sampling");

  // The game's top-screen transfer: 480x400 -> 240x400, 2:1 along X.
  Fast::Renderer3ds::PicaDisplayTransferView transfer;
  transfer.InputPhysicalAddress = 0x1000;
  transfer.OutputPhysicalAddress = 0x2000;
  transfer.InputWidth = 480;
  transfer.InputHeight = 400;
  transfer.OutputWidth = 240;
  transfer.OutputHeight = 400;
  transfer.Flags = 0x01001000U;
  for (const float factor : {1.0F, 1280.0F / 1200.0F}) {
    const auto alloc = [&](Lines lines) {
      return PlanPicaTargetExtent(480, 400, 3.0F, factor, 480, 400, PicaTargetSamplingX(480, 400, 3.0F, lines));
    };
    const auto oneAlloc = alloc(Lines::One);
    const auto twoAlloc = alloc(Lines::Two);
    Require(twoAlloc.Width == 1440 && oneAlloc.Width == 720 && oneAlloc.Height == twoAlloc.Height &&
                oneAlloc.Height == (factor > 1.0F ? 1280U : 1200U),
            "3x: 720 lines, not 1440 samples; the presented width unchanged");
    const auto onePlan = PlanPicaTransfer(transfer, 480, 400, oneAlloc, oneAlloc);
    Require(onePlan.Valid && !onePlan.Linear && onePlan.SourceWidth == 720 && onePlan.ContentWidth == 720 &&
                onePlan.AllocWidth == 720 && onePlan.SourceHeight == onePlan.ContentHeight,
            "one sample: the 2:1 filter becomes a 1:1 copy of the whole target");
    const uint64_t onePixelsAt3 = uint64_t{oneAlloc.Width} * oneAlloc.Height;
    for (const float scale : {3.0F, 2.75F, 2.5F, 2.25F, 2.0F, 1.5F}) {
      const auto one = PlanPicaDynamicExtent(480, 400, scale, oneAlloc);
      const auto two = PlanPicaDynamicExtent(480, 400, scale, twoAlloc);
      Require(one.Width * 2U == two.Width && one.Height == two.Height && one.SamplingX == 0.5F &&
                  one.ScaleX == scale * 0.5F && one.Scale == scale,
              "every DRS level: the same presented lines from half the samples");
      Require(one.Width == ScaledExtent(240, scale) && one.Height <= oneAlloc.Height && one.Width <= oneAlloc.Width,
              "one sample per presented line, inside the allocation");
      // The controller's pixel model: the area follows (s / max)^2.
      const double area = static_cast<double>(uint64_t{one.Width} * one.Height) / static_cast<double>(onePixelsAt3);
      Require(std::abs(area - (scale / 3.0) * (scale / 3.0)) < 0.005, "DRS pixel area ~ scale squared");
      const auto onePlanAt = PlanPicaTransfer(transfer, 480, 400, one, oneAlloc);
      const auto twoPlanAt = PlanPicaTransfer(transfer, 480, 400, two, twoAlloc);
      Require(onePlanAt.Valid && twoPlanAt.Valid && !onePlanAt.Linear && twoPlanAt.Linear,
              "copy (one) against the averaging blit (two)");
      Require(onePlanAt.ContentWidth == twoPlanAt.ContentWidth && onePlanAt.ContentHeight == twoPlanAt.ContentHeight &&
                  onePlanAt.AllocWidth == twoPlanAt.AllocWidth && onePlanAt.AllocHeight == twoPlanAt.AllocHeight,
              "the display image and its content are the same with both policies");
      Require(onePlanAt.SourceWidth == onePlanAt.ContentWidth && onePlanAt.SourceHeight == onePlanAt.ContentHeight &&
                  onePlanAt.SourceWidth == one.Width && onePlanAt.SourceHeight <= one.Height &&
                  twoPlanAt.SourceWidth == two.Width,
              "1:1 copy of the drawn corner, never past it");
      Require((onePlanAt.ContentWidth < onePlanAt.AllocWidth) == (scale < 3.0F),
              "below the top scale the edge bands still apply");
      const auto scanout = PlanPicaScanout(onePlanAt.OutputWidth, onePlanAt.OutputHeight, onePlanAt.AllocHeight,
                                           oneAlloc.Scale, 1280, 720);
      Require(scanout.CoversWindow == (factor > 1.0F), "the scanout is unchanged (16:9 covers the window)");
      // A full-target draw lands on the drawn corner (non-widened path; the
      // widened one shares its X edges).
      if (factor == 1.0F) {
        Require(PlanPicaViewport(0.0F, 0.0F, 480.0F, 400.0F, one.ScaleX, one.Scale) ==
                    PicaRect{0, 0, static_cast<int32_t>(one.Width), static_cast<int32_t>(one.Height)},
                "a full-screen viewport covers the drawn extent");
        const PicaRect left = PlanPicaViewport(0.0F, 0.0F, 241.0F, 400.0F, one.ScaleX, one.Scale);
        const PicaRect right = PlanPicaViewport(241.0F, 0.0F, 239.0F, 400.0F, one.ScaleX, one.Scale);
        Require(left.X + left.Width == right.X && right.X + right.Width == static_cast<int32_t>(one.Width),
                "neighbouring viewports tile at half the scale");
      }
    }
  }
  // A top-screen transfer without the filter (a 1:1 crop) keeps the source's
  // density; both axes filtered still averages Y.
  const auto oneAlloc = PlanPicaTargetExtent(480, 400, 3.0F, 1.0F, 480, 400, 0.5F);
  transfer.OutputWidth = 480;
  transfer.Flags = 0x00001000U;
  auto plan = PlanPicaTransfer(transfer, 480, 400, oneAlloc, oneAlloc);
  Require(plan.Valid && !plan.Linear && plan.SourceWidth == 720 && plan.ContentWidth == 720 && plan.AllocWidth == 720,
          "unfiltered transfer: content at the source's density");
  transfer.OutputWidth = 240;
  transfer.OutputHeight = 200;
  transfer.Flags = 0x02001000U;
  plan = PlanPicaTransfer(transfer, 480, 400, oneAlloc, oneAlloc);
  Require(plan.Valid && plan.Linear && plan.SourceWidth == 720 && plan.ContentWidth == 720 &&
              plan.SourceHeight == 1200 && plan.ContentHeight == 600,
          "2:1 on both axes: X copied, Y still averaged");
}

// ---- savestates: CapturePicaColorTargets / RestorePicaColorTargets ----

void TestSavestateDepthWords() {
  using namespace Fast::Deko3d;
  using L = DepthStencilWordLayout;
  Require(PackDepthStencilWord(kProbeDepth, kProbeStencil, L::DepthHigh) == 0x4000005AU &&
              PackDepthStencilWord(kProbeDepth, kProbeStencil, L::DepthLow) == 0x5A400000U,
          "Z24S8 words in both layouts");
  Require(PackDepthStencilWord(1.0F, 0xFF, L::DepthHigh) == 0xFFFFFFFFU &&
              PackDepthStencilWord(2.0F, 0, L::DepthLow) == 0x00FFFFFFU &&
              PackDepthStencilWord(-1.0F, 7, L::DepthHigh) == 7U,
          "depth clamped to [0, 1]");
  for (const auto layout : {L::DepthHigh, L::DepthLow}) {
    const uint32_t word = PackDepthStencilWord(0.75F, 0x81, layout);
    Require(WordDepth24(word, layout) == Depth24(0.75F) && WordStencil(word, layout) == 0x81,
            "unpack inverts pack");
  }
  // The probe clear, rounded either way by the hardware; garbage is refused
  // (the save fails instead of storing swapped depth and stencil).
  Require(IdentifyDepthStencilWordLayout(0x4000005AU) == L::DepthHigh &&
              IdentifyDepthStencilWordLayout(0x3FFFFF5AU) == L::DepthHigh &&
              IdentifyDepthStencilWordLayout(0x5A400000U) == L::DepthLow &&
              IdentifyDepthStencilWordLayout(0x5A3FFFFFU) == L::DepthLow,
          "probe clear identifies the layout");
  Require(!IdentifyDepthStencilWordLayout(0x00000000U) && !IdentifyDepthStencilWordLayout(0x4000005BU) &&
              !IdentifyDepthStencilWordLayout(0x5A400002U) && !IdentifyDepthStencilWordLayout(0xFFFFFF00U),
          "unknown words refused");
}

// A 2x target (16x16) of an 8x8 guest framebuffer through the whole path:
// read-back -> guest-extent snapshot -> guest memory (SaveState's encode) ->
// decode (LoadState) -> validation -> the pixels a restored 2x target gets.
void TestSavestateRoundTrip() {
  using namespace Fast::Deko3d;
  using Fast::Renderer3ds::PicaRenderTargetColorSnapshot;
  const PicaTargetKey key{1, 0x18000000, 0x18100000, 8, 8, 0, 3};
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 16;
  for (const auto layout : {DepthStencilWordLayout::DepthHigh, DepthStencilWordLayout::DepthLow}) {
    std::vector<uint8_t> rgba(kWidth * kHeight * 4U);
    std::vector<uint32_t> words(kWidth * kHeight);
    for (uint32_t y = 0; y < kHeight; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
        // Colour constant over each guest pixel's 2x2 block, so its average is exact.
        uint8_t* texel = rgba.data() + (y * kWidth + x) * 4U;
        texel[0] = static_cast<uint8_t>((x / 2U) * 30U);
        texel[1] = static_cast<uint8_t>((y / 2U) * 30U);
        texel[2] = static_cast<uint8_t>(((x / 2U) ^ (y / 2U)) * 16U);
        texel[3] = 255U;
        words[y * kWidth + x] =
            PackDepthStencilWord(static_cast<float>(y * kWidth + x + 1U) / 300.0F, static_cast<uint8_t>(x ^ y), layout);
      }
    }
    const auto snapshot = MakeGuestExtentSnapshot(key, kWidth, kHeight, rgba, words, layout);
    Require(snapshot.RenderTargetNamespace == 1 && snapshot.ColorPhysicalAddress == 0x18000000 &&
                snapshot.DepthPhysicalAddress == 0x18100000 && snapshot.FramebufferWidth == 8 &&
                snapshot.FramebufferHeight == 8 && snapshot.FramebufferColorFormat == 0 &&
                snapshot.FramebufferDepthFormat == 3,
            "snapshot identity is the target key");
    Require(snapshot.RenderScalePermille == Fast::Renderer3ds::kPicaGuestExtentRenderScalePermille &&
                snapshot.ImageWidth == 8 && snapshot.ImageHeight == 8 && snapshot.SampleCount == 1 &&
                !snapshot.WBuffering && !snapshot.SceneEffectsResolved,
            "snapshot at the guest extent, as SaveState requires");
    Require(snapshot.NormalGuideRgba8.empty() && snapshot.ShadowR32UintLe.empty() &&
                snapshot.ColorRgba8.size() == 8U * 8U * 4U && snapshot.DepthValues.size() == 64U &&
                snapshot.StencilValues.size() == 64U,
            "colour, depth and stencil only");
    for (uint32_t y = 0; y < 8U; ++y) {
      for (uint32_t x = 0; x < 8U; ++x) {
        const uint8_t* texel = snapshot.ColorRgba8.data() + (y * 8U + x) * 4U;
        Require(texel[0] == x * 30U && texel[1] == y * 30U && texel[2] == (x ^ y) * 16U && texel[3] == 255U,
                "colour area-averaged to the guest pixel");
        // Nearest: the source pixel at the destination centre (2x + 1, 2y + 1).
        const uint32_t source = words[(2U * y + 1U) * kWidth + 2U * x + 1U];
        Require(Depth24(snapshot.DepthValues[y * 8U + x]) == WordDepth24(source, layout) &&
                    snapshot.StencilValues[y * 8U + x] == WordStencil(source, layout),
                "depth and stencil from the same nearest pixel");
      }
    }
    // SaveState writes it into guest memory, LoadState reads it back.
    const auto colorBytes = Fast::Renderer3ds::EncodePicaFramebuffer(snapshot.ColorRgba8, 8, 8, 8, 8, 0);
    const auto depthBytes =
        Fast::Renderer3ds::EncodePicaDepthStencil(snapshot.DepthValues, snapshot.StencilValues, 8, 8, 3);
    PicaRenderTargetColorSnapshot loaded = snapshot;
    loaded.ColorRgba8 = Fast::Renderer3ds::DecodePicaFramebuffer(colorBytes, 8, 8, 0);
    Fast::Renderer3ds::DecodePicaDepthStencil(depthBytes, 8, 8, 3, loaded.DepthValues, loaded.StencilValues);
    Require(loaded.ColorRgba8 == snapshot.ColorRgba8 && loaded.StencilValues == snapshot.StencilValues,
            "guest memory keeps colour and stencil");
    const std::array<PicaRenderTargetColorSnapshot, 1> restored{loaded};
    Require(ValidateRestoreSnapshots(restored).empty(), "a loaded snapshot restores");
    const auto pixels = PlanRestorePixels(loaded, kWidth, kHeight, layout);
    Require(pixels.Rgba8.size() == rgba.size() && pixels.DepthStencil.size() == words.size(), "pixels at the target extent");
    for (uint32_t y = 0; y < kHeight; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
        const uint32_t word = pixels.DepthStencil[y * kWidth + x];
        const uint32_t saved = words[(2U * (y / 2U) + 1U) * kWidth + 2U * (x / 2U) + 1U];
        const uint32_t depth = WordDepth24(word, layout);
        const uint32_t savedDepth = WordDepth24(saved, layout);
        Require((depth > savedDepth ? depth - savedDepth : savedDepth - depth) <= 1U &&
                    WordStencil(word, layout) == WordStencil(saved, layout),
                "restored depth/stencil: the saved guest pixel, nearest");
      }
    }
  }
  // Uniform colour survives the area average and the bilinear enlargement unchanged.
  std::vector<uint8_t> flat(kWidth * kHeight * 4U);
  for (size_t index = 0; index < flat.size(); index += 4U) {
    flat[index] = 12;
    flat[index + 1] = 200;
    flat[index + 2] = 77;
    flat[index + 3] = 255;
  }
  const std::vector<uint32_t> cleared(kWidth * kHeight, PackDepthStencilWord(1.0F, 0, DepthStencilWordLayout::DepthHigh));
  const auto snapshot = MakeGuestExtentSnapshot(key, kWidth, kHeight, flat, cleared, DepthStencilWordLayout::DepthHigh);
  const auto pixels = PlanRestorePixels(snapshot, kWidth, kHeight, DepthStencilWordLayout::DepthHigh);
  Require(pixels.Rgba8 == flat && pixels.DepthStencil == cleared, "uniform target round-trips exactly");
  // A widescreen top target (taller than its scale) still saves at the guest extent.
  const PicaTargetKey top{1, 0x18000000, 0x18100000, 16, 8, 0, 3};
  const std::vector<uint8_t> wideColor(32U * 21U * 4U, 9);
  const std::vector<uint32_t> wideWords(32U * 21U, 0x12345678U);
  const auto wide = MakeGuestExtentSnapshot(top, 32, 21, wideColor, wideWords, DepthStencilWordLayout::DepthLow);
  Require(wide.ImageWidth == 16 && wide.ImageHeight == 8 && wide.ColorRgba8.size() == 16U * 8U * 4U &&
              wide.DepthValues.size() == 128U,
          "widened target resampled to the guest extent");
  // A top target at one sample per presented line (config/deko3d_line_samples)
  // under a low dynamic-resolution level: fewer columns than the guest's
  // (enlarged on capture), more rows; saved and restored per axis.
  const std::vector<uint8_t> lineColor(12U * 24U * 4U, 9);
  const std::vector<uint32_t> lineWords(12U * 24U, 0x12345678U);
  const auto lines = MakeGuestExtentSnapshot(top, 12, 24, lineColor, lineWords, DepthStencilWordLayout::DepthLow);
  Require(lines.ImageWidth == 16 && lines.ImageHeight == 8 && lines.ColorRgba8 == std::vector<uint8_t>(16U * 8U * 4U, 9),
          "one-sample target saved at the guest extent");
  const auto linePixels = PlanRestorePixels(lines, 12, 24, DepthStencilWordLayout::DepthLow);
  Require(linePixels.Rgba8 == lineColor && linePixels.DepthStencil == lineWords, "and restored at its own extent");
}

void TestSavestateRestoreValidation() {
  using namespace Fast::Deko3d;
  using Fast::Renderer3ds::PicaRenderTargetColorSnapshot;
  PicaRenderTargetColorSnapshot good;
  good.RenderTargetNamespace = 1;
  good.ColorPhysicalAddress = 0x18000000;
  good.DepthPhysicalAddress = 0x18100000;
  good.FramebufferWidth = 8;
  good.FramebufferHeight = 8;
  good.FramebufferDepthFormat = 3;
  good.RenderScalePermille = Fast::Renderer3ds::kPicaGuestExtentRenderScalePermille;
  good.ImageWidth = 8;
  good.ImageHeight = 8;
  good.ColorRgba8.assign(256, 0);
  good.DepthValues.assign(64, 1.0F);
  Require(ValidateRestoreSnapshots(std::span<const PicaRenderTargetColorSnapshot>{}).empty(), "nothing to restore");
  Require(ValidateRestoreSnapshots(std::span(&good, 1)).empty(), "guest extent without stencil (D24)");
  const auto refused = [&](auto change, const char* message) {
    PicaRenderTargetColorSnapshot bad = good;
    change(bad);
    Require(!ValidateRestoreSnapshots(std::span(&bad, 1)).empty(), message);
  };
  refused([](auto& s) { s.ImageWidth = 16; s.ColorRgba8.assign(512, 0); s.DepthValues.assign(128, 1.0F); },
          "guest extent other than the framebuffer's");
  refused([](auto& s) { s.ColorRgba8.pop_back(); }, "short colour");
  refused([](auto& s) { s.DepthValues.pop_back(); }, "short depth");
  refused([](auto& s) { s.StencilValues.assign(63, 0); }, "short stencil");
  refused([](auto& s) { s.DepthValues[5] = std::numeric_limits<float>::quiet_NaN(); }, "depth NaN");
  refused([](auto& s) { s.DepthValues[5] = 1.5F; }, "depth above 1");
  refused([](auto& s) { s.ColorPhysicalAddress = 0; }, "no colour address");
  refused([](auto& s) { s.SampleCount = 4; }, "multisampled");
  const std::array<PicaRenderTargetColorSnapshot, 2> twice{good, good};
  Require(!ValidateRestoreSnapshots(twice).empty(), "same identity twice");
  // Legacy (saved at a renderer's 2x scale, with guides deko3d does not have):
  // its colour, depth and stencil restore; mixed scales are refused.
  PicaRenderTargetColorSnapshot legacy = good;
  legacy.RenderScalePermille = 2000;
  legacy.ImageWidth = 16;
  legacy.ImageHeight = 16;
  legacy.ColorRgba8.assign(1024, 0);
  legacy.DepthValues.assign(256, 0.5F);
  legacy.NormalGuideRgba8.assign(1024, 128);
  Require(ValidateRestoreSnapshots(std::span(&legacy, 1)).empty(), "legacy snapshot restores");
  const auto pixels = PlanRestorePixels(legacy, 24, 24, DepthStencilWordLayout::DepthHigh);
  Require(pixels.Rgba8.size() == 24U * 24U * 4U && pixels.DepthStencil.size() == 576U &&
              pixels.DepthStencil.front() == PackDepthStencilWord(0.5F, 0, DepthStencilWordLayout::DepthHigh),
          "legacy resampled from its own extent");
  PicaRenderTargetColorSnapshot other = good;
  other.ColorPhysicalAddress = 0x18200000;
  const std::array<PicaRenderTargetColorSnapshot, 2> mixed{legacy, other};
  Require(ValidateRestoreSnapshots(mixed).find("different render scales") != std::string::npos,
          "mixed scales refused");
  // Snapshot order and the key a restore creates.
  Require(PicaTargetKeyOf(good) == PicaTargetKey{1, 0x18000000, 0x18100000, 8, 8, 0, 3}, "key of a snapshot");
  Require(PicaTargetKeyLess({1, 0x10, 0, 8, 8, 0, 0}, {1, 0x20, 0, 8, 8, 0, 0}) &&
              PicaTargetKeyLess({1, 0x20, 0, 8, 8, 0, 0}, {2, 0x10, 0, 8, 8, 0, 0}) &&
              !PicaTargetKeyLess({1, 0x10, 0, 8, 8, 0, 0}, {1, 0x10, 0, 8, 8, 0, 0}),
          "namespace, then colour address (FindColor's order)");
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
  TestTopScreenLineSamples();
  TestLogSchedule();
  TestUiPlan();
  TestSavestateDepthWords();
  TestSavestateRoundTrip();
  TestSavestateRestoreValidation();
  TestFrameBreakdown();
  TestDepthBreakdown();
  std::puts("oot3d_deko3d_backend_tests: ok");
  return 0;
}
