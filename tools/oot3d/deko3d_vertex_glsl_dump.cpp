// Generates the vertex GLSL the deko3d backend would ask for, straight from a
// title's SHBIN files (no capture): each vertex DVLE becomes the packet a draw
// of it would have (output map from the DVLE output table, as
// tools/renderer/tev_program/build_vertex_artifacts.cpp does), then the same
// GenerateOot3dPicaVertexShader + TranslatePicaShaderToOpenGl43 the backend
// runs. Writes <dir>/vert_<key>.glsl (the pack key) for
// tools/oot3d/deko3d_shader_pack.py --glsl and prints one line per DVLE.
// Geometry DVLEs are listed, not translated (no geometry stage in deko3d).
//
//   oot3d_deko3d_vertex_glsl_dump <diretório> <arquivo.shbin> [...]
#include "fast/backends/gfx_deko3d_shader_pack.h"
#include "fast/renderer3ds/pica_gl_shader_dialect.h"
#include "oot3d_native_pica_shader_gen.h"
#include "oot3d_native_shbin.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "uso: %s <diretório> <arquivo.shbin> [...]\n", argv[0]);
        return 2;
    }
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    int written = 0, failed = 0, geometry = 0;
    for (int arg = 2; arg < argc; ++arg) {
        const std::filesystem::path path(argv[arg]);
        std::ifstream input(path, std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        const std::string name = path.filename().string();
        Oot3dNativeGame::Oot3dShbinEntries entries;
        std::string error;
        if (!input || !Oot3dNativeGame::ReadOot3dShbinEntries(bytes, entries, &error)) {
            std::printf("%s erro %s\n", name.c_str(), input ? error.c_str() : "não abriu");
            ++failed;
            continue;
        }
        for (const size_t dvle : entries.Geometry) {
            std::printf("%s#%zu geometria (sem estágio de geometria no deko3d)\n", name.c_str(), dvle);
            ++geometry;
        }
        for (const auto& entry : entries.Vertex) {
            Oot3dNativeGame::Oot3dPicaGeneratedVertexShader generated;
            std::string translated;
            if (!Oot3dNativeGame::GenerateOot3dPicaVertexShader(*entry.Packet, entry.State, generated, &error) ||
                !Fast::Renderer3ds::TranslatePicaShaderToOpenGl43(generated.Source, translated, &error)) {
                std::printf("%s#%zu erro %s\n", name.c_str(), entry.Dvle, error.c_str());
                ++failed;
                continue;
            }
            // On the console the program memory past the SHBIN holds whatever
            // was uploaded before; the text must not depend on it (same check
            // as tools/renderer/tev_program/export_vertex_programs.cpp).
            auto tail = std::make_unique<Oot3dNativeGame::Oot3dPicaDrawPacket>(*entry.Packet);
            auto& shader = tail->VertexShader;
            for (size_t i = shader.ProgramWordCount; i < shader.Program.size(); ++i) shader.Program[i] = 0x88000000U;
            for (size_t i = shader.SwizzleWordCount; i < shader.Swizzles.size(); ++i) shader.Swizzles[i] = 0xFFFFFFFFU;
            Oot3dNativeGame::Oot3dPicaGeneratedVertexShader withTail;
            if (!Oot3dNativeGame::GenerateOot3dPicaVertexShader(*tail, entry.State, withTail, &error) ||
                withTail.Source != generated.Source) {
                std::printf("%s#%zu erro o texto depende de dados fora do SHBIN\n", name.c_str(), entry.Dvle);
                ++failed;
                continue;
            }
            const uint64_t key = Fast::Deko3d::ShaderTextKey(Fast::Deko3d::ShaderStage::Vertex, translated);
            char file[32];
            std::snprintf(file, sizeof(file), "vert_%016llx", static_cast<unsigned long long>(key));
            std::ofstream(directory / (std::string(file) + ".glsl"), std::ios::binary | std::ios::trunc) << translated;
            std::printf("%s#%zu %s\n", name.c_str(), entry.Dvle, file);
            ++written;
        }
    }
    std::fprintf(stderr, "%d vertex gerados, %d geometria fora, %d com erro\n", written, geometry, failed);
    return failed == 0 && written > 0 ? 0 : 1;
}
