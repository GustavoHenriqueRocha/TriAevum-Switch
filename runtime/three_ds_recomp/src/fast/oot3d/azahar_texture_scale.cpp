#include "oot3d/renderer/azahar_texture_scale.h"

#include <algorithm>
#include <cmath>

namespace Oot3d::Renderer {
namespace {

// Source texels covered by each output texel of one axis, with the share of
// the output texel each one covers (the shares of one output sum to 1).
struct AxisWeights {
    std::vector<uint32_t> First;
    std::vector<uint32_t> Count;
    std::vector<float> Weights; // Count[i] entries per output, in order
};

AxisWeights MakeAxisWeights(uint32_t sourceSize, uint32_t outputSize) {
    AxisWeights axis;
    axis.First.resize(outputSize);
    axis.Count.resize(outputSize);
    const double ratio = static_cast<double>(sourceSize) / outputSize;
    for (uint32_t output = 0; output < outputSize; ++output) {
        const double begin = output * ratio;
        const double end = output + 1U == outputSize ? sourceSize : (output + 1U) * ratio;
        const auto first = static_cast<uint32_t>(begin);
        const auto last = std::min(sourceSize, static_cast<uint32_t>(std::ceil(end)));
        axis.First[output] = first;
        axis.Count[output] = last - first;
        for (uint32_t source = first; source < last; ++source) {
            const double covered = std::min<double>(end, source + 1U) - std::max<double>(begin, source);
            axis.Weights.push_back(static_cast<float>(covered / (end - begin)));
        }
    }
    return axis;
}

uint8_t ToUnorm8(float value) noexcept {
    return static_cast<uint8_t>(std::clamp(std::lround(value), 0L, 255L));
}

} // namespace

TextureSize CappedReplacementSize(TextureSize native, TextureSize replacement, uint32_t maxScale) noexcept {
    if (maxScale == 0U || native.Width == 0U || native.Height == 0U) {
        return replacement;
    }
    const uint64_t width = static_cast<uint64_t>(native.Width) * maxScale;
    const uint64_t height = static_cast<uint64_t>(native.Height) * maxScale;
    return {static_cast<uint32_t>(std::min<uint64_t>(replacement.Width, width)),
            static_cast<uint32_t>(std::min<uint64_t>(replacement.Height, height))};
}

bool TexturePackCapChangesImage(TextureSize native, TextureSize source, uint32_t fromCap, uint32_t toCap) noexcept {
    return CappedReplacementSize(native, source, fromCap) != CappedReplacementSize(native, source, toCap);
}

std::vector<uint8_t> DownscaleRgba8(std::span<const uint8_t> source, TextureSize from, TextureSize to) {
    if (from.Width == 0U || from.Height == 0U || to.Width == 0U || to.Height == 0U || to.Width > from.Width ||
        to.Height > from.Height || source.size() != static_cast<size_t>(from.Width) * from.Height * 4U) {
        return {};
    }
    const AxisWeights columns = MakeAxisWeights(from.Width, to.Width);
    const AxisWeights rows = MakeAxisWeights(from.Height, to.Height);
    std::vector<size_t> columnStart(to.Width);
    for (size_t x = 0, offset = 0; x < to.Width; offset += columns.Count[x], ++x) {
        columnStart[x] = offset;
    }
    std::vector<uint8_t> output(static_cast<size_t>(to.Width) * to.Height * 4U);
    size_t rowWeight = 0U;
    for (uint32_t y = 0; y < to.Height; rowWeight += rows.Count[y], ++y) {
        for (uint32_t x = 0; x < to.Width; ++x) {
            float alpha = 0.0F;
            float premultiplied[3] = {};
            float plain[3] = {};
            for (uint32_t row = 0; row < rows.Count[y]; ++row) {
                const float wy = rows.Weights[rowWeight + row];
                const uint8_t* line =
                    source.data() + (static_cast<size_t>(rows.First[y] + row) * from.Width + columns.First[x]) * 4U;
                for (uint32_t column = 0; column < columns.Count[x]; ++column) {
                    const float weight = wy * columns.Weights[columnStart[x] + column];
                    const uint8_t* texel = line + static_cast<size_t>(column) * 4U;
                    const float coverage = weight * texel[3];
                    alpha += coverage;
                    for (int channel = 0; channel < 3; ++channel) {
                        premultiplied[channel] += coverage * texel[channel];
                        plain[channel] += weight * texel[channel];
                    }
                }
            }
            uint8_t* out = output.data() + (static_cast<size_t>(y) * to.Width + x) * 4U;
            for (int channel = 0; channel < 3; ++channel) {
                out[channel] = ToUnorm8(alpha > 0.0F ? premultiplied[channel] / alpha : plain[channel]);
            }
            out[3] = ToUnorm8(alpha);
        }
    }
    return output;
}

uint32_t FullMipLevelCount(TextureSize size) noexcept {
    if (size.Width == 0U || size.Height == 0U) {
        return 0U;
    }
    uint32_t levels = 1U;
    for (uint32_t largest = std::max(size.Width, size.Height); largest > 1U; largest >>= 1U) {
        ++levels;
    }
    return levels;
}

std::vector<std::vector<uint8_t>> BuildRgba8MipChain(std::span<const uint8_t> level0, TextureSize size,
                                                     uint32_t levels) {
    std::vector<std::vector<uint8_t>> chain;
    levels = std::min(levels, FullMipLevelCount(size));
    if (levels <= 1U || level0.size() != static_cast<size_t>(size.Width) * size.Height * 4U) {
        return chain;
    }
    chain.reserve(levels - 1U);
    std::span<const uint8_t> previous = level0;
    for (uint32_t level = 1; level < levels; ++level) {
        const TextureSize next{std::max(1U, size.Width >> 1U), std::max(1U, size.Height >> 1U)};
        chain.push_back(DownscaleRgba8(previous, size, next));
        previous = chain.back();
        size = next;
    }
    return chain;
}

} // namespace Oot3d::Renderer
