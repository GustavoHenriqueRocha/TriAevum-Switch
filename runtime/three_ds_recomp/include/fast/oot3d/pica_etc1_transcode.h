#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace Fast::Oot3d {

// PICA ETC1 (format 12) and ETC1A4 (format 13) re-laid as standard ETC1
// blocks, which GL_COMPRESSED_RGB8_ETC2 samples bit-exactly: the GPU reads
// 4 bits per texel instead of the 32 of the RGBA8 decode.
//
// PICA: 8x8 tiles in raster order, each four 4x4 blocks (Z order), each block
// a little-endian 64-bit word (ETC1A4: 64 alpha bits first). GL: 4x4 blocks
// in raster order, each the same 64-bit word big-endian. No vertical flip:
// DecodePicaTextureRgba8 keeps the PICA row order too.
//
// Refused (false: decode to RGBA8 as before) when:
// - a side is not a multiple of 8 (the tiled layout pads it);
// - a differential block overflows a 5-bit base: ETC1 leaves it undefined
//   (the decoder wraps) and ETC2 reads it as a T/H/planar block;
// - ETC1A4 has any alpha below 15: ETC2 RGB has no alpha.
inline bool TranscodePicaEtc1ToEtc2(std::span<const uint8_t> bytes,
                                    uint32_t width, uint32_t height,
                                    bool hasAlpha,
                                    std::vector<uint8_t>& output) {
    if (width == 0U || height == 0U || (width % 8U) != 0U ||
        (height % 8U) != 0U) {
        return false;
    }
    const size_t blockBytes = hasAlpha ? 16U : 8U;
    const uint32_t blocksX = width / 4U;
    const uint32_t blocksY = height / 4U;
    const size_t blockCount = static_cast<size_t>(blocksX) * blocksY;
    if (bytes.size() < blockCount * blockBytes) {
        return false;
    }
    output.resize(blockCount * 8U);
    const uint32_t tilesX = width / 8U;
    for (uint32_t by = 0U; by < blocksY; ++by) {
        for (uint32_t bx = 0U; bx < blocksX; ++bx) {
            const size_t tile = static_cast<size_t>(by / 2U) * tilesX + bx / 2U;
            const size_t source =
                (tile * 4U + (bx % 2U) + 2U * (by % 2U)) * blockBytes;
            const uint8_t* block = bytes.data() + source;
            if (hasAlpha) {
                for (size_t index = 0U; index < 8U; ++index) {
                    if (block[index] != 0xFFU) {
                        return false;
                    }
                }
                block += 8U;
            }
            // block[7] is the most significant byte of the word.
            if ((block[4] & 0x02U) != 0U) {
                for (uint32_t channel = 0U; channel < 3U; ++channel) {
                    const uint8_t packed = block[7U - channel];
                    const int base = packed >> 3U;
                    const int delta = static_cast<int>(packed & 0x07U) -
                                      ((packed & 0x04U) != 0U ? 8 : 0);
                    if (base + delta < 0 || base + delta > 31) {
                        return false;
                    }
                }
            }
            uint8_t* destination =
                output.data() + (static_cast<size_t>(by) * blocksX + bx) * 8U;
            for (size_t index = 0U; index < 8U; ++index) {
                destination[index] = block[7U - index];
            }
        }
    }
    return true;
}

// config/gl_etc2: "1" sends ETC1 textures as ETC2 (a886ca69). Off by default
// since 2026-10-23 (pgo5l): Mesa 20.1's nvc0 accepts ETC/ASTC only on GK20A
// (class 0xA297), not on the Switch's GM20B, so every ETC2 upload went through
// st/mesa's compressed fallback: a malloc'd copy of the compressed data per
// mip image (in the newlib heap shared with the game) and an ETC2 -> RGBA8
// decode on the render thread, against 4 bits per texel counted by the
// residency budget. Off, the texture takes the RGBA8 decode, predecoded on
// the PICA plan worker.
inline bool PicaEtc2UploadEnabled() {
    static const bool enabled = [] {
#if defined(__SWITCH__)
        std::ifstream file("sdmc:/switch/oot3dre/config/gl_etc2");
        std::string text;
        if (!file || !std::getline(file, text)) {
            return false;
        }
        return !text.empty() && text[0] == '1';
#else
        const char* value = std::getenv("TRIAEVUM_GL_ETC2");
        return value != nullptr && value[0] == '1';
#endif
    }();
    return enabled;
}

} // namespace Fast::Oot3d
