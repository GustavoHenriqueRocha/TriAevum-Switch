// Pure parts of the deko3d backend (docs/DEKO3D_PROGRESSO.md): what runs
// without a GPU is tested here on the host.
#include "fast/backends/gfx_deko3d.h"
#include "fast/backends/gfx_deko3d_pica_plan.h"
#include "fast/backends/gfx_deko3d_programs.h"
#include "fast/backends/gfx_deko3d_shader_pack.h"

#include <array>
#include <cstring>
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

int main() {
  TestRendererConfigSelection();
  TestShaderPack();
  TestProgramPick();
  TestStageCacheResolvesOnce();
  TestSamplerAndTextureKeys();
  TestMipPlanAndSlots();
  TestVertexPlan();
  std::puts("oot3d_deko3d_backend_tests: ok");
  return 0;
}
