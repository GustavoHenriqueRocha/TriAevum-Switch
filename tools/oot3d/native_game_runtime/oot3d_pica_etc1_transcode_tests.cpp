#include "fast/oot3d/pica_etc1_transcode.h"
#include "fast/oot3d/pica_texture_decode.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

namespace {

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

int Clamp(int value) { return value < 0 ? 0 : (value > 255 ? 255 : value); }

// ETC1 per the Khronos spec over raster-ordered big-endian blocks: what the
// GPU samples from GL_COMPRESSED_RGB8_ETC2 for a valid ETC1 stream.
std::vector<uint8_t> DecodeStandardEtc1(const std::vector<uint8_t> &blocks,
                                        uint32_t width, uint32_t height) {
  static constexpr int kTable[8][2] = {{2, 8},   {5, 17},  {9, 29},
                                       {13, 42}, {18, 60}, {24, 80},
                                       {33, 106}, {47, 183}};
  std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4U);
  const uint32_t blocksX = width / 4U;
  for (uint32_t by = 0; by < height / 4U; ++by) {
    for (uint32_t bx = 0; bx < blocksX; ++bx) {
      const uint8_t *b = blocks.data() + (by * blocksX + bx) * 8U;
      int base[2][3];
      const bool diff = (b[3] & 2U) != 0U;
      const bool flip = (b[3] & 1U) != 0U;
      for (int c = 0; c < 3; ++c) {
        if (diff) {
          const int b5 = b[c] >> 3;
          const int d = (b[c] & 7) - ((b[c] & 4) ? 8 : 0);
          base[0][c] = (b5 << 3) | (b5 >> 2);
          base[1][c] = ((b5 + d) << 3) | ((b5 + d) >> 2);
        } else {
          base[0][c] = (b[c] >> 4) * 17;
          base[1][c] = (b[c] & 15) * 17;
        }
      }
      const int table[2] = {b[3] >> 5, (b[3] >> 2) & 7};
      const uint32_t msb = (b[4] << 8) | b[5];
      const uint32_t lsb = (b[6] << 8) | b[7];
      for (uint32_t x = 0; x < 4; ++x) {
        for (uint32_t y = 0; y < 4; ++y) {
          const uint32_t i = x * 4 + y;
          const int sub = flip ? (y >= 2) : (x >= 2);
          int modifier = kTable[table[sub]][(lsb >> i) & 1U];
          if ((msb >> i) & 1U)
            modifier = -modifier;
          uint8_t *out = &rgba[((by * 4 + y) * width + bx * 4 + x) * 4U];
          for (int c = 0; c < 3; ++c)
            out[c] = static_cast<uint8_t>(Clamp(base[sub][c] + modifier));
          out[3] = 255;
        }
      }
    }
  }
  return rgba;
}

// Random PICA ETC1(A4) words, differential overflow cleared unless kept.
std::vector<uint8_t> RandomPica(std::mt19937 &rng, uint32_t width,
                                uint32_t height, bool alpha, bool opaque) {
  const size_t blockBytes = alpha ? 16U : 8U;
  std::vector<uint8_t> bytes(width * height / 16U * blockBytes);
  for (auto &byte : bytes)
    byte = static_cast<uint8_t>(rng());
  for (size_t at = 0; at < bytes.size(); at += blockBytes) {
    if (alpha && opaque)
      for (size_t i = 0; i < 8; ++i)
        bytes[at + i] = 0xFF;
    uint8_t *word = &bytes[at + (alpha ? 8U : 0U)];
    for (int c = 0; c < 3; ++c) {
      const int b5 = word[7 - c] >> 3;
      const int d = (word[7 - c] & 7) - ((word[7 - c] & 4) ? 8 : 0);
      if (b5 + d < 0 || b5 + d > 31)
        word[7 - c] &= 0xF8; // delta 0
    }
  }
  return bytes;
}

} // namespace

int main() {
  using namespace Fast::Oot3d;
  std::mt19937 rng(1234);
  for (const auto [width, height] :
       std::array<std::array<uint32_t, 2>, 4>{{{8, 8}, {64, 32}, {16, 128},
                                                {256, 256}}}) {
    for (const bool alpha : {false, true}) {
      const auto pica = RandomPica(rng, width, height, alpha, true);
      std::vector<uint8_t> blocks;
      Require(TranscodePicaEtc1ToEtc2(pica, width, height, alpha, blocks),
              "transcodifica bloco válido");
      Require(blocks.size() == width * height / 2U, "4 bits por texel");
      std::vector<uint8_t> expected;
      std::string error;
      Require(DecodePicaTextureRgba8(alpha ? 13U : 12U,
                                     static_cast<uint16_t>(width),
                                     static_cast<uint16_t>(height), pica,
                                     expected, &error),
              "decodificador PICA");
      Require(DecodeStandardEtc1(blocks, width, height) == expected,
              "ETC2 na GPU == decodificação PICA em RGBA8");
    }
  }
  {
    // Overflow diferencial: ETC2 leria um bloco T/H/planar.
    auto pica = RandomPica(rng, 8, 8, false, true);
    pica[4] |= 0x02U; // bit 33 (diferencial)
    pica[7] = 0x04U; // R base 0, delta -4
    std::vector<uint8_t> blocks;
    Require(!TranscodePicaEtc1ToEtc2(pica, 8, 8, false, blocks),
            "recusa overflow diferencial");
  }
  {
    auto pica = RandomPica(rng, 8, 8, true, true);
    pica[16 + 3] = 0xEFU;
    std::vector<uint8_t> blocks;
    Require(!TranscodePicaEtc1ToEtc2(pica, 8, 8, true, blocks),
            "recusa ETC1A4 com alpha < 15");
  }
  {
    const auto pica = RandomPica(rng, 16, 16, false, true);
    std::vector<uint8_t> blocks;
    Require(!TranscodePicaEtc1ToEtc2(pica, 12, 16, false, blocks),
            "recusa lado que não é múltiplo de 8");
    Require(!TranscodePicaEtc1ToEtc2(std::span(pica).first(100), 16, 16,
                                     false, blocks),
            "recusa dados truncados");
  }
  std::puts("oot3d_pica_etc1_transcode_tests: ok");
  return 0;
}
