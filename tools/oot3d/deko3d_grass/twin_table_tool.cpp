// Offline: the twins of the grass rules' textures among every texture of the
// game (make_twin_table.py feeds it). The runtime's own decode, signature and
// twin score, so the table says what GrassTwinRules would find with every
// signature at hand.
//
//   twin_table_tool <rules.txt> <textures.bin>
// rules.txt:    one rule per line: "<hash hex> <width> <height> <channel 0-4>"
// textures.bin: records of u64 hash, u16 width, u16 height, u8 PICA format,
//               u32 byte count, the base level's encoded bytes
// stdout:       one twin per line: "<twin hash> <w> <h> <rule hash> <w> <h>
//               <channel> <score> <sample scale> <sample offset>"
#include "fast/oot3d/grass_texture_twins.h"
#include "oot3d/renderer/pica_texture_decode.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <vector>

using namespace Fast::Oot3d;

namespace {

struct Texture {
    uint64_t Hash = 0U;
    uint16_t Width = 0U, Height = 0U;
    std::unique_ptr<GrassTextureSignature> Signature;
};

template <class T>
bool Read(std::istream& input, T& value) {
    return static_cast<bool>(input.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s rules.txt textures.bin\n", argv[0]);
        return 2;
    }
    struct Rule {
        uint64_t Hash;
        uint16_t Width, Height;
        unsigned Channel;
    };
    std::vector<Rule> rules;
    if (FILE* file = std::fopen(argv[1], "r")) {
        Rule rule{};
        unsigned width = 0U, height = 0U;
        while (std::fscanf(file, "%" SCNx64 " %u %u %u", &rule.Hash, &width, &height, &rule.Channel) == 4) {
            rule.Width = static_cast<uint16_t>(width);
            rule.Height = static_cast<uint16_t>(height);
            rules.push_back(rule);
        }
        std::fclose(file);
    }
    std::ifstream input(argv[2], std::ios::binary);
    if (rules.empty() || !input) {
        std::fprintf(stderr, "no rules or no textures\n");
        return 1;
    }
    std::map<std::pair<uint64_t, uint32_t>, Texture> textures;
    for (;;) {
        Texture texture;
        uint8_t format = 0U;
        uint32_t size = 0U;
        if (!Read(input, texture.Hash) || !Read(input, texture.Width) || !Read(input, texture.Height) ||
            !Read(input, format) || !Read(input, size))
            break;
        std::vector<uint8_t> bytes(size);
        if (!input.read(reinterpret_cast<char*>(bytes.data()), size)) break;
        const auto key = std::pair(texture.Hash, (uint32_t{texture.Width} << 16U) | texture.Height);
        // As pica_grass_source.cpp: textures of at least 16x16 are decoded.
        if (texture.Width < 16U || texture.Height < 16U || textures.contains(key)) continue;
        std::vector<uint8_t> rgba;
        if (!::Oot3d::Renderer::DecodePicaTextureRgba8(format, texture.Width, texture.Height, bytes, rgba, nullptr))
            continue;
        texture.Signature = std::make_unique<GrassTextureSignature>(
            ComputeGrassTextureSignature(texture.Width, texture.Height, rgba));
        textures.emplace(key, std::move(texture));
    }
    std::fprintf(stderr, "%zu textures decoded\n", textures.size());
    for (const auto& rule : rules) {
        const auto found = textures.find({rule.Hash, (uint32_t{rule.Width} << 16U) | rule.Height});
        if (found == textures.end()) {
            std::fprintf(stderr, "rule texture %016" PRIx64 " %ux%u not in the game\n", rule.Hash, rule.Width,
                         rule.Height);
            return 1;
        }
    }
    for (const auto& [key, twin] : textures) {
        for (const auto& rule : rules) {
            // A texture with a rule of its own never inherits (GrassTwinRules).
            if (std::any_of(rules.begin(), rules.end(), [&](const Rule& own) {
                    return own.Hash == twin.Hash && own.Width == twin.Width && own.Height == twin.Height;
                }))
                break;
            const auto& source = textures.at({rule.Hash, (uint32_t{rule.Width} << 16U) | rule.Height});
            const float score = GrassTextureTwinScore(*source.Signature, *twin.Signature);
            if (score < kGrassTextureTwinThreshold) continue;
            GrassPlacementRule placement;
            placement.Channel = static_cast<GrassSampleChannel>(rule.Channel);
            const auto derived = DeriveGrassTwinRule(placement, *source.Signature, *twin.Signature, twin.Hash);
            std::printf("%016" PRIx64 " %u %u %016" PRIx64 " %u %u %u %.9g %.9g %.9g\n", twin.Hash, twin.Width,
                        twin.Height, rule.Hash, rule.Width, rule.Height, rule.Channel, static_cast<double>(score),
                        static_cast<double>(derived.SampleScale), static_cast<double>(derived.SampleOffset));
        }
    }
    return 0;
}
