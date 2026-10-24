// Pure parts of the deko3d backend (docs/DEKO3D_PROGRESSO.md): what runs
// without a GPU is tested here on the host.
#include "fast/backends/gfx_deko3d.h"
#include "fast/backends/gfx_deko3d_shader_pack.h"

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

} // namespace

// Off the Switch the selection is constant (gfx_deko3d_selection.cpp is not
// linked into this test).
namespace Fast {
bool Deko3dRendererSelected() { return false; }
} // namespace Fast

int main() {
  TestRendererConfigSelection();
  TestShaderPack();
  std::puts("oot3d_deko3d_backend_tests: ok");
  return 0;
}
