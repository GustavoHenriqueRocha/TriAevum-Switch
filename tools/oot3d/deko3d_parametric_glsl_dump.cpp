// Writes the GLSL 4.3 text of the six parametric (uber) fragment programs
// the deko3d backend falls back to (unlit, lit with lookup tables, lit
// without, each with a float or an integer texture 0), as frag_<key>.glsl,
// the same names the game records for shaders missing from
// shadercache/deko3d.pack. Then tools/oot3d/deko3d_shader_pack.py --glsl <dir>
// compiles them into the pack, so the always-available programs are there
// from the first launch.
//
//   oot3d_deko3d_parametric_glsl_dump <dir>
#include "fast/backends/gfx_deko3d_shader_pack.h"
#include "fast/renderer3ds/pica_gl_shader_dialect.h"
#include "oot3d_native_pica_fragment_shader_gen.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <directory>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    struct Base {
        bool Lighting;
        bool IntegerTexture0;
        bool LightingLuts;
    };
    constexpr Base kBases[] = {{false, false, true}, {true, false, true},  {false, true, true},
                               {true, true, true},   {true, false, false}, {true, true, false}};
    int written = 0;
    for (const Base& base : kBases) {
        Oot3dNativeGame::Oot3dPicaGeneratedFragmentShader generated;
        std::string error;
        const unsigned index = static_cast<unsigned>(&base - kBases);
        if (!Oot3dNativeGame::GenerateOot3dPicaParametricBaseFragmentShader(base.Lighting, base.IntegerTexture0,
                                                                              generated, &error, base.LightingLuts)) {
            std::fprintf(stderr, "base %u: no parametric program (%s)\n", index, error.c_str());
            continue;
        }
        std::string translated;
        if (!Fast::Renderer3ds::TranslatePicaShaderToOpenGl43(generated.Source, translated, &error)) {
            std::fprintf(stderr, "base %u: GL 4.3 translation failed (%s)\n", index, error.c_str());
            return 1;
        }
        const uint64_t key = Fast::Deko3d::ShaderTextKey(Fast::Deko3d::ShaderStage::Fragment, translated);
        char name[64];
        std::snprintf(name, sizeof(name), "frag_%016llx.glsl", static_cast<unsigned long long>(key));
        std::ofstream(directory / name, std::ios::binary | std::ios::trunc) << translated;
        std::printf("base %u (light=%u, luts=%u, integer texture0=%u): %s, %zu bytes\n", index,
                    base.Lighting ? 1U : 0U, base.Lighting && base.LightingLuts ? 1U : 0U,
                    base.IntegerTexture0 ? 1U : 0U, name, translated.size());
        ++written;
    }
    return written == static_cast<int>(std::size(kBases)) ? 0 : 1;
}
