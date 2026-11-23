#include "oot3d_native_shbin.h"

#include <iterator>
#include <stdexcept>

namespace Oot3dNativeGame {

namespace {

void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

} // namespace

bool ReadOot3dShbinEntries(std::span<const uint8_t> bytes, Oot3dShbinEntries& entries, std::string* error) try {
    entries = {};
    const auto word = [&](size_t p) {
        Check(p <= bytes.size() && bytes.size() - p >= 4, "truncated SHBIN");
        return uint32_t(bytes[p]) | uint32_t(bytes[p + 1]) << 8 | uint32_t(bytes[p + 2]) << 16 |
               uint32_t(bytes[p + 3]) << 24;
    };
    Check(word(0) == 0x424C5644U, "not DVLB");
    const size_t count = word(4);
    Check(count && count <= (bytes.size() - 8) / 4, "invalid DVLE table");
    const size_t p = 8 + count * 4;
    Check(word(p) == 0x504C5644U, "not DVLP");
    Oot3dPicaShaderState program;
    program.ProgramWordCount = word(p + 12);
    program.SwizzleWordCount = word(p + 20);
    Check(program.ProgramWordCount != 0 && program.ProgramWordCount <= program.Program.size() &&
              program.SwizzleWordCount <= program.Swizzles.size(),
          "invalid program capacity");
    for (size_t i = 0; i < program.ProgramWordCount; ++i) program.Program[i] = word(p + word(p + 8) + 4 * i);
    // Each swizzle record is 8 bytes; the PICA uses the low word.
    for (size_t i = 0; i < program.SwizzleWordCount; ++i) program.Swizzles[i] = word(p + word(p + 16) + 8 * i);
    for (size_t index = 0; index < count; ++index) {
        const size_t d = word(8 + 4 * index);
        Check(word(d) == 0x454C5644U, "not a DVLE");
        const unsigned stage = (word(d + 4) >> 16) & 255U;
        Check(stage <= 1, "unknown DVLE stage");
        if (stage == 1) {
            entries.Geometry.push_back(index);
            continue;
        }
        Oot3dShbinVertexEntry entry;
        entry.Dvle = index;
        entry.Packet = std::make_unique<Oot3dPicaDrawPacket>();
        entry.Packet->VertexShader = program;
        auto& state = entry.State;
        const uint32_t mainOffset = word(d + 8);
        Check(mainOffset < program.ProgramWordCount, "invalid entry point");
        state.ShaderInterface.VertexMainOffset = static_cast<uint16_t>(mainOffset);
        std::array<uint32_t, 16> maps;
        maps.fill(0x1F1F1F1FU);
        const size_t offset = d + word(d + 40), outputs = word(d + 44);
        Check(outputs <= bytes.size() / 8, "invalid output table extent");
        for (size_t i = 0; i < outputs; ++i) {
            const auto descriptor = word(offset + 8 * i);
            const unsigned kind = descriptor & 0xFFFFU, reg = descriptor >> 16;
            const auto mask = word(offset + 8 * i + 4);
            Check(reg < 16 && mask && !(mask & ~15U), "invalid output register or mask");
            // Native output semantic bases (position, quaternion, color,
            // UV0, UV0.w, UV1, UV2, reserved, view).
            constexpr unsigned bases[]{0, 4, 8, 12, 16, 14, 22, 31, 18};
            constexpr unsigned lengths[]{4, 4, 4, 2, 1, 2, 2, 0, 4};
            Check(kind < std::size(bases) && lengths[kind], "unsupported SHBIN output semantic");
            unsigned component = 0;
            for (unsigned lane = 0; lane < 4; ++lane) {
                if (!(mask & (1U << lane))) continue;
                Check(component < lengths[kind], "output semantic exceeds native width");
                const auto shift = lane * 8;
                Check(((maps[reg] >> shift) & 31) == 31, "overlapping output semantics");
                maps[reg] = (maps[reg] & ~(255U << shift)) | ((bases[kind] + component++) << shift);
            }
            state.ShaderInterface.OutputMask |= 1U << reg;
        }
        auto& registers = entry.Packet->Registers;
        size_t packed = 0;
        for (size_t reg = 0; reg < maps.size(); ++reg) {
            if (!(state.ShaderInterface.OutputMask & (1U << reg))) continue;
            Check(packed < 7, "output map exceeds PICA interface");
            registers[0x50 + packed++] = maps[reg];
        }
        registers[0x4F] = static_cast<uint32_t>(packed);
        registers[0x2BD] = state.ShaderInterface.OutputMask;
        registers[0x2BA] = state.ShaderInterface.VertexMainOffset;
        entries.Vertex.push_back(std::move(entry));
    }
    return true;
} catch (const std::exception& e) {
    entries = {};
    if (error != nullptr) *error = e.what();
    return false;
}

} // namespace Oot3dNativeGame
