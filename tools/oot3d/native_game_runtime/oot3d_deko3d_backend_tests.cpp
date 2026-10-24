// Pure parts of the deko3d backend (docs/DEKO3D_PROGRESSO.md): what runs
// without a GPU is tested here on the host.
#include "fast/backends/gfx_deko3d.h"
#include "fast/backends/gfx_deko3d_pica_pipeline.h"
#include "fast/backends/gfx_deko3d_pica_plan.h"
#include "fast/oot3d/scene_presentation_policy.h"
#include "fast/backends/gfx_deko3d_programs.h"
#include "fast/backends/gfx_deko3d_shader_pack.h"

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
  Require(Deko3dRendererConfigSelects("deko3d"), "deko3d seleciona");
  Require(Deko3dRendererConfigSelects("deko3d\n"), "com quebra de linha");
  Require(Deko3dRendererConfigSelects("  DEKO3D \r\n"), "maiúsculas e espaços");
  Require(Deko3dRendererConfigSelects("deko3d\ngl"), "só a primeira linha conta");
  Require(!Deko3dRendererConfigSelects(""), "arquivo vazio = GL");
  Require(!Deko3dRendererConfigSelects("gl"), "gl = GL");
  Require(!Deko3dRendererConfigSelects("opengl"), "opengl = GL");
  Require(!Deko3dRendererConfigSelects("deko3d2"), "nome com sufixo = GL");
  Require(!Deko3dRendererConfigSelects("\ndeko3d"), "primeira linha vazia = GL");
  // Fora do Switch nunca seleciona (o arquivo nem é lido).
  Require(!Fast::Deko3dRendererSelected(), "host: GL sempre");
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
  // Mesmo vetor de tools/oot3d/deko3d_shader_pack.py --selftest.
  Require(ShaderTextKey(ShaderStage::Fragment, "abc") == 0x593522965D44E8B2ULL,
          "chave FNV igual à da ferramenta de pacote");
  Require(ShaderTextKey(ShaderStage::Vertex, "abc") != ShaderTextKey(ShaderStage::Fragment, "abc"),
          "o estágio entra na chave");
  ShaderPackIndex index;
  std::string error;
  Require(index.Parse(MakePack(10, 20), &error), "pacote válido");
  Require(index.Count() == 2, "duas entradas");
  ShaderStage stage = ShaderStage::Vertex;
  const auto blob = index.Find(20, &stage);
  Require(blob.size() == 8 && blob[4] == 0x42 && stage == ShaderStage::Fragment, "acha a entrada 20");
  Require(index.Find(15).empty(), "chave ausente");
  Require(!index.Parse(MakePack(20, 10), &error) && index.Count() == 0, "fora de ordem reprova");
  auto bad = MakePack(10, 20);
  Put32(bad, 72, 0);
  Require(!index.Parse(bad, &error), "blob sem DKSH reprova");
  auto shortPack = MakePack(10, 20);
  shortPack.resize(70);
  Require(!index.Parse(shortPack, &error), "blob além do fim reprova");
  Require(!index.Parse({}, &error), "vazio reprova");
}

void TestProgramPick() {
  using namespace Fast::Deko3d;
  Require(PickPicaProgram(true, true, true) == PicaProgramPick::Specialized, "especializado vence o uber");
  Require(PickPicaProgram(true, true, false) == PicaProgramPick::Specialized, "especializado sem uber");
  Require(PickPicaProgram(true, false, true) == PicaProgramPick::Uber, "falta o especializado: uber");
  Require(PickPicaProgram(true, false, false) == PicaProgramPick::NoFragment, "sem nenhum fragment: pula");
  Require(PickPicaProgram(false, true, true) == PicaProgramPick::NoVertex, "sem vertex: pula mesmo com fragment");
  PicaProgramPickCounters counters;
  counters.Count(PicaProgramPick::Uber);
  counters.Count(PicaProgramPick::Uber);
  counters.Count(PicaProgramPick::NoVertex);
  Require(counters.Uber == 2 && counters.NoVertex == 1 && counters.Specialized == 0, "contadores por escolha");
}

// Regressão de desenho: cada texto é traduzido/procurado no pacote uma vez só
// por identidade, inclusive quando falta (senão cada draw sem DKSH refaria a
// tradução de ~31 KB na thread de render).
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
    Require(cache.Get(present, [&] { ++calls; return &shader; }) == &shader, "acha o shader");
    Require(cache.Get(absent, [&]() -> const FakeShader* { ++calls; return nullptr; }) == nullptr,
            "ausente continua ausente");
  }
  Require(calls == 2 && cache.Resolutions() == 2 && cache.Size() == 2, "uma resolução por identidade");
  cache.Clear();
  Require(cache.Size() == 0, "Clear esvazia");
}

// Pacote 4: as mesmas regras do backend GL (ToPicaGlWrap, PicaGlMinFilter,
// faixa de LOD) para o deko3d amostrar igual.
void TestSamplerAndTextureKeys() {
  using namespace Fast::Deko3d;
  Require(MapPicaWrap(0) == PicaWrap::ClampToEdge, "wrap 0 = clamp to edge");
  Require(MapPicaWrap(2) == PicaWrap::Repeat && MapPicaWrap(6) == PicaWrap::Repeat &&
              MapPicaWrap(7) == PicaWrap::Repeat, "wrap 2/6/7 = repeat");
  Require(MapPicaWrap(3) == PicaWrap::MirroredRepeat, "wrap 3 = mirrored");
  Require(MapPicaWrap(1) == PicaWrap::Unsupported && MapPicaWrap(4) == PicaWrap::Unsupported &&
              MapPicaWrap(9) == PicaWrap::Unsupported, "borda/assimétrico/inválido = não suportado");
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
  Require(key.Mip == PicaMipFilter::Linear, "mip linear com cadeia");
  Require(key.MaxLevel == 3, "MAX_LEVEL limitado à cadeia enviada (min(7, 4-1))");
  Require(key.MinLevel == 3, "BASE_LEVEL não passa do MAX_LEVEL");
  Require(key.WrapS == PicaWrap::Repeat && key.WrapT == PicaWrap::MirroredRepeat && key.LodBiasRaw == -128,
          "wrap e bias");
  Require(MakePicaSamplerKey(view, 1).Mip == PicaMipFilter::None, "sem mip: filtro de mip desligado");
  PicaSamplerKeyHash samplerHash;
  Require(samplerHash(key) == samplerHash(MakePicaSamplerKey(view, 4)), "hash estável");
  const auto textureKey = MakePicaTextureKey(view, 0x1234);
  auto other = view;
  other.NativeWrapS = 0;
  Require(!(textureKey == MakePicaTextureKey(other, 0x1234)), "estado do sampler entra na chave (como no GL)");
  PicaTextureKeyHash textureHash;
  Require(textureHash(textureKey) == textureHash(MakePicaTextureKey(view, 0x1234)), "hash de textura estável");
  Require(ClassifyPicaTexture(view) == PicaTextureUse::Color, "tipo 0 = cor");
  view.NativeType = 2;
  Require(ClassifyPicaTexture(view) == PicaTextureUse::Shadow2D, "tipo 2 no slot 0 = Shadow2D");
  view.Slot = 1;
  Require(ClassifyPicaTexture(view) == PicaTextureUse::Unsupported, "tipo 2 fora do slot 0 = rejeitado");
  view.Slot = 0;
  view.NativeType = 3;
  Require(ClassifyPicaTexture(view) == PicaTextureUse::Color, "tipo 3 no slot 0 = cor");
}

void TestMipPlanAndSlots() {
  using namespace Fast::Deko3d;
  std::array<PicaMipPlan, kMaxPicaMipLevels> mips{};
  const uint32_t total = PlanPicaRgba8Mips(64, 32, 3, 256, mips);
  Require(mips[0].Bytes == 64 * 32 * 4 && mips[0].Offset == 0, "nível 0");
  Require(mips[1].Width == 32 && mips[1].Height == 16 && mips[1].Offset == 8192, "nível 1 alinhado");
  Require(mips[2].Width == 16 && mips[2].Height == 8 && mips[2].Offset == 8192 + 2048, "nível 2");
  Require(total == 8192 + 2048 + 512, "total da cadeia");
  Require(PlanPicaRgba8Mips(0, 8, 1, 256, mips) == 0 && PlanPicaRgba8Mips(8, 8, 17, 256, mips) == 0,
          "cadeia vazia/longa demais = 0");
  DescriptorSlots<4> slots(2);
  Require(slots.Allocate() == 2 && slots.Allocate() == 3, "reservados pulados");
  Require(slots.Allocate() == 4, "cheio devolve a capacidade");
  slots.Free(2);
  Require(slots.Live() == 3 && slots.Allocate() == 2, "slot livre reaproveitado");
  slots.Free(9);
  Require(slots.Live() == 4, "slot fora da faixa ignorado");
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
  Require(PlanPicaVertices(bindings, attributes, plan), "plano válido");
  Require(plan.BindingOffset[0] == 0 && plan.BindingOffset[3] == 48 && plan.StreamBytes == 58,
          "bindings alinhados a 16 num bloco só");
  Require(plan.Attributes[0].Kind == PicaAttributeKind::Float && plan.Attributes[0].Components == 3,
          "posição float x3");
  Require(plan.Attributes[5].Kind == PicaAttributeKind::SignedShort && plan.Attributes[5].Binding == 3,
          "atributo 5 no binding 3");
  // Regressão de desenho: atributo ausente lê (0,0,0,1) como array GL desligado.
  Require(plan.Attributes[1].Kind == PicaAttributeKind::Constant &&
              plan.Attributes[1].Binding == kPicaConstantVertexBinding, "ausente = buffer constante");
  Require(plan.AttributeMask == 0x21 && plan.BindingMask == 0x9, "máscaras");
  const PicaVertexAttributeView orphan[] = {{2, 7, PicaVertexFormat::Float, 4, 0}};
  Require(!PlanPicaVertices(bindings, orphan, plan) && plan.Error != nullptr, "atributo sem binding reprova");
  const PicaVertexBindingView reserved[] = {{15, 4, false, a}};
  Require(!PlanPicaVertices(reserved, {}, plan), "binding 15 é do buffer constante");
  const PicaVertexAttributeView five[] = {{0, 0, PicaVertexFormat::Float, 5, 0}};
  Require(!PlanPicaVertices(bindings, five, plan), "5 componentes reprova");
  Require(PicaUniformBindBytes(1) == 256 && PicaUniformBindBytes(256) == 256 && PicaUniformBindBytes(6112) == 6144 &&
              PicaUniformBindBytes(1 << 20) == 0x10000, "tamanho de UBO alinhado e limitado a 64 KiB");
}

} // namespace

// Off the Switch the selection is constant (gfx_deko3d_selection.cpp is not
// linked into this test).
namespace Fast {
bool Deko3dRendererSelected() { return false; }
} // namespace Fast

// ---- pacote 5: alvos, estado, transfer, scanout ----

void TestPipelineEnums() {
  using namespace Fast::Deko3d;
  using C = Fast::Renderer3ds::PicaCompareFunction;
  // Valores do deko3d.h (DkCompareOp_*): Never=1 ... Always=8.
  Require(DekoCompareOp(C::Never) == 1 && DekoCompareOp(C::Less) == 2 && DekoCompareOp(C::Equal) == 3 &&
              DekoCompareOp(C::LessOrEqual) == 4 && DekoCompareOp(C::Greater) == 5 &&
              DekoCompareOp(C::NotEqual) == 6 && DekoCompareOp(C::GreaterOrEqual) == 7 &&
              DekoCompareOp(C::Always) == 8,
          "comparações no valor do DkCompareOp");
  using L = Fast::Renderer3ds::PicaLogicOperation;
  // A ordem PICA não é a do GL/deko3d: Set (PICA 4) é DkLogicOp_Set 15.
  Require(DekoLogicOp(L::Copy) == kDekoLogicOpCopy && DekoLogicOp(L::Set) == 15 && DekoLogicOp(L::Xor) == 6 &&
              DekoLogicOp(L::Invert) == 10 && DekoLogicOp(L::NoOp) == 5 && DekoLogicOp(L::Nand) == 14 &&
              DekoLogicOp(L::CopyInverted) == 12 && DekoLogicOp(L::AndInverted) == 4,
          "operação lógica PICA -> DkLogicOp");
  using F = Fast::Renderer3ds::NativeBlendFactor;
  Require(DekoBlendFactor(F::DestColor) == 9 && DekoBlendFactor(F::SourceAlpha) == 5 &&
              DekoBlendFactor(F::OneMinusConstantAlpha) == (4 | 0x20) && DekoBlendFactor(F::SourceAlphaSaturate) == 11,
          "fatores de blend no valor do DkBlendFactor");
  using T = Fast::Renderer3ds::PicaTopology;
  Require(DekoPrimitive(T::TriangleList) == 4 && DekoPrimitive(T::GeometryShader) == 4 &&
              DekoPrimitive(T::TriangleStrip) == 5 && DekoPrimitive(T::TriangleFan) == 6,
          "topologias como no GL");
}

void TestCullPlan() {
  using namespace Fast::Deko3d;
  using M = Fast::Renderer3ds::NativeCullMode;
  Require(!PlanPicaCull(M::KeepAll, false, false).Enabled, "KeepAll sem cull");
  // Origem em cima (como o Vulkan): o sentido de face inverte em relação ao GL.
  const auto ccw = PlanPicaCull(M::KeepCounterClockwise, false, false);
  Require(ccw.Enabled && !ccw.CullFront && !ccw.FrontCcw, "KeepCCW -> frente CW, cull de trás");
  const auto cw = PlanPicaCull(M::KeepClockwise, true, false);
  Require(cw.Enabled && cw.CullFront && cw.FrontCcw, "framebuffer invertido corta a frente");
  Require(PlanPicaCull(M::KeepCounterClockwise, false, true).FrontCcw, "config/deko3d_cull_invert volta ao GL");
}

void TestTargetsAndRects() {
  using namespace Fast::Deko3d;
  Require(kTopScreenPhysicalWidth == Fast::Oot3d::kNativeTopScreenPhysicalWidth &&
              kTopScreenPhysicalHeight == Fast::Oot3d::kNativeTopScreenPhysicalHeight,
          "tela de cima = a do GL");
  const auto top = PlanPicaTargetExtent(480, 400, 2.0F, 1.0666F, 480, 400);
  Require(top.Width == 960 && top.Wide && top.Height == 853 && top.ScaleY > 2.13F, "alvo de cima alargado");
  const auto bottom = PlanPicaTargetExtent(320, 240, 2.0F, 1.0666F, 480, 400);
  Require(bottom.Width == 640 && bottom.Height == 480 && !bottom.Wide, "outros alvos não alargam");
  Require(!PlanPicaTargetExtent(480, 400, 3.0F, 1.0F, 480, 400).Wide, "widescreen desligado");
  Require(ScaledEdge(7, 1.5F) == 11 && ScaledExtent(0, 2.0F) == 0 && ScaledExtent(1, 0.1F) == 1,
          "bordas iguais às do PicaGlScale");
  const PicaRect viewport = PlanPicaViewport(10.7F, 0.0F, 240.9F, 400.0F, 2.0F);
  Require(viewport == PicaRect{20, 0, 480, 800}, "viewport trunca e escala as duas bordas");
  Require(PlanPicaScissor(3, 5, 6, 14, 6, 2.0F, 960, 800) == PicaRect{10, 12, 20, 2}, "scissor inclusivo");
  Require(PlanPicaScissor(0, 5, 6, 14, 6, 2.0F, 960, 800) == PicaRect{0, 0, 960, 800}, "sem scissor = alvo");
  Require(PlanPicaScissor(3, 9, 0, 3, 0, 1.0F, 64, 64).Width == 0, "scissor invertido vazio");
  Require(ClipPicaRect({-4, 10, 20, 100}, 64, 64) == PicaRect{0, 10, 16, 54}, "recorte ao alvo");
  Require(ClipPicaRect({70, 0, 5, 5}, 64, 64) == PicaRect{}, "fora do alvo = vazio");
  PicaTargetKey a{1, 0x18000000, 0x18100000, 240, 400, 0, 3};
  PicaTargetKey b = a;
  Require(a == b && PicaTargetKeyHash{}(a) == PicaTargetKeyHash{}(b), "chave igual");
  b.DepthFormat = 2;
  Require(!(a == b), "formato de depth diferencia");
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
  Require(coverage.Color && !coverage.Depth, "fill cobre a cor; sem depth (endereço 0)");
  const auto color = DecodePicaClearColor(fill, 0);
  Require(color[0] == 0x11 / 255.0F && color[3] == 0x44 / 255.0F, "RGBA8 do fill (byte 3 = R)");
  PicaTargetKey shortKey = key;
  shortKey.Height = 401;
  Require(!PicaFillCovers(fill, shortKey).Color, "fill menor que o alvo não limpa");
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
  Require(plan.Valid && !plan.Linear && plan.SourceWidth == 480 && plan.ContentHeight == 800, "cópia 1:1");
  transfer.Flags = 1U << 24U; // 2:1 horizontal
  transfer.OutputWidth = 120;
  plan = PlanPicaTransfer(transfer, 240, 400, 2.0F, 2.0F, 480, 800);
  Require(plan.Valid && plan.Linear && plan.OutputWidth == 120 && plan.SourceWidth == 480 && plan.AllocWidth == 240,
          "2:1 linear lê o dobro");
  transfer.Flags = 8U;
  Require(!PlanPicaTransfer(transfer, 240, 400, 2.0F, 2.0F, 480, 800).Valid, "modo não suportado rejeita");
  transfer.Flags = 0;
  transfer.InputWidth = 300;
  Require(!PlanPicaTransfer(transfer, 240, 400, 2.0F, 2.0F, 480, 800).Valid, "maior que a fonte rejeita");
  // Flip: entrada não linear sem dont-swizzle e sem bit 0 = invertido (GL).
  transfer.InputWidth = 240;
  transfer.OutputWidth = 240;
  Require(PlanPicaTransfer(transfer, 240, 400, 1.0F, 1.0F, 240, 400).Flip, "flip padrão como no GL");
  transfer.Flags = 1U;
  Require(!PlanPicaTransfer(transfer, 240, 400, 1.0F, 1.0F, 240, 400).Flip, "bit 0 desfaz o flip");
  // Tela de cima 400x240 apresentada em 1280x720: 5:3 com barras.
  const auto placement = PlanPicaScanout(240, 400, 800, 2.0F, 1280, 720);
  Require(placement.Viewport == PicaRect{40, 0, 1200, 720} && !placement.CoversWindow, "5:3 centralizado");
  // Alargada (16:9): cobre a janela sem barra de 1 pixel.
  const auto wide = PlanPicaScanout(240, 400, 853, 2.0F, 1280, 720);
  Require(wide.CoversWindow && wide.Viewport.Width == 1280, "16:9 cobre a janela");
}

void TestLogSchedule() {
  // Linhas que mostram se o p5 chegou ao console (24/10: o log não dizia de
  // qual NRO vinha nem se o PICA chegava ao backend antes do quadro 1800).
  Require(std::string_view(Fast::kDeko3dBuildTag) == "p5b", "etiqueta do build no topo do log");
  Require(Fast::Deko3dFrameLogDue(1) && Fast::Deko3dFrameLogDue(60) && Fast::Deko3dFrameLogDue(300) &&
              Fast::Deko3dFrameLogDue(1800) && Fast::Deko3dFrameLogDue(7200) && !Fast::Deko3dFrameLogDue(61) &&
              !Fast::Deko3dFrameLogDue(0),
          "quadros com log");
  Require(Fast::Deko3dPicaLogDue(300) && Fast::Deko3dPicaLogDue(1800) && Fast::Deko3dPicaLogDue(3600) &&
              !Fast::Deko3dPicaLogDue(60) && !Fast::Deko3dPicaLogDue(0),
          "contadores PICA cedo (300) e a cada 1800");
}

int main() {
  TestRendererConfigSelection();
  TestShaderPack();
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
  std::puts("oot3d_deko3d_backend_tests: ok");
  return 0;
}
