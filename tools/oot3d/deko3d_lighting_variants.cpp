// Writes the deko3d lighting variants (toon, per-pixel) of PICA fragment
// texts for the pack: the same mutation the console applies before it looks a
// variant up (gfx_deko3d_lighting.h). Input: a directory of GLSL 4.3 fragment
// texts (frag_<key>.glsl, as the game records them); output: one
// frag_<variant key>.glsl per variant, which
// tools/oot3d/deko3d_shader_pack.py --glsl compiles like any recorded text
// (its --lighting-variants option runs this tool).
//
//   oot3d_deko3d_lighting_variants <input dir> <output dir>
#include "fast/backends/gfx_deko3d_lighting.h"
#include "fast/backends/gfx_deko3d_shader_pack.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <input dir> <output dir>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path input(argv[1]);
    const std::filesystem::path output(argv[2]);
    std::filesystem::create_directories(output);
    using Fast::Deko3d::LightingMode;
    struct Tally {
        unsigned Written = 0;
        unsigned Unsupported = 0;
    };
    Tally toon;
    Tally perPixel;
    unsigned texts = 0;
    for (const auto& entry : std::filesystem::directory_iterator(input)) {
        const auto name = entry.path().filename().string();
        if (!entry.is_regular_file() || name.rfind("frag_", 0) != 0 || entry.path().extension() != ".glsl") {
            continue;
        }
        std::ifstream file(entry.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        ++texts;
        for (const auto mode : {LightingMode::Toon, LightingMode::PerPixel}) {
            Tally& tally = mode == LightingMode::Toon ? toon : perPixel;
            const auto variant = Fast::Deko3d::LightingFragmentVariant(mode, text);
            if (!variant) {
                ++tally.Unsupported;
                std::printf("unsupported %s %s\n", Fast::Deko3d::LightingModeName(mode), name.c_str());
                continue;
            }
            const uint64_t key = Fast::Deko3d::ShaderTextKey(Fast::Deko3d::ShaderStage::Fragment, *variant);
            char out[64];
            std::snprintf(out, sizeof(out), "frag_%016llx.glsl", static_cast<unsigned long long>(key));
            std::ofstream(output / out, std::ios::binary | std::ios::trunc) << *variant;
            std::printf("%s %s %s\n", Fast::Deko3d::LightingModeName(mode), name.c_str(), out);
            ++tally.Written;
        }
    }
    std::printf("lighting variants: %u fragment texts; toon %u (%u unsupported); perpixel %u (%u unsupported)\n",
                texts, toon.Written, toon.Unsupported, perPixel.Written, perPixel.Unsupported);
    return 0;
}
