#pragma once

// Lean uniforms for the PICA path (config/gl_uniform_trim, on by
// default; a file with "0" turns it off; desktop: TRIAEVUM_GL_UNIFORM_TRIM).
//
// Each draw sends two std140 blocks to the uniform ring: the vertex one
// (1616 bytes: b, flip_viewport, i[4], f[96]) and the fragment one (6112 bytes,
// pica_uniform_layout.h). Many programs only read the start of the block (a
// fragment shader with no lighting and no parametric program stops at
// texture_lod_bias, byte 1184). At program creation, the final GLSL
// (after TranslatePicaShaderToOpenGl43) is scanned and the prefix
// [0, N) it can read is stored; the draw copies only that prefix to the ring, but reserves
// and binds (glBindBufferRange) the whole block as before, so the GL state is
// the same and the bytes the shader reads are identical to the old path's (the
// rest of the bound range keeps whatever the ring had; nobody reads it).
//
// Conservative: a non-literal index (or an array used whole) counts as the whole
// member; what the scanner doesn't understand (unknown member, the instance
// without ".member", #define, literal index outside the array) counts as the whole
// block. Comments count as references (only overestimates).
// PICA relative addressing (a0/aL) shows up as get_offset_register()
// reading uniforms.f[index]: if called, counts as the whole f[] (the index is
// (base + a) & 0x7F, any register); if only defined and never called, it doesn't
// count.
//
// GL-free and stateless: tested offline
// (tests/gfx_opengl_pica_uniform_trim_tests.cpp).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "fast/oot3d/pica_uniform_layout.h"

namespace Fast::PicaGlUniformTrim {

// Reads the whole block (or less could not be proven).
inline constexpr size_t kAll = static_cast<size_t>(-1);

// Member of a std140 block: Count == 0 is a scalar/struct of Stride bytes;
// otherwise an array of Count elements of Stride bytes.
struct Member {
    std::string_view Name;
    size_t Offset = 0U;
    size_t Stride = 0U;
    size_t Count = 0U;

    [[nodiscard]] constexpr size_t End() const noexcept {
        return Offset + Stride * (Count == 0U ? 1U : Count);
    }
};

namespace detail {

constexpr size_t kVec4 = Oot3d::kPicaPackedVec4Bytes;

inline constexpr std::array<Member, 4> kVertexMembers{{
    {"b", Oot3d::kPicaPackedVertexBooleanMaskOffset, 4U, 0U},
    {"flip_viewport", Oot3d::kPicaPackedVertexFlipViewportOffset, 4U, 0U},
    {"i", Oot3d::kPicaPackedVertexIntegerOffset, kVec4, 4U},
    {"f", Oot3d::kPicaPackedVertexFloatOffset, kVec4, 96U},
}};

inline constexpr std::array<Member, 25> kFragmentMembers{{
    {"tev_constants", Oot3d::kPicaPackedFragmentTevConstantsOffset, kVec4, 6U},
    {"combiner_buffer_color", Oot3d::kPicaPackedFragmentCombinerBufferOffset, kVec4, 0U},
    {"alpha_reference", Oot3d::kPicaPackedFragmentAlphaReferenceOffset, 4U, 0U},
    {"depth_scale", Oot3d::kPicaPackedFragmentDepthScaleOffset, 4U, 0U},
    {"depth_offset", Oot3d::kPicaPackedFragmentDepthOffsetOffset, 4U, 0U},
    {"w_buffering", Oot3d::kPicaPackedFragmentWBufferingOffset, 4U, 0U},
    {"fog_color", Oot3d::kPicaPackedFragmentFogColorOffset, kVec4, 0U},
    {"fog_lut", Oot3d::kPicaPackedFragmentFogLutOffset, kVec4, 64U},
    {"texture_lod_bias", Oot3d::kPicaPackedFragmentTextureLodBiasOffset, kVec4, 0U},
    {"lighting_specular0", Oot3d::kPicaPackedFragmentLightingSpecular0Offset, kVec4, 8U},
    {"lighting_specular1", Oot3d::kPicaPackedFragmentLightingSpecular1Offset, kVec4, 8U},
    {"lighting_diffuse", Oot3d::kPicaPackedFragmentLightingDiffuseOffset, kVec4, 8U},
    {"lighting_ambient", Oot3d::kPicaPackedFragmentLightingAmbientOffset, kVec4, 8U},
    {"lighting_position", Oot3d::kPicaPackedFragmentLightingPositionOffset, kVec4, 8U},
    {"lighting_spot_direction", Oot3d::kPicaPackedFragmentLightingSpotDirectionOffset, kVec4, 8U},
    {"lighting_attenuation", Oot3d::kPicaPackedFragmentLightingAttenuationOffset, kVec4, 8U},
    {"lighting_global_ambient", Oot3d::kPicaPackedFragmentLightingGlobalAmbientOffset, kVec4, 0U},
    {"shadow_texture_bias", Oot3d::kPicaPackedFragmentShadowTextureBiasOffset, 4U, 0U},
    {"shadow_orthographic", Oot3d::kPicaPackedFragmentShadowOrthographicOffset, 4U, 0U},
    {"shadow_bias_constant", Oot3d::kPicaPackedFragmentShadowBiasConstantOffset, 4U, 0U},
    {"shadow_bias_linear", Oot3d::kPicaPackedFragmentShadowBiasLinearOffset, 4U, 0U},
    // Parametric program: structs (size = distance to the next member).
    {"tev_program", Oot3d::kPicaPackedFragmentTevProgramOffset,
     Oot3d::kPicaPackedFragmentLightingProgramOffset - Oot3d::kPicaPackedFragmentTevProgramOffset, 0U},
    {"lighting_program", Oot3d::kPicaPackedFragmentLightingProgramOffset,
     Oot3d::kPicaPackedFragmentControlOffset - Oot3d::kPicaPackedFragmentLightingProgramOffset, 0U},
    {"fragment_control", Oot3d::kPicaPackedFragmentControlOffset,
     Oot3d::kPicaPackedFragmentProcTexOffset - Oot3d::kPicaPackedFragmentControlOffset, 0U},
    {"proctex_program", Oot3d::kPicaPackedFragmentProcTexOffset,
     Oot3d::kPicaPackedFragmentUniformSize - Oot3d::kPicaPackedFragmentProcTexOffset, 0U},
}};

constexpr bool IsIdent(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
constexpr bool IsSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}
inline size_t SkipSpace(std::string_view s, size_t at) noexcept {
    while (at < s.size() && IsSpace(s[at])) ++at;
    return at;
}
// Position of `word` as a whole token from `from`, or npos.
inline size_t FindToken(std::string_view s, std::string_view word,
                        size_t from) noexcept {
    while (true) {
        const size_t at = s.find(word, from);
        if (at == std::string_view::npos) return at;
        const bool begins = at == 0U || !IsIdent(s[at - 1U]);
        const size_t after = at + word.size();
        const bool ends = after >= s.size() || !IsIdent(s[after]);
        if (begins && ends) return at;
        from = at + 1U;
    }
}

// Range [Begin, End) of the body of a function defined and never called, or
// empty. The definition is "<type> name(...) {": the token before the name is an
// identifier.
struct Range {
    size_t Begin = 0U;
    size_t End = 0U;
};
inline Range UncalledHelperBody(std::string_view s, std::string_view name) {
    if (name.empty()) return {};
    size_t definition = std::string_view::npos;
    size_t bodyBegin = 0U, bodyEnd = 0U;
    size_t uses = 0U;
    for (size_t at = FindToken(s, name, 0U); at != std::string_view::npos;
         at = FindToken(s, name, at + name.size())) {
        if (definition != std::string_view::npos && at >= bodyBegin &&
            at < bodyEnd) {
            ++uses; // recursion: conservative, counts as a call
            continue;
        }
        size_t before = at;
        while (before > 0U && IsSpace(s[before - 1U])) --before;
        const bool typed = before > 0U && IsIdent(s[before - 1U]);
        size_t open = SkipSpace(s, at + name.size());
        if (definition == std::string_view::npos && typed && open < s.size() &&
            s[open] == '(') {
            const size_t close = s.find(')', open);
            const size_t brace =
                close == std::string_view::npos ? close : SkipSpace(s, close + 1U);
            if (brace != std::string_view::npos && brace < s.size() &&
                s[brace] == '{') {
                size_t depth = 0U;
                size_t cursor = brace;
                for (; cursor < s.size(); ++cursor) {
                    if (s[cursor] == '{') ++depth;
                    if (s[cursor] == '}' && --depth == 0U) break;
                }
                if (cursor >= s.size()) return {}; // body without end: counts everything
                definition = at;
                bodyBegin = brace;
                bodyEnd = cursor + 1U;
                continue;
            }
        }
        ++uses;
    }
    if (definition == std::string_view::npos || uses != 0U) return {};
    return {bodyBegin, bodyEnd};
}

// Decimal literal index "[ 12 ]" (u suffix accepted) from `at` (which
// points to '['). Returns false when it isn't a literal.
inline bool LiteralIndex(std::string_view s, size_t at, size_t* index) {
    size_t cursor = SkipSpace(s, at + 1U);
    size_t value = 0U;
    size_t digits = 0U;
    while (cursor < s.size() && s[cursor] >= '0' && s[cursor] <= '9') {
        value = value * 10U + static_cast<size_t>(s[cursor] - '0');
        if (++digits > 6U) return false;
        ++cursor;
    }
    if (digits == 0U) return false;
    if (cursor < s.size() && (s[cursor] == 'u' || s[cursor] == 'U')) ++cursor;
    cursor = SkipSpace(s, cursor);
    if (cursor >= s.size() || s[cursor] != ']') return false;
    *index = value;
    return true;
}

} // namespace detail

// Bytes [0, N) of the `instance` instance block that `source` can read; kAll
// when it can't be proven. `helper`: function whose body doesn't count if it
// is never called.
inline size_t UsedBytes(std::string_view source, std::string_view instance,
                        std::span<const Member> members,
                        std::string_view helper = {}) {
    using namespace detail;
    const Range skip = UncalledHelperBody(source, helper);
    size_t used = 0U;
    for (size_t at = FindToken(source, instance, 0U);
         at != std::string_view::npos;
         at = FindToken(source, instance, at + instance.size())) {
        if (at >= skip.Begin && at < skip.End) continue;
        size_t cursor = SkipSpace(source, at + instance.size());
        if (cursor < source.size() && source[cursor] == ';') {
            // The "} instance;" declaration (nothing else comes before ';').
            size_t before = at;
            while (before > 0U && IsSpace(source[before - 1U])) --before;
            if (before > 0U && source[before - 1U] == '}') continue;
            return kAll;
        }
        if (cursor >= source.size() || source[cursor] != '.') return kAll;
        cursor = SkipSpace(source, cursor + 1U);
        size_t nameEnd = cursor;
        while (nameEnd < source.size() && IsIdent(source[nameEnd])) ++nameEnd;
        const std::string_view name = source.substr(cursor, nameEnd - cursor);
        const Member* member = nullptr;
        for (const Member& candidate : members) {
            if (candidate.Name == name) {
                member = &candidate;
                break;
            }
        }
        if (member == nullptr) return kAll;
        size_t end = member->End();
        if (member->Count != 0U) {
            const size_t open = SkipSpace(source, nameEnd);
            size_t index = 0U;
            if (open < source.size() && source[open] == '[' &&
                LiteralIndex(source, open, &index)) {
                if (index >= member->Count) return kAll;
                end = member->Offset + member->Stride * (index + 1U);
            }
        }
        used = std::max(used, end);
    }
    return used;
}

// Vertex GLSL ("uniforms" block).
inline size_t VertexUsedBytes(std::string_view source) {
    return UsedBytes(source, "uniforms", detail::kVertexMembers,
                     "get_offset_register");
}

// Fragment GLSL ("fragment_uniforms" block).
inline size_t FragmentUsedBytes(std::string_view source) {
    return UsedBytes(source, "fragment_uniforms", detail::kFragmentMembers);
}

// Prefixes of a program (vertex + fragment): each block gets the largest
// prefix read by either stage (today each stage only declares
// its own block; the union only guards against a stage that starts reading the other's).
struct ProgramUsage {
    size_t Vertex = kAll;
    size_t Fragment = kAll;
};
[[nodiscard]] inline ProgramUsage ProgramUsedBytes(std::string_view vertex,
                                                   std::string_view fragment) {
    const auto unite = [](size_t a, size_t b) {
        return a == kAll || b == kAll ? kAll : std::max(a, b);
    };
    return {unite(VertexUsedBytes(vertex), VertexUsedBytes(fragment)),
            unite(FragmentUsedBytes(vertex), FragmentUsedBytes(fragment))};
}

// Bytes to copy from a block of `blockSize` bytes: the used prefix in
// multiples of vec4, never more than the block; trim off: the whole block.
[[nodiscard]] constexpr size_t SendBytes(bool trim, size_t used,
                                         size_t blockSize) noexcept {
    if (!trim || used == kAll) return blockSize;
    const size_t rounded = (used + 15U) & ~static_cast<size_t>(15U);
    return std::min(rounded, blockSize);
}

// What the ring already has of a block: the copied bytes (prefix), the size
// of the bound block and where (ring generation, offset).
struct LastUpload {
    std::vector<uint8_t> Bytes;
    size_t BlockSize = 0U;
    uint64_t Generation = 0U;
    uint64_t Offset = 0U;

    void Forget() noexcept {
        Bytes.clear();
        BlockSize = 0U;
        Generation = 0U;
    }
};

// Dedup: the previous block can be rebound when it has the same size, the
// same ring generation and the first `send` bytes (everything the program
// reads) equal. With send == block.size() it is the old rule (equal block).
[[nodiscard]] inline bool Reusable(const LastUpload& last, uint64_t generation,
                                   std::span<const uint8_t> block,
                                   size_t send) noexcept {
    return last.Generation != 0U && last.Generation == generation &&
           last.BlockSize == block.size() && last.Bytes.size() >= send &&
           send <= block.size() &&
           (send == 0U ||
            std::memcmp(last.Bytes.data(), block.data(), send) == 0);
}

// Decision for a block in a draw (the backend and the test use the same one):
// Reuse rebinds last.Offset; otherwise copy Send bytes into a new range of
// block.size() bytes. trim: config/gl_uniform_trim and the block goes to the ring;
// dedup: config/gl_lean and the block goes to the ring.
struct Plan {
    bool Reuse = false;
    size_t Send = 0U;
};
[[nodiscard]] inline Plan PlanUpload(bool trim, bool dedup,
                                     const LastUpload& last,
                                     uint64_t generation,
                                     std::span<const uint8_t> block,
                                     size_t used) noexcept {
    Plan plan;
    plan.Send = SendBytes(trim, used, block.size());
    plan.Reuse = dedup && Reusable(last, generation, block, plan.Send);
    return plan;
}
// After copying `send` bytes from `block` to `offset` (with dedup).
inline void Remember(LastUpload& last, std::span<const uint8_t> block,
                     size_t send, uint64_t generation, uint64_t offset) {
    last.Bytes.assign(block.begin(), block.begin() + static_cast<std::ptrdiff_t>(send));
    last.BlockSize = block.size();
    last.Generation = generation;
    last.Offset = offset;
}

} // namespace Fast::PicaGlUniformTrim
