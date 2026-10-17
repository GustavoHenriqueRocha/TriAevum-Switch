#pragma once

// Shadow copy of the fixed-function GL state the PICA draw path sets
// (gfx_opengl_pica.cpp, SubmitPicaDraw). Every draw used to issue the whole
// state again (~35 calls: framebuffer, draw buffer, VAO, program, four
// texture units, colour mask, blend/logic op, depth, stencil, cull,
// viewport, scissor) and then rebind the legacy VAO. Mesa filters most
// repeats itself, but each one still pays the dispatch, the context lookup
// and the enum validation on the render thread; the VAO ping-pong was not a
// repeat at all and made the state tracker rebuild the vertex arrays of
// every draw.
//
// The cache only answers "does this call change anything we know of";
// the GL calls stay in the backend, so it is free of GL and is checked
// offline (tests/gfx_opengl_pica_state_cache_tests.cpp). A value is
// "unknown" after Invalidate() (anyone else may have touched GL: the legacy
// Fast3D path, the scanout/post chain, fills, clears, transfers), and then
// the next call is always issued. Disabled (config/gl_state_cache 0) every
// call is issued as before, and the counters still run, so the same build
// measures both sides of an A/B.
//
// Draw order, the values sent and the draw calls themselves never change:
// only calls that would set a value the context already holds are dropped.

#include <array>
#include <cstdint>
#include <cstring>

namespace Fast::PicaGlStateCache {

template <typename T>
struct Slot {
    T Value{};
    bool Known = false;

    void Reset() noexcept { Known = false; }
};

// Calls the draw path issued and those the cache dropped (each counted with
// its GL call weight: a framebuffer change is bind + draw buffer = 2).
struct Counters {
    uint64_t Issued = 0U;
    uint64_t Skipped = 0U;
    uint64_t ProgramBinds = 0U;
    uint64_t TextureBinds = 0U;
    uint64_t VertexArrayBinds = 0U;
    uint64_t FramebufferBinds = 0U;
};

inline constexpr uint32_t kTextureUnits = 16U;

struct State {
    bool Enabled = true;
    Counters Count;

    Slot<uint32_t> Framebuffer;
    Slot<uint32_t> VertexArray;
    Slot<uint32_t> Program;
    Slot<uint32_t> ActiveUnit;
    std::array<Slot<uint32_t>, kTextureUnits> Texture{};

    Slot<uint8_t> ColorMask;
    Slot<bool> Blend;
    Slot<bool> ColorLogicOp;
    Slot<std::array<uint32_t, 2>> BlendEquation;
    Slot<std::array<uint32_t, 4>> BlendFunc;
    Slot<std::array<uint32_t, 4>> BlendColor; // float bits
    Slot<uint32_t> LogicOp;

    Slot<bool> DepthTest;
    Slot<uint32_t> DepthFunc;
    Slot<bool> DepthMask;

    Slot<bool> StencilTest;
    Slot<std::array<uint32_t, 3>> StencilFunc;
    Slot<uint32_t> StencilMask;
    Slot<std::array<uint32_t, 3>> StencilOp;

    Slot<bool> CullFace;
    Slot<uint32_t> FrontFace;
    Slot<uint32_t> CullFaceMode;

    Slot<std::array<int32_t, 4>> Viewport;
    Slot<bool> ScissorTest;
    Slot<std::array<int32_t, 4>> Scissor;

    // True when the call must be issued; records the value either way.
    template <typename T>
    [[nodiscard]] bool Change(Slot<T>& slot, const T& value,
                              uint32_t calls = 1U) noexcept {
        if (Enabled && slot.Known && slot.Value == value) {
            Count.Skipped += calls;
            return false;
        }
        slot.Value = value;
        slot.Known = true;
        Count.Issued += calls;
        return true;
    }

    // glBindFramebuffer (+ glDrawBuffer, per-framebuffer state that only
    // needs restating when the binding moves).
    [[nodiscard]] bool BindFramebuffer(uint32_t framebuffer) noexcept {
        const bool issue = Change(Framebuffer, framebuffer, 2U);
        Count.FramebufferBinds += issue ? 1U : 0U;
        return issue;
    }
    [[nodiscard]] bool BindVertexArray(uint32_t vertexArray) noexcept {
        const bool issue = Change(VertexArray, vertexArray);
        Count.VertexArrayBinds += issue ? 1U : 0U;
        return issue;
    }
    [[nodiscard]] bool UseProgram(uint32_t program) noexcept {
        const bool issue = Change(Program, program);
        Count.ProgramBinds += issue ? 1U : 0U;
        return issue;
    }

    // Binding `texture` on `unit` needs: glActiveTexture when *activate, then
    // glBindTexture when the result is true. Disabled, both are always set
    // (the former loop issued the pair for every unit).
    [[nodiscard]] bool BindTexture(uint32_t unit, uint32_t texture,
                                   bool* activate) noexcept {
        if (Enabled && unit < kTextureUnits && Texture[unit].Known &&
            Texture[unit].Value == texture) {
            *activate = false;
            Count.Skipped += 2U;
            return false;
        }
        *activate = Change(ActiveUnit, unit);
        if (unit < kTextureUnits) {
            Texture[unit].Value = texture;
            Texture[unit].Known = true;
        }
        ++Count.Issued;
        ++Count.TextureBinds;
        return true;
    }

    // Helpers that create or upload a texture bind it on the active unit:
    // whatever that unit held is no longer known.
    void ForgetActiveTextureBinding() noexcept {
        if (!ActiveUnit.Known) {
            for (auto& texture : Texture) {
                texture.Reset();
            }
            return;
        }
        if (ActiveUnit.Value < kTextureUnits) {
            Texture[ActiveUnit.Value].Reset();
        }
    }

    // A fill, a target creation or a deferred Shadow2D clear binds another
    // framebuffer and leaves its own scissor and write masks.
    void ForgetClearState() noexcept {
        Framebuffer.Reset();
        ScissorTest.Reset();
        ColorMask.Reset();
        DepthMask.Reset();
        StencilMask.Reset();
    }

    void Invalidate() noexcept {
        const bool enabled = Enabled;
        const Counters count = Count;
        *this = State{};
        Enabled = enabled;
        Count = count;
    }
};

// GL_DEPTH_CLAMP across the PICA and legacy paths. The legacy Fast3D
// renderer enables it once (GfxRenderingAPIOGL::Init) and never turns it
// off. With it on, GL does not clip at the near/far planes: geometry between
// the eye and the near plane (PICA clip z > 0 with w > 0) is rasterized with
// its depth clamped to the near value instead of being clipped. The PICA
// clips there (as do the Vulkan backend, depthClampEnable = false, and the
// GL dialect's z_gl = -2z - w mapping, which assumes clipping), so in first
// person - Link's arm and sleeve right in front of the camera, the
// slingshot aim - a huge polygon covered half the screen on the Switch.
// PICA draws therefore run with the clamp off; the legacy path gets it back
// before its next draw (RestoreLegacyVertexArray). Kept outside State: no
// one else touches GL_DEPTH_CLAMP, so Invalidate() must not forget it.
struct DepthClamp {
    // GL_DEPTH_CLAMP is currently disabled for the PICA path.
    bool PicaOff = false;

    // Before a PICA draw: true means glDisable(GL_DEPTH_CLAMP).
    [[nodiscard]] bool EnterPica() noexcept {
        if (PicaOff) {
            return false;
        }
        PicaOff = true;
        return true;
    }

    // Before legacy GL work: true means glEnable(GL_DEPTH_CLAMP).
    [[nodiscard]] bool LeavePica() noexcept {
        if (!PicaOff) {
            return false;
        }
        PicaOff = false;
        return true;
    }
};

[[nodiscard]] inline uint32_t FloatBits(float value) noexcept {
    uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// config/gl_state_cache: on unless the text starts with 0/n/f.
// config/gl_stats: off unless the text starts with 1/y/t (an empty file
// counts as on: creating it is the switch).
[[nodiscard]] inline bool ParseSwitch(const char* text, bool fallback) noexcept {
    if (text == nullptr) {
        return fallback;
    }
    for (; *text != '\0'; ++text) {
        const char c = *text;
        if (c == '1' || c == 'y' || c == 'Y' || c == 't' || c == 'T') {
            return true;
        }
        if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F') {
            return false;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
    }
    return fallback;
}

} // namespace Fast::PicaGlStateCache
