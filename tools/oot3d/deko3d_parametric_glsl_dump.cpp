// Writes the GLSL 4.3 text of the four parametric (uber) fragment programs
// the deko3d backend falls back to, as frag_<key>.glsl, the same names the
// game records for shaders missing from shadercache/deko3d.pack. Then
// tools/oot3d/deko3d_shader_pack.py --glsl <dir> compiles them into the pack,
// so the always-available programs are there from the first launch.
//
//   oot3d_deko3d_parametric_glsl_dump <dir>
#include "fast/backends/gfx_deko3d_shader_pack.h"
#include "fast/renderer3ds/pica_gl_shader_dialect.h"
#include "oot3d_native_pica_fragment_shader_gen.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "uso: %s <diretório>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    int written = 0;
    for (unsigned index = 0; index < 4U; ++index) {
        Oot3dNativeGame::Oot3dPicaGeneratedFragmentShader generated;
        std::string error;
        if (!Oot3dNativeGame::GenerateOot3dPicaParametricBaseFragmentShader((index & 1U) != 0U, (index & 2U) != 0U,
                                                                              generated, &error)) {
            std::fprintf(stderr, "base %u: sem programa paramétrico (%s)\n", index, error.c_str());
            continue;
        }
        std::string translated;
        if (!Fast::Renderer3ds::TranslatePicaShaderToOpenGl43(generated.Source, translated, &error)) {
            std::fprintf(stderr, "base %u: tradução GL 4.3 falhou (%s)\n", index, error.c_str());
            return 1;
        }
        const uint64_t key = Fast::Deko3d::ShaderTextKey(Fast::Deko3d::ShaderStage::Fragment, translated);
        char name[64];
        std::snprintf(name, sizeof(name), "frag_%016llx.glsl", static_cast<unsigned long long>(key));
        std::ofstream(directory / name, std::ios::binary | std::ios::trunc) << translated;
        std::printf("base %u (luz=%u, textura0 inteira=%u): %s, %zu bytes\n", index, index & 1U, (index >> 1) & 1U,
                    name, translated.size());
        ++written;
    }
    return written == 4 ? 0 : 1;
}
