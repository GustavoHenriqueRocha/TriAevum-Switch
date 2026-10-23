#include "fast/backends/gfx_opengl_pica_residency.h"
#include "fast/backends/gfx_opengl_pica_uniform_trim.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace Fast;
using PicaGlUniformTrim::FragmentUsedBytes;
using PicaGlUniformTrim::kAll;
using PicaGlUniformTrim::VertexUsedBytes;

// Cabecalho igual ao do gerador (oot3d_native_pica_shader_gen.cpp) depois
// de TranslatePicaShaderToOpenGl43.
const std::string kVertexHead =
    "#version 430\n"
    "layout(binding=0, std140) uniform PicaVertexUniforms {\n"
    "    uint b;\n    int flip_viewport;\n    uvec4 i[4];\n    vec4 f[96];\n"
    "} uniforms;\n";
// O auxiliar do decompilador (enderecamento relativo a0/aL).
const std::string kOffsetHelper =
    "vec4 get_offset_register(int base_index, int offset) {\n"
    "    int fixed_offset = offset >= -128 && offset <= 127 ? offset : 0;\n"
    "    uint index = uint((base_index + fixed_offset) & 0x7F);\n"
    "    return index < 96u ? uniforms.f[index] : vec4(1.0);\n"
    "}\n";
const std::string kVertexMain =
    "void main() {\n    if (uniforms.flip_viewport != 0) gl_Position.y = 1.0;\n}\n";

constexpr size_t kVertexBlock = Oot3d::kPicaPackedVertexUniformSize;      // 1616
constexpr size_t kFragmentBlock = Oot3d::kPicaPackedFragmentUniformSize;  // 6112

TEST(PicaGlUniformTrim, VertexLiteralRegistersGiveTheHighestOneRead) {
    const std::string source = kVertexHead +
                               "void exec_shader() { reg_tmp0 = uniforms.f[3] + uniforms.f [ 12 ].xyzw; }\n" +
                               kVertexMain;
    EXPECT_EQ(VertexUsedBytes(source), 80U + 13U * 16U);
}

TEST(PicaGlUniformTrim, VertexHeaderOnly) {
    EXPECT_EQ(VertexUsedBytes(kVertexHead + kVertexMain), 8U);
    EXPECT_EQ(VertexUsedBytes(kVertexHead + "bool x = (uniforms.b & 1u) != 0u;\n"), 4U);
    EXPECT_EQ(VertexUsedBytes(kVertexHead + "uvec4 l = uniforms.i[2];\n"), 16U + 3U * 16U);
    // i[] com indice nao literal: o array inteiro, nao o bloco.
    EXPECT_EQ(VertexUsedBytes(kVertexHead + "uvec4 l = uniforms.i[n];\n"), 80U);
}

TEST(PicaGlUniformTrim, RelativeAddressingCalledReadsTheWholeFloatArray) {
    const std::string source =
        kVertexHead + kOffsetHelper +
        "void exec_shader() { reg_tmp3.x = dot(get_offset_register(20, address_registers.x), vec4(1.0)); "
        "reg_tmp4 = uniforms.f[7]; }\n" +
        kVertexMain;
    EXPECT_EQ(VertexUsedBytes(source), kVertexBlock);
}

TEST(PicaGlUniformTrim, RelativeAddressingHelperNeverCalledDoesNotCount) {
    const std::string source = kVertexHead + kOffsetHelper +
                               "void exec_shader() { reg_tmp4 = uniforms.f[7]; }\n" + kVertexMain;
    EXPECT_EQ(VertexUsedBytes(source), 80U + 8U * 16U);
}

TEST(PicaGlUniformTrim, DirectNonLiteralIndexReadsTheWholeFloatArray) {
    EXPECT_EQ(VertexUsedBytes(kVertexHead + "vec4 v = uniforms.f[address_registers.x + 4];\n"),
              kVertexBlock);
    EXPECT_EQ(VertexUsedBytes(kVertexHead + "vec4 v = uniforms.f[0x10];\n"), kVertexBlock);
    // Array inteiro sem indice (copia/atribuicao).
    EXPECT_EQ(VertexUsedBytes(kVertexHead + "vec4 c[96] = uniforms.f;\n"), kVertexBlock);
}

TEST(PicaGlUniformTrim, AnythingUnknownIsTheWholeBlock) {
    EXPECT_EQ(VertexUsedBytes(kVertexHead + "vec4 v = uniforms.g[1];\n"), kAll);
    EXPECT_EQ(VertexUsedBytes(kVertexHead + "#define U uniforms\nvec4 v = U.f[1];\n"), kAll);
    EXPECT_EQ(VertexUsedBytes(kVertexHead + "vec4 v = uniforms.f[96];\n"), kAll);
    EXPECT_EQ(FragmentUsedBytes("float x = fragment_uniforms.not_a_member;\n"), kAll);
    // Um auxiliar chamado com recursao/sem corpo fecha: conta tudo.
    EXPECT_EQ(VertexUsedBytes(kVertexHead + "vec4 get_offset_register(int a, int b) { return uniforms.f[a];\n"),
              kVertexBlock);
}

TEST(PicaGlUniformTrim, OtherBlocksAndIdentifiersDoNotMatch) {
    // "fragment_uniforms" e "PicaVertexUniforms" nao sao o token "uniforms".
    EXPECT_EQ(VertexUsedBytes(kVertexHead + "float a = fragment_uniforms.depth_scale;\n" + kVertexMain), 8U);
    EXPECT_EQ(FragmentUsedBytes("vec4 v = uniforms.f[90];\n"), 0U);
}

const std::string kFragmentHead =
    "layout(binding=4, std140) uniform PicaFragmentUniforms {\n"
    "    vec4 tev_constants[6];\n    vec4 combiner_buffer_color;\n"
    "    vec4 texture_lod_bias;\n"
    "} fragment_uniforms;\n";

TEST(PicaGlUniformTrim, FragmentMembersGiveTheirEnd) {
    const std::string base =
        kFragmentHead +
        "vec4 s0(vec2 c) { return textureLod(t, c, fragment_uniforms.texture_lod_bias[0]); }\n"
        "void main() { vec4 k = fragment_uniforms.tev_constants[2] + fragment_uniforms.combiner_buffer_color; }\n";
    EXPECT_EQ(FragmentUsedBytes(base), 1184U);
    EXPECT_EQ(FragmentUsedBytes(kFragmentHead + "float d = fragment_uniforms.depth_scale;"), 120U);
    // Indice nao literal num array: o membro inteiro.
    EXPECT_EQ(FragmentUsedBytes(kFragmentHead + "vec4 f = fragment_uniforms.fog_lut[idx];"), 1168U);
    EXPECT_EQ(FragmentUsedBytes(kFragmentHead + "vec4 f = fragment_uniforms.tev_constants[i];"), 96U);
    EXPECT_EQ(FragmentUsedBytes(base + "vec4 l = fragment_uniforms.lighting_attenuation[i];\n"
                                       "vec4 g = fragment_uniforms.lighting_global_ambient;\n"),
              2096U);
    EXPECT_EQ(FragmentUsedBytes(base + "float b = fragment_uniforms.shadow_bias_linear;"), 2112U);
    EXPECT_EQ(FragmentUsedBytes(base + "uint o = fragment_uniforms.tev_program.ops[1].x;"), 2224U);
    EXPECT_EQ(FragmentUsedBytes(base + "uvec4 c = fragment_uniforms.fragment_control;"), 2496U);
    EXPECT_EQ(FragmentUsedBytes(base + "f(fragment_uniforms.proctex_program);"), kFragmentBlock);
}

TEST(PicaGlUniformTrim, LayoutTableMatchesThePackedBlocks) {
    size_t end = 0U;
    for (const auto& member : PicaGlUniformTrim::detail::kVertexMembers) {
        EXPECT_GE(member.Offset, end) << member.Name;
        end = member.End();
    }
    EXPECT_EQ(end, kVertexBlock);
    end = 0U;
    for (const auto& member : PicaGlUniformTrim::detail::kFragmentMembers) {
        EXPECT_GE(member.Offset, end) << member.Name;
        end = member.End();
    }
    EXPECT_EQ(end, kFragmentBlock);
}

TEST(PicaGlUniformTrim, ProgramUsageUnitesBothStages) {
    const std::string vertex = kVertexHead + "vec4 v = uniforms.f[2];\n" + kVertexMain;
    const std::string fragment = kFragmentHead + "vec4 l = fragment_uniforms.texture_lod_bias;\n";
    auto usage = PicaGlUniformTrim::ProgramUsedBytes(vertex, fragment);
    EXPECT_EQ(usage.Vertex, 80U + 3U * 16U);
    EXPECT_EQ(usage.Fragment, 1184U);
    // Um estagio que lesse o bloco do outro alarga a faixa (ou o bloco todo).
    usage = PicaGlUniformTrim::ProgramUsedBytes(vertex, fragment + "vec4 x = uniforms.f[40];\n");
    EXPECT_EQ(usage.Vertex, 80U + 41U * 16U);
    usage = PicaGlUniformTrim::ProgramUsedBytes(vertex + "float d = fragment_uniforms.w_buffering;\n" +
                                                    "vec4 g = fragment_uniforms.lighting_global_ambient;\n",
                                                fragment);
    EXPECT_EQ(usage.Fragment, 2096U);
    usage = PicaGlUniformTrim::ProgramUsedBytes(vertex, fragment + "vec4 x = uniforms.zz;\n");
    EXPECT_EQ(usage.Vertex, kAll);
}

TEST(PicaGlUniformTrim, SendBytes) {
    using PicaGlUniformTrim::SendBytes;
    EXPECT_EQ(SendBytes(false, 1184U, kFragmentBlock), kFragmentBlock);
    EXPECT_EQ(SendBytes(true, kAll, kFragmentBlock), kFragmentBlock);
    EXPECT_EQ(SendBytes(true, 1184U, kFragmentBlock), 1184U);
    EXPECT_EQ(SendBytes(true, 4U, kVertexBlock), 16U);
    EXPECT_EQ(SendBytes(true, 9000U, kFragmentBlock), kFragmentBlock);
}

// Simulacao do envio de uniforms de uma sequencia de draws pelo anel real
// (StreamRing), caminho antigo (bloco inteiro, dedup do bloco inteiro) e
// novo (prefixo lido, dedup do prefixo). A cada geracao o armazenamento
// comeca com lixo (orfanado). No fim, para cada draw, os bytes que o
// programa le na faixa ligada sao os do bloco e iguais aos do caminho
// antigo, e a faixa ligada tem o mesmo tamanho.
struct Bound {
    uint64_t Generation = 0U;
    uint64_t Offset = 0U;
    size_t Size = 0U;
};
struct SimulatedRing {
    PicaGlResidency::StreamRing Ring{64U * 1024U};
    std::map<uint64_t, std::vector<uint8_t>> Storage;
    PicaGlUniformTrim::LastUpload Last;
    size_t SentBytes = 0U;
    size_t Reused = 0U;
    std::mt19937 Garbage{7U};

    Bound Draw(bool trim, std::span<const uint8_t> block, size_t used) {
        const uint64_t alignment = 256U;
        (void)Ring.Reserve(PicaGlResidency::StreamRing::Span(block.size(), alignment));
        auto& storage = Storage[Ring.Generation()];
        if (storage.empty()) {
            storage.resize(Ring.Capacity());
            for (auto& byte : storage) byte = static_cast<uint8_t>(Garbage());
        }
        const auto plan = PicaGlUniformTrim::PlanUpload(trim, true, Last, Ring.Generation(), block, used);
        if (plan.Reuse) {
            ++Reused;
            return {Ring.Generation(), Last.Offset, block.size()};
        }
        const auto allocation = Ring.Allocate(block.size(), alignment);
        EXPECT_TRUE(allocation.Valid);
        std::copy_n(block.begin(), plan.Send, storage.begin() + static_cast<std::ptrdiff_t>(allocation.Offset));
        SentBytes += plan.Send;
        PicaGlUniformTrim::Remember(Last, block, plan.Send, Ring.Generation(), allocation.Offset);
        return {Ring.Generation(), allocation.Offset, block.size()};
    }
};

TEST(PicaGlUniformTrim, TrimmedUploadsBindTheSameBytesTheProgramReads) {
    std::mt19937 random(1234U);
    // Programas: prefixos lidos como os do corpus do console (1184, 2096) e
    // um que le tudo.
    const std::vector<size_t> programs{1184U, 2096U, kAll, 120U};
    // Poucos blocos distintos (repeticao como num quadro real), mas que
    // diferem tambem so na cauda nao lida.
    std::vector<std::vector<uint8_t>> blocks(5, std::vector<uint8_t>(kFragmentBlock));
    for (auto& block : blocks) {
        for (auto& byte : block) byte = static_cast<uint8_t>(random() & 3U);
    }
    blocks[1] = blocks[0];
    blocks[1][3000] ^= 0xFFU; // so a cauda muda
    SimulatedRing before, after;
    struct Record {
        size_t Block, Used;
        Bound Old, New;
    };
    std::vector<Record> records;
    for (int draw = 0; draw < 4000; ++draw) {
        const size_t block = random() % blocks.size();
        const size_t used = programs[random() % programs.size()];
        records.push_back({block, used, before.Draw(false, blocks[block], used),
                           after.Draw(true, blocks[block], used)});
    }
    for (const auto& record : records) {
        const auto& block = blocks[record.Block];
        const size_t read = record.Used == kAll ? block.size() : record.Used;
        ASSERT_EQ(record.New.Size, record.Old.Size);
        const auto& oldStorage = before.Storage[record.Old.Generation];
        const auto& newStorage = after.Storage[record.New.Generation];
        for (size_t index = 0; index < read; ++index) {
            ASSERT_EQ(oldStorage[record.Old.Offset + index], block[index]);
            ASSERT_EQ(newStorage[record.New.Offset + index], block[index]) << index;
        }
    }
    // Menos bytes copiados. O reuso pode ate cair (o anterior copiou so o
    // prefixo de um programa que le menos e este le mais), sem afetar a
    // imagem.
    EXPECT_LT(after.SentBytes, before.SentBytes);
    EXPECT_GT(after.Reused, 0U);
}

TEST(PicaGlUniformTrim, TrimOffIsTheOldPath) {
    std::vector<uint8_t> a(kVertexBlock, 1U), b(kVertexBlock, 1U);
    b.back() = 2U;
    PicaGlUniformTrim::LastUpload last;
    PicaGlUniformTrim::Remember(last, a, a.size(), 1U, 512U);
    EXPECT_TRUE(PicaGlUniformTrim::PlanUpload(false, true, last, 1U, a, 8U).Reuse);
    EXPECT_FALSE(PicaGlUniformTrim::PlanUpload(false, true, last, 1U, b, 8U).Reuse);
    EXPECT_EQ(PicaGlUniformTrim::PlanUpload(false, true, last, 1U, b, 8U).Send, kVertexBlock);
    // Com trim, a cauda nao lida nao impede o reuso.
    EXPECT_TRUE(PicaGlUniformTrim::PlanUpload(true, true, last, 1U, b, 8U).Reuse);
    // Outra geracao, outro tamanho ou sem dedup: envia.
    EXPECT_FALSE(PicaGlUniformTrim::PlanUpload(true, true, last, 2U, a, 8U).Reuse);
    EXPECT_FALSE(PicaGlUniformTrim::PlanUpload(true, false, last, 1U, a, 8U).Reuse);
    std::vector<uint8_t> shorter(kVertexBlock - 16U, 1U);
    EXPECT_FALSE(PicaGlUniformTrim::PlanUpload(true, true, last, 1U, shorter, 8U).Reuse);
    // O anterior copiou menos do que este programa le: envia.
    PicaGlUniformTrim::Remember(last, a, 16U, 1U, 512U);
    EXPECT_FALSE(PicaGlUniformTrim::PlanUpload(true, true, last, 1U, a, 80U).Reuse);
    EXPECT_TRUE(PicaGlUniformTrim::PlanUpload(true, true, last, 1U, a, 8U).Reuse);
}

} // namespace
