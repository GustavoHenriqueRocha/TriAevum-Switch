// Pacote de texturas do TopScreen (config/atlas_overrides.o3tu).
// Regressão de 2026-10-22: uma cópia do SD para o Eden corrompeu o .o3tu (mesmo
// tamanho, bytes trocados) e o jogo parou com "payload hash mismatch". O carregador
// precisa recusar qualquer byte trocado ou arquivo cortado com um erro claro, e o
// pacote real (OOT3D_TEST_O3TU=caminho) precisa carregar.
#include "oot3d_top_screen_texture_overrides.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
using Oot3dNativeGame::TopScreenTextureOverridePack;
using Oot3dNativeGame::TopScreenTextureOverrideResult;

void Require(bool condition, const std::string &message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    std::exit(1);
  }
}

void PutU32(Bytes &bytes, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8)
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void PutU64(Bytes &bytes, std::uint64_t value) {
  PutU32(bytes, static_cast<std::uint32_t>(value));
  PutU32(bytes, static_cast<std::uint32_t>(value >> 32U));
}

std::uint64_t Fnv1a64(const Bytes &bytes) {
  std::uint64_t value = 0xCBF29CE484222325ULL;
  for (const auto byte : bytes) {
    value ^= byte;
    value *= 0x100000001B3ULL;
  }
  return value;
}

// Pacote v1: "O3TU", versão, contagem; cada entrada = hash original, hash da
// substituição, tamanho e os bytes da substituição.
Bytes MakePack(const Bytes &original, const Bytes &replacement) {
  Bytes bytes{'O', '3', 'T', 'U'};
  PutU32(bytes, 1U);
  PutU32(bytes, 1U);
  PutU64(bytes, Fnv1a64(original));
  PutU64(bytes, Fnv1a64(replacement));
  PutU32(bytes, static_cast<std::uint32_t>(replacement.size()));
  bytes.insert(bytes.end(), replacement.begin(), replacement.end());
  return bytes;
}

std::string LoadError(const Bytes &bytes) {
  TopScreenTextureOverridePack pack;
  std::string error;
  Require(!pack.LoadBytes(bytes, &error), "pacote inválido foi aceito");
  return error;
}

} // namespace

int main() {
  const Bytes original(64, 0x11);
  Bytes replacement(64);
  for (std::size_t index = 0; index < replacement.size(); ++index)
    replacement[index] = static_cast<std::uint8_t>(index * 7U);
  const Bytes good = MakePack(original, replacement);

  {
    TopScreenTextureOverridePack pack;
    std::string error;
    Require(pack.LoadBytes(good, &error), "pacote válido: " + error);
    Bytes texture = original;
    Require(pack.Apply(texture) == TopScreenTextureOverrideResult::Applied,
            "substituição aplicada");
    Require(texture == replacement, "bytes substituídos");
  }
  // Cada byte do payload trocado precisa ser pego (o caso do Eden).
  for (std::size_t index = good.size() - replacement.size(); index < good.size(); ++index) {
    Bytes corrupt = good;
    corrupt[index] ^= 0x5AU;
    Require(LoadError(corrupt) == "TopScreen texture override payload hash mismatch",
            "byte " + std::to_string(index) + " trocado no payload");
  }
  // Cortado em qualquer ponto: erro claro, nunca leitura fora do buffer.
  for (std::size_t size = 0; size < good.size(); ++size) {
    const Bytes cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(size));
    Require(!LoadError(cut).empty(), "corte em " + std::to_string(size) + " bytes sem erro");
  }
  {
    Bytes trailing = good;
    trailing.push_back(0U);
    Require(LoadError(trailing) == "TopScreen texture override pack has trailing data",
            "lixo no fim");
  }
  {
    Bytes header = good;
    header[0] = 'X';
    Require(LoadError(header) == "invalid TopScreen texture override pack header", "magic");
  }

  if (const char *real = std::getenv("OOT3D_TEST_O3TU"); real != nullptr && *real != '\0') {
    TopScreenTextureOverridePack pack;
    std::string error;
    Require(pack.Load(real, &error), std::string("pacote real ") + real + ": " + error);
    Require(!pack.Empty(), "pacote real vazio");
    std::puts("texture overrides: pacote real carregou");
  }
  std::puts("texture overrides: PASS");
  return 0;
}
