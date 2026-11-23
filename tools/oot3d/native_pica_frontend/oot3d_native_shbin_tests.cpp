// SHBIN -> vertex packets, with hand-made DVLB bytes (no title file).
#include "oot3d_native_pica_shader_gen.h"
#include "oot3d_native_shbin.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace Oot3dNativeGame;

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "oot3d_native_shbin_tests: " << message << '\n';
        ++failures;
    }
}

struct Output {
    uint16_t Kind;
    uint16_t Register;
    uint32_t Mask;
};

struct Dvle {
    uint8_t Stage;
    uint32_t Entry;
    std::vector<Output> Outputs;
};

void Put(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    if (bytes.size() < at + 4) bytes.resize(at + 4);
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = uint8_t(value >> (8 * i));
}

// DVLB: magic, count, DVLE offsets; DVLP: magic, version, code offset/count,
// swizzle offset/count (8 bytes each); DVLE: magic, stage << 16, entry, ...,
// output table offset (+40) and count (+44), relative to the DVLE.
std::vector<uint8_t> Shbin(const std::vector<uint32_t>& code, const std::vector<uint32_t>& swizzles,
                           const std::vector<Dvle>& dvles) {
    std::vector<uint8_t> bytes;
    const size_t dvlp = 8 + 4 * dvles.size();
    Put(bytes, 0, 0x424C5644U);
    Put(bytes, 4, uint32_t(dvles.size()));
    Put(bytes, dvlp, 0x504C5644U);
    Put(bytes, dvlp + 8, 0x28U);
    Put(bytes, dvlp + 12, uint32_t(code.size()));
    const size_t swizzleOffset = 0x28U + 4 * code.size();
    Put(bytes, dvlp + 16, uint32_t(swizzleOffset));
    Put(bytes, dvlp + 20, uint32_t(swizzles.size()));
    for (size_t i = 0; i < code.size(); ++i) Put(bytes, dvlp + 0x28U + 4 * i, code[i]);
    for (size_t i = 0; i < swizzles.size(); ++i) {
        Put(bytes, dvlp + swizzleOffset + 8 * i, swizzles[i]);
        Put(bytes, dvlp + swizzleOffset + 8 * i + 4, 0U);
    }
    for (size_t index = 0; index < dvles.size(); ++index) {
        const size_t d = (bytes.size() + 15U) & ~size_t(15U);
        Put(bytes, 8 + 4 * index, uint32_t(d));
        Put(bytes, d, 0x454C5644U);
        Put(bytes, d + 4, uint32_t(dvles[index].Stage) << 16);
        Put(bytes, d + 8, dvles[index].Entry);
        Put(bytes, d + 40, 0x40U);
        Put(bytes, d + 44, uint32_t(dvles[index].Outputs.size()));
        for (size_t o = 0; o < dvles[index].Outputs.size(); ++o) {
            const auto& output = dvles[index].Outputs[o];
            Put(bytes, d + 0x40U + 8 * o, uint32_t(output.Kind) | uint32_t(output.Register) << 16);
            Put(bytes, d + 0x40U + 8 * o + 4, output.Mask);
        }
    }
    return bytes;
}

// mov o0, v0; mov o1, v1; end (operand descriptor 0: xyzw, identity swizzles).
const std::vector<uint32_t> kCode{0x4C000000U, 0x4C201000U, 0x88000000U};
const std::vector<uint32_t> kSwizzles{0x0D86C36FU};

void VertexEntryBecomesTheDrawInterface() {
    const auto bytes = Shbin(kCode, kSwizzles, {{0, 0, {{0, 0, 15}, {2, 1, 15}, {3, 2, 3}}}});
    Oot3dShbinEntries entries;
    std::string error;
    Expect(ReadOot3dShbinEntries(bytes, entries, &error), "vertex-only SHBIN is rejected");
    Expect(entries.Vertex.size() == 1 && entries.Geometry.empty(), "vertex-only SHBIN entry count");
    if (entries.Vertex.size() != 1) return;
    const auto& entry = entries.Vertex[0];
    const auto& registers = entry.Packet->Registers;
    Expect(entry.Dvle == 0, "DVLE index");
    Expect(entry.Packet->VertexShader.ProgramWordCount == 3 && entry.Packet->VertexShader.Program[1] == 0x4C201000U,
           "program words");
    Expect(entry.Packet->VertexShader.SwizzleWordCount == 1 && entry.Packet->VertexShader.Swizzles[0] == 0x0D86C36FU,
           "swizzle words (low word of each 8-byte record)");
    Expect(registers[0x4F] == 3, "output count register");
    Expect(registers[0x50] == 0x03020100U, "o0 = position");
    Expect(registers[0x51] == 0x0B0A0908U, "o1 = color");
    Expect(registers[0x52] == 0x1F1F0D0CU, "o2.xy = texcoord0");
    Expect(registers[0x2BD] == 7U && entry.State.ShaderInterface.OutputMask == 7U, "output mask");

    Oot3dPicaGeneratedVertexShader shader;
    Expect(GenerateOot3dPicaVertexShader(*entry.Packet, entry.State, shader, &error), "vertex source generation");
    Expect(shader.Source.find("vec4 pica_position = vec4(pica_output0.x, pica_output0.y, pica_output0.z, "
                              "pica_output0.w);") != std::string::npos,
           "position read from o0");
    Expect(shader.Source.find("pica_texcoord0 = vec2(pica_output2.x, pica_output2.y);") != std::string::npos,
           "texcoord0 read from o2");
}

void GeometryEntryIsListedNotTranslated() {
    const auto bytes = Shbin(kCode, kSwizzles, {{0, 0, {{0, 0, 15}}}, {1, 1, {{0, 0, 15}, {9, 1, 15}}}});
    Oot3dShbinEntries entries;
    std::string error;
    Expect(ReadOot3dShbinEntries(bytes, entries, &error), "mixed-stage SHBIN is rejected");
    Expect(entries.Vertex.size() == 1 && entries.Geometry.size() == 1 && entries.Geometry[0] == 1,
           "mixed-stage SHBIN keeps the vertex DVLE and lists the geometry one");
}

void EachEntryHasItsOwnInterface() {
    const auto bytes = Shbin(kCode, kSwizzles, {{0, 0, {{0, 0, 15}, {2, 1, 15}}}, {0, 1, {{0, 0, 15}}}});
    Oot3dShbinEntries entries;
    Expect(ReadOot3dShbinEntries(bytes, entries), "two vertex DVLEs are rejected");
    Expect(entries.Vertex.size() == 2, "two vertex DVLEs");
    if (entries.Vertex.size() != 2) return;
    Expect(entries.Vertex[1].State.ShaderInterface.VertexMainOffset == 1, "second entry point");
    Expect(entries.Vertex[1].Packet->Registers[0x4F] == 1 && entries.Vertex[1].Packet->Registers[0x51] == 0,
           "second DVLE does not inherit the first one's outputs");
}

void InvalidInputsFail() {
    Oot3dShbinEntries entries;
    std::string error;
    Expect(!ReadOot3dShbinEntries(std::vector<uint8_t>{0x44, 0x56}, entries, &error) && !error.empty(), "truncated");
    auto bytes = Shbin(kCode, kSwizzles, {{0, 0, {{0, 0, 15}}}});
    bytes[0] = 0;
    Expect(!ReadOot3dShbinEntries(bytes, entries, &error), "not DVLB");
    Expect(!ReadOot3dShbinEntries(Shbin(kCode, kSwizzles, {{0, 7, {{0, 0, 15}}}}), entries, &error),
           "entry point past the program");
    Expect(!ReadOot3dShbinEntries(Shbin(kCode, kSwizzles, {{0, 0, {{0, 0, 15}, {2, 0, 15}}}}), entries, &error),
           "overlapping output semantics");
    Expect(!ReadOot3dShbinEntries(Shbin(kCode, kSwizzles, {{0, 0, {{7, 0, 1}}}}), entries, &error),
           "reserved output semantic");
    Expect(!ReadOot3dShbinEntries(Shbin(kCode, kSwizzles, {{2, 0, {{0, 0, 15}}}}), entries, &error),
           "unknown DVLE stage");
}

} // namespace

int main() {
    VertexEntryBecomesTheDrawInterface();
    GeometryEntryIsListedNotTranslated();
    EachEntryHasItsOwnInterface();
    InvalidInputsFail();
    if (failures != 0) {
        std::cerr << failures << " failures\n";
        return 1;
    }
    std::cout << "oot3d_native_shbin_tests: ok\n";
    return 0;
}
