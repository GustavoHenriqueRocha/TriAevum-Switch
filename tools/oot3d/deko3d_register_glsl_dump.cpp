// Generates the fragment GLSL the deko3d backend would ask for, from PICA
// register values written offline (for example from CMB materials by
// tools/oot3d/cmb_tev_registers.py). Each input line is
//   <id> <reg>=<value> [<reg>=<value> ...]
// with hex registers and values; the registers are applied over a minimal
// draw (one triangle, texture 0 on). Writes <dir>/<id>.glsl (GLSL 4.3, the
// text the pack is keyed by) and prints "<id> frag_<key>" per line.
//
//   oot3d_deko3d_register_glsl_dump <entrada.txt> <diretório>
#include "fast/backends/gfx_deko3d_shader_pack.h"
#include "fast/renderer3ds/pica_gl_shader_dialect.h"
#include "oot3d_native_pica_fragment_shader_gen.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

Oot3dNativeGame::Oot3dPicaDrawPacket BasePacket() {
    Oot3dNativeGame::Oot3dPicaDrawPacket packet;
    packet.VertexShader.ProgramWordCount = 1;
    packet.Registers[0x04FU] = 1U;
    packet.Registers[0x050U] = 0x03020100U;
    packet.Registers[0x080U] = 1U;
    packet.Registers[0x103U] = 0x00000F01U;
    for (const uint32_t stage : {0x0C0U, 0x0C8U, 0x0D0U, 0x0D8U, 0x0F0U, 0x0F8U}) {
        packet.Registers[stage + 0U] = 0x000F000FU;  // PREVIOUS, REPLACE
        packet.Registers[stage + 3U] = 0xFFFFFFFFU;
    }
    return packet;
}

Oot3dNativeGame::Oot3dPicaDecodedDrawState BaseState() {
    Oot3dNativeGame::Oot3dPicaDecodedDrawState state;
    state.ShaderInterface.OutputMask = 1U;
    state.VertexInput.AttributeCount = 1U;
    state.VertexInput.Attributes[0].Format = Oot3dNativeGame::Oot3dPicaVertexFormat::Float;
    state.VertexInput.Attributes[0].ComponentCount = 3U;
    state.VertexInput.Loaders[0].ByteStride = 12U;
    state.VertexInput.Loaders[0].ComponentCount = 1U;
    state.VertexInput.VertexCount = 3U;
    state.Framebuffer.Width = 240U;
    state.Framebuffer.Height = 400U;
    state.OutputMerger.ColorWriteMask = 0x0FU;
    return state;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "uso: %s <entrada.txt> <diretório>\n", argv[0]);
        return 2;
    }
    std::ifstream input(argv[1]);
    const std::filesystem::path directory(argv[2]);
    std::filesystem::create_directories(directory);
    int written = 0, failed = 0;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string id, assignment;
        if (!(fields >> id) || id[0] == '#') continue;
        auto packet = BasePacket();
        while (fields >> assignment) {
            const auto equals = assignment.find('=');
            const unsigned long reg = std::stoul(assignment.substr(0, equals), nullptr, 16);
            if (equals == std::string::npos || reg >= packet.Registers.size()) {
                std::fprintf(stderr, "%s: registrador inválido %s\n", id.c_str(), assignment.c_str());
                return 1;
            }
            packet.Registers[reg] = static_cast<uint32_t>(std::stoul(assignment.substr(equals + 1), nullptr, 16));
        }
        auto state = BaseState();
        for (unsigned unit = 0; unit < 3U; ++unit) {
            state.Textures[unit].Enabled = (packet.Registers[0x080U] >> unit & 1U) != 0U;  // 2D
        }
        state.Texture2UsesCoordinate1 = (packet.Registers[0x080U] >> 13 & 1U) != 0U;
        Oot3dNativeGame::Oot3dPicaGeneratedFragmentShader generated;
        std::string error, translated;
        if (!Oot3dNativeGame::GenerateOot3dPicaFragmentShader(packet, state, generated, &error,
                                                              Oot3dNativeGame::Oot3dPicaShaderBuildPurpose::OfflineSource) ||
            !Fast::Renderer3ds::TranslatePicaShaderToOpenGl43(generated.Source, translated, &error)) {
            std::printf("%s erro %s\n", id.c_str(), error.c_str());
            ++failed;
            continue;
        }
        const uint64_t key = Fast::Deko3d::ShaderTextKey(Fast::Deko3d::ShaderStage::Fragment, translated);
        std::ofstream(directory / (id + ".glsl"), std::ios::binary | std::ios::trunc) << translated;
        std::printf("%s frag_%016llx\n", id.c_str(), static_cast<unsigned long long>(key));
        ++written;
    }
    std::fprintf(stderr, "%d gerados, %d com erro\n", written, failed);
    return written > 0 ? 0 : 1;
}
