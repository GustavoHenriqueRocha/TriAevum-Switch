#include "fast/oot3d/pica_texture_predecode.h"

#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

std::vector<uint8_t> Pattern(size_t size, uint8_t seed) {
  std::vector<uint8_t> bytes(size);
  for (size_t i = 0; i < size; ++i)
    bytes[i] = static_cast<uint8_t>(seed + i * 37U);
  return bytes;
}

} // namespace

int main() {
  using namespace Fast::Oot3d;
  {
    // Mip count/size same as the GL backend's (64x32 RGBA8: 64x32, 32x16, 16x8).
    Require(PicaTextureMipCount(64, 32, 7) == 3U, "3 levels");
    Require(PicaTextureMipCount(64, 32, 0) == 1U, "max level 0");
    Require(PicaTextureMipCount(0, 32, 7) == 0U, "no size");
    Require(PicaTextureEncodedMipSize(0, 64, 32) == size_t{64 * 32 * 4}, "RGBA8");
    Require(PicaTextureEncodedMipSize(12, 4, 4) == size_t{8 * 8 / 2}, "ETC1 minimum 8x8");
    Require(!PicaTextureEncodedMipSize(14, 8, 8), "invalid format");
  }
  {
    // The predecoded mips are exactly those of the per-level decoding
    // the render thread used to do.
    constexpr uint8_t kFormat = 0U; // RGBA8
    const auto bytes = Pattern(64 * 32 * 4 + 32 * 16 * 4 + 16 * 8 * 4, 3);
    auto decoded = DecodePicaTextureMips(kFormat, 64, 32, 7, bytes);
    Require(decoded && decoded->Mips.size() == 3U, "3 mips decoded");
    size_t offset = 0;
    for (uint32_t level = 0; level < 3; ++level) {
      const uint32_t w = 64U >> level, h = 32U >> level;
      const size_t encoded = *PicaTextureEncodedMipSize(kFormat, w, h);
      std::vector<uint8_t> reference;
      Require(::Oot3d::Renderer::DecodePicaTextureRgba8(
                  kFormat, static_cast<uint16_t>(w), static_cast<uint16_t>(h),
                  std::span<const uint8_t>(bytes).subspan(offset, encoded), reference),
              "reference decoding");
      Require(decoded->Mips[level] == reference, "mip same as the render thread's");
      offset += encoded;
    }
    Require(decoded->Bytes == (64 * 32 + 32 * 16 + 16 * 8) * 4U, "bytes counted");
    // Truncated chain: nothing is stored (the render thread reports the error).
    Require(!DecodePicaTextureMips(kFormat, 64, 32, 7,
                                   std::span<const uint8_t>(bytes).first(64 * 32 * 4)),
            "truncated");
  }
  {
    // Each content is decoded once and delivered once.
    PicaPredecodeCache cache(1U << 20U);
    const PicaPredecodeKey key{0x1234, 8, 8, 0, 1};
    Require(cache.Claim(key), "first claim");
    Require(!cache.Claim(key), "repeated claim");
    cache.Put(key, DecodePicaTextureMips(0, 8, 8, 0, Pattern(256, 1)));
    auto taken = cache.Take(key);
    Require(taken && taken->Mips[0].size() == 256U, "take hits");
    Require(!cache.Take(key), "delivered only once");
    // Same content with another format is another texture.
    Require(cache.Claim({0x1234, 8, 8, 1, 1}), "format is part of the key");
    // A texture the render thread created on its own is not decoded later.
    const PicaPredecodeKey late{0x9999, 8, 8, 0, 1};
    Require(!cache.Take(late), "take without worker misses");
    Require(!cache.Claim(late), "worker doesn't redo what was already uploaded");
    const auto stats = cache.Snapshot();
    Require(stats.Hits == 1U && stats.Misses == 2U && stats.Entries == 0U && stats.Bytes == 0U,
            "stats");
  }
  {
    // Memory cap: the oldest leave first and the accounting adds up.
    PicaPredecodeCache cache(3U * 256U);
    for (uint64_t hash = 1; hash <= 5; ++hash)
      cache.Put({hash, 8, 8, 0, 1}, DecodePicaTextureMips(0, 8, 8, 0, Pattern(256, uint8_t(hash))));
    const auto stats = cache.Snapshot();
    Require(stats.Entries == 3U && stats.Bytes == 3U * 256U && stats.Evicted == 2U, "cap");
    Require(!cache.Take({1, 8, 8, 0, 1}) && cache.Take({5, 8, 8, 0, 1}), "FIFO");
    // Larger than the whole cap: doesn't even go in.
    cache.Put({7, 16, 16, 0, 1}, DecodePicaTextureMips(0, 16, 16, 0, Pattern(1024, 7)));
    Require(!cache.Take({7, 16, 16, 0, 1}), "too large");
  }
  {
    // The order queue doesn't grow unbounded when everything is consumed.
    PicaPredecodeCache cache(1U << 20U);
    for (uint64_t hash = 1; hash <= 20000; ++hash) {
      cache.Put({hash, 8, 8, 0, 1}, DecodePicaTextureMips(0, 8, 8, 0, Pattern(256, 0)));
      Require(cache.Take({hash, 8, 8, 0, 1}) != nullptr, "consumed");
    }
    Require(cache.Snapshot().Bytes == 0U, "nothing left over");
  }
  {
    // Worker and render thread at the same time (run with TSan to check).
    PicaPredecodeCache cache(64U * 256U);
    std::thread worker([&] {
      for (uint64_t hash = 1; hash <= 2000; ++hash)
        if (cache.Claim({hash, 8, 8, 0, 1}))
          cache.Put({hash, 8, 8, 0, 1}, DecodePicaTextureMips(0, 8, 8, 0, Pattern(256, 0)));
    });
    uint64_t hits = 0;
    for (uint64_t hash = 1; hash <= 2000; ++hash)
      hits += cache.Take({hash, 8, 8, 0, 1}) != nullptr;
    worker.join();
    const auto stats = cache.Snapshot();
    Require(stats.Hits == hits && stats.Hits + stats.Misses == 2000U, "concurrent count");
  }
  std::puts("pica texture predecode: PASS");
  return 0;
}
