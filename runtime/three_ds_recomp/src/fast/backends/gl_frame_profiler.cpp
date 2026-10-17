// Perfil por etapa da render thread do backend OpenGL; veja
// fast/backends/gl_frame_profiler.h.

#include "fast/backends/gl_frame_profiler.h"

#if defined(ENABLE_OPENGL) && !defined(USE_OPENGLES) && !defined(__APPLE__)
#define TRIAEVUM_GL_PERF_GPU 1
#include "fast/backends/gfx_opengl.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>

namespace Fast::GlPerf {

namespace {

constexpr std::array<const char*, kStageCount> kStageNames{
    "begin", "clear", "pica_top", "pica_bottom", "pica_off", "pica_canon",
    "transfer", "post_aa", "post_easu", "scanout", "ui", "present",
    "other", "pacing", "idle"};

// Contadores de chamadas GL (ganchos nos ponteiros do glad, só no Switch).
enum Counter : size_t {
    kDraws, kClears, kFboBinds, kFboChanges, kTexBinds, kProgramBinds,
    kProgramChanges, kVaoBinds, kUboBinds, kUniforms, kBufferUploadBytes,
    kBufferReallocs, kMapBytes, kTextureUploadBytes, kBlits, kBarriers,
    kSyncs, kFinish, kFlush, kGetError, kGets, kReadbacks, kGenDelete,
    kEnables,
    // Caminho PICA (GlPerf::CountPica, bits PicaGlGpuLean::CountBit).
    kPicaFsDiscard, kPicaFsDepth, kPicaFsImage, kPicaBlended,
    kPicaLogicCopyOff, kPicaBlendIdentityOff, kPicaNoEffectSkipped,
    kPicaFullDepthClears, kCounterCount
};
constexpr size_t kPicaCountFirst = kPicaFsDiscard;
constexpr std::array<const char*, kCounterCount> kCounterNames{
    "draws", "clears", "fbo_binds", "fbo_changes", "tex_binds", "prog_binds",
    "prog_changes", "vao_binds", "ubo_binds", "uniforms", "buf_upload_kb",
    "buf_reallocs", "map_kb", "tex_upload_kb", "blits", "barriers", "syncs",
    "finish", "flush", "get_error", "gets", "readbacks", "gen_delete",
    "enables", "fs_discard", "fs_depth", "fs_image", "blended",
    "logic_copy_off", "blend_identity_off", "noeffect_skipped",
    "full_depth_clears"};
constexpr bool CounterIsBytes(size_t counter) {
    return counter == kBufferUploadBytes || counter == kMapBytes ||
           counter == kTextureUploadBytes;
}

enum FrameKind : uint8_t { kStep, kInterp, kHold, kKindCount };
constexpr std::array<const char*, kKindCount> kKindNames{"step", "interp", "hold"};

// GL calls happen on the thread that holds the context (the render thread,
// or the game thread while it runs a synchronous command with the worker
// idle): plain counters, no atomics in the hooks.
std::array<uint64_t, kCounterCount> gFrameCounters{};

uint64_t NowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

struct KindWindow {
    uint64_t Frames = 0;
    uint64_t FrameNs = 0;  // FrameBegin -> FrameEnd (parede, render thread)
    std::array<uint64_t, kStageCount> CpuNs{};
    std::array<uint64_t, kCounterCount> Counters{};
    std::array<uint64_t, kCounterCount> CounterMax{};
    uint64_t GpuSamples = 0;
    uint64_t GpuTotalNs = 0;
    uint64_t GpuMaxNs = 0;
    std::array<uint64_t, kGpuStageCount> GpuNs{};
};

struct Window {
    std::array<KindWindow, kKindCount> Kinds{};
    uint64_t GpuDropped = 0;
    uint64_t GpuOverflows = 0;
    uint64_t Unbalanced = 0;
};

struct State {
    // Configuration.
    uint32_t SampleEvery = 30;
    bool GpuTimer = false;
    int TimerBits = 0;
    bool Hooks = false;

    // Render-thread tracking.
    Stage Cpu = Stage::Idle;
    Stage Gpu = Stage::Begin;
    uint64_t LastNs = 0;
    uint64_t FrameStartNs = 0;
    bool FrameOpen = false;
    bool Canonical = false;
    bool FrameAuthoritative = false;
    bool FrameRepeated = false;
    uint64_t FrameSerial = 0;
    std::array<uint64_t, kStageCount> FrameCpuNs{};

    std::mutex Mutex;  // guards Published
    Window Published;
};

State& S() {
    static State state;
    return state;
}

void Charge(State& state, uint64_t now) {
    if (state.LastNs != 0U && now > state.LastNs) {
        state.FrameCpuNs[static_cast<size_t>(state.Cpu)] += now - state.LastNs;
    }
    state.LastNs = now;
}

#if defined(TRIAEVUM_GL_PERF_GPU)

// ---------------------------------------------------------------- GPU ----
constexpr size_t kSlots = 4;
constexpr uint32_t kQueriesPerSlot = 128;

struct Slot {
    std::array<GLuint, kQueriesPerSlot> Queries{};
    std::array<Stage, kQueriesPerSlot> Stages{};
    uint32_t Used = 0;
    uint64_t Serial = 0;
    FrameKind Kind = kStep;
    bool Pending = false;
    bool Overflowed = false;
};
std::array<Slot, kSlots> gSlots{};
int gRecording = -1;

void IssueTimestamp(Stage stage, bool final) {
    if (gRecording < 0) return;
    auto& slot = gSlots[static_cast<size_t>(gRecording)];
    // The last query is reserved for the end of the frame; past the limit
    // the remaining stages run under the last recorded one.
    if (!final && slot.Used + 1U >= kQueriesPerSlot) {
        slot.Overflowed = true;
        return;
    }
    if (slot.Used >= kQueriesPerSlot) return;
    glQueryCounter(slot.Queries[slot.Used], GL_TIMESTAMP);
    slot.Stages[slot.Used] = stage;
    ++slot.Used;
}

// Reads every sampled frame whose results are already available; never waits.
void PollGpu(State& state) {
    for (auto& slot : gSlots) {
        if (!slot.Pending || slot.Used < 2U) {
            slot.Pending = false;
            continue;
        }
        if (slot.Serial + 2U > state.FrameSerial) continue;
        GLint available = 0;
        glGetQueryObjectiv(slot.Queries[slot.Used - 1U],
                           GL_QUERY_RESULT_AVAILABLE, &available);
        if (available == 0) continue;
        std::array<GLuint64, kQueriesPerSlot> times{};
        for (uint32_t index = 0; index < slot.Used; ++index) {
            glGetQueryObjectui64v(slot.Queries[index], GL_QUERY_RESULT,
                                  &times[index]);
        }
        slot.Pending = false;
        std::lock_guard lock(state.Mutex);
        if (slot.Overflowed) ++state.Published.GpuOverflows;
        auto& kind = state.Published.Kinds[slot.Kind];
        for (uint32_t index = 0; index + 1U < slot.Used; ++index) {
            const auto stage = static_cast<size_t>(slot.Stages[index]);
            if (times[index + 1U] >= times[index] && stage < kGpuStageCount) {
                kind.GpuNs[stage] += times[index + 1U] - times[index];
            }
        }
        const uint64_t total = times[slot.Used - 1U] >= times[0]
                                   ? times[slot.Used - 1U] - times[0]
                                   : 0U;
        kind.GpuTotalNs += total;
        kind.GpuMaxNs = std::max(kind.GpuMaxNs, total);
        ++kind.GpuSamples;
    }
}

void BeginGpuSample(State& state) {
    if (!state.GpuTimer) return;
    PollGpu(state);
    // One sample per block of N frames, one frame later in each block: with
    // fps60 step and interp frames alternate, and a fixed phase with an even
    // N (the default 30) sampled only one of the two kinds (no step_gpu_* in
    // any perf.txt).
    const uint64_t serial = state.FrameSerial;
    if ((serial + serial / state.SampleEvery) % state.SampleEvery != 0U) return;
    int free = -1;
    for (size_t index = 0; index < kSlots; ++index) {
        if (!gSlots[index].Pending) {
            free = static_cast<int>(index);
            break;
        }
    }
    if (free < 0) {
        // Every slot still waits for the GPU: drop the oldest.
        size_t oldest = 0;
        for (size_t index = 1; index < kSlots; ++index) {
            if (gSlots[index].Serial < gSlots[oldest].Serial) oldest = index;
        }
        gSlots[oldest].Pending = false;
        free = static_cast<int>(oldest);
        std::lock_guard lock(state.Mutex);
        ++state.Published.GpuDropped;
    }
    gRecording = free;
    auto& slot = gSlots[static_cast<size_t>(free)];
    slot.Used = 0;
    slot.Overflowed = false;
    slot.Serial = state.FrameSerial;
    IssueTimestamp(Stage::Begin, false);
}

void EndGpuSample(FrameKind kind) {
    if (gRecording < 0) return;
    IssueTimestamp(Stage::Present, true);
    // The final query sits after the swap: without a flush it would reach
    // the GPU only with the next frame's commands, and "present" would
    // include the wait between frames. Sampled frames only; the hook's count
    // of this flush is taken back.
    glFlush();
    if (S().Hooks && gFrameCounters[kFlush] != 0U) --gFrameCounters[kFlush];
    auto& slot = gSlots[static_cast<size_t>(gRecording)];
    slot.Kind = kind;
    slot.Pending = true;
    gRecording = -1;
}

void InitGpu(State& state) {
    // Errors left by earlier code are not the probe's.
    for (int drained = 0; drained < 16 && glGetError() != GL_NO_ERROR; ++drained) {
    }
    GLint bits = 0;
    glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &bits);
    state.TimerBits = bits;
    if (bits <= 0) return;
    for (auto& slot : gSlots) {
        glGenQueries(static_cast<GLsizei>(slot.Queries.size()), slot.Queries.data());
    }
    // An unsupported timestamp query raises an error here.
    bool ok = true;
    for (int drained = 0; drained < 16 && glGetError() != GL_NO_ERROR; ++drained) {
        ok = false;
    }
    state.GpuTimer = ok;
}

#else
void BeginGpuSample(State&) {}
void EndGpuSample(FrameKind) {}
void InitGpu(State&) {}
void IssueTimestamp(Stage, bool) {}
#endif

#if defined(TRIAEVUM_GL_PERF_GPU) && defined(__SWITCH__)
// ---------------------------------------------------- GL call counters ----
// Hooks replace glad's function pointers only when the profiler is on.

inline void Add(Counter counter, uint64_t value = 1U) {
    gFrameCounters[counter] += value;
}

uint64_t PixelBytes(GLenum format, GLenum type) {
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        return 2;
    case GL_UNSIGNED_INT_8_8_8_8:
    case GL_UNSIGNED_INT_8_8_8_8_REV:
    case GL_UNSIGNED_INT_10_10_10_2:
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_24_8:
    case GL_UNSIGNED_INT_10F_11F_11F_REV:
        return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        return 8;
    default:
        break;
    }
    uint64_t channels = 4;
    switch (format) {
    case GL_RED: case GL_RED_INTEGER: case GL_DEPTH_COMPONENT:
    case GL_STENCIL_INDEX: case GL_GREEN: case GL_BLUE:
        channels = 1; break;
    case GL_RG: case GL_RG_INTEGER: case GL_DEPTH_STENCIL:
        channels = 2; break;
    case GL_RGB: case GL_BGR: case GL_RGB_INTEGER:
        channels = 3; break;
    default:
        channels = 4; break;
    }
    uint64_t component = 1;
    switch (type) {
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT:
        component = 2; break;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT:
        component = 4; break;
    default:
        component = 1; break;
    }
    return channels * component;
}

GLuint gDrawFramebuffer = 0;
GLuint gProgram = 0;

// Each hook counts and forwards to the driver's original entry point. Every
// use of the name is a token paste, so glad's "#define glX glad_glX" never
// expands inside the macro.
#define TRIAEVUM_GL_PERF_HOOK(fn, params, args, ...)                           \
    decltype(glad_##fn) real_##fn = nullptr;                                   \
    auto hook_##fn params -> decltype(real_##fn args) {                        \
        __VA_ARGS__;                                                           \
        return real_##fn args;                                                 \
    }

TRIAEVUM_GL_PERF_HOOK(glDrawArrays, (GLenum m, GLint f, GLsizei c), (m, f, c), Add(kDraws))
TRIAEVUM_GL_PERF_HOOK(glDrawElements, (GLenum m, GLsizei c, GLenum t, const void* i),
                      (m, c, t, i), Add(kDraws))
TRIAEVUM_GL_PERF_HOOK(glDrawElementsBaseVertex,
                      (GLenum m, GLsizei c, GLenum t, const void* i, GLint b),
                      (m, c, t, i, b), Add(kDraws))
TRIAEVUM_GL_PERF_HOOK(glDrawArraysInstanced, (GLenum m, GLint f, GLsizei c, GLsizei n),
                      (m, f, c, n), Add(kDraws))
TRIAEVUM_GL_PERF_HOOK(glDrawElementsInstanced,
                      (GLenum m, GLsizei c, GLenum t, const void* i, GLsizei n),
                      (m, c, t, i, n), Add(kDraws))
TRIAEVUM_GL_PERF_HOOK(glDrawElementsInstancedBaseVertex,
                      (GLenum m, GLsizei c, GLenum t, const void* i, GLsizei n, GLint b),
                      (m, c, t, i, n, b), Add(kDraws))
TRIAEVUM_GL_PERF_HOOK(glDrawRangeElements,
                      (GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void* i),
                      (m, s, e, c, t, i), Add(kDraws))
TRIAEVUM_GL_PERF_HOOK(glClear, (GLbitfield mask), (mask), Add(kClears))
TRIAEVUM_GL_PERF_HOOK(glClearBufferfv, (GLenum b, GLint d, const GLfloat* v), (b, d, v),
                      Add(kClears))
TRIAEVUM_GL_PERF_HOOK(glClearBufferiv, (GLenum b, GLint d, const GLint* v), (b, d, v),
                      Add(kClears))
TRIAEVUM_GL_PERF_HOOK(glClearBufferuiv, (GLenum b, GLint d, const GLuint* v), (b, d, v),
                      Add(kClears))
TRIAEVUM_GL_PERF_HOOK(glClearBufferfi, (GLenum b, GLint d, GLfloat z, GLint s), (b, d, z, s),
                      Add(kClears))
TRIAEVUM_GL_PERF_HOOK(glBindFramebuffer, (GLenum target, GLuint fb), (target, fb),
                      Add(kFboBinds);
                      if (target != GL_READ_FRAMEBUFFER && fb != gDrawFramebuffer) {
                          gDrawFramebuffer = fb;
                          Add(kFboChanges);
                      })
TRIAEVUM_GL_PERF_HOOK(glBindTexture, (GLenum t, GLuint x), (t, x), Add(kTexBinds))
TRIAEVUM_GL_PERF_HOOK(glBindImageTexture,
                      (GLuint u, GLuint x, GLint l, GLboolean y, GLint z, GLenum a, GLenum f),
                      (u, x, l, y, z, a, f), Add(kTexBinds))
TRIAEVUM_GL_PERF_HOOK(glBindSampler, (GLuint u, GLuint s), (u, s), Add(kTexBinds))
TRIAEVUM_GL_PERF_HOOK(glUseProgram, (GLuint program), (program),
                      Add(kProgramBinds);
                      if (program != gProgram) {
                          gProgram = program;
                          Add(kProgramChanges);
                      })
TRIAEVUM_GL_PERF_HOOK(glBindVertexArray, (GLuint a), (a), Add(kVaoBinds))
TRIAEVUM_GL_PERF_HOOK(glBindBufferBase, (GLenum t, GLuint i, GLuint b), (t, i, b),
                      Add(kUboBinds))
TRIAEVUM_GL_PERF_HOOK(glBindBufferRange,
                      (GLenum t, GLuint i, GLuint b, GLintptr o, GLsizeiptr s),
                      (t, i, b, o, s), Add(kUboBinds))
TRIAEVUM_GL_PERF_HOOK(glBufferData, (GLenum t, GLsizeiptr s, const void* d, GLenum u),
                      (t, s, d, u),
                      Add(kBufferReallocs);
                      if (d != nullptr && s > 0) Add(kBufferUploadBytes, static_cast<uint64_t>(s)))
TRIAEVUM_GL_PERF_HOOK(glBufferSubData, (GLenum t, GLintptr o, GLsizeiptr s, const void* d),
                      (t, o, s, d),
                      if (s > 0) Add(kBufferUploadBytes, static_cast<uint64_t>(s)))
TRIAEVUM_GL_PERF_HOOK(glMapBufferRange, (GLenum t, GLintptr o, GLsizeiptr s, GLbitfield a),
                      (t, o, s, a),
                      if ((a & GL_MAP_WRITE_BIT) != 0U && s > 0)
                          Add(kMapBytes, static_cast<uint64_t>(s)))
TRIAEVUM_GL_PERF_HOOK(glTexImage2D,
                      (GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f,
                       GLenum y, const void* p),
                      (t, l, i, w, h, b, f, y, p),
                      if (p != nullptr && w > 0 && h > 0)
                          Add(kTextureUploadBytes,
                              static_cast<uint64_t>(w) * static_cast<uint64_t>(h) *
                                  PixelBytes(f, y)))
TRIAEVUM_GL_PERF_HOOK(glTexSubImage2D,
                      (GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f,
                       GLenum ty, const void* p),
                      (t, l, x, y, w, h, f, ty, p),
                      if (p != nullptr && w > 0 && h > 0)
                          Add(kTextureUploadBytes,
                              static_cast<uint64_t>(w) * static_cast<uint64_t>(h) *
                                  PixelBytes(f, ty)))
TRIAEVUM_GL_PERF_HOOK(glCompressedTexImage2D,
                      (GLenum t, GLint l, GLenum i, GLsizei w, GLsizei h, GLint b, GLsizei s,
                       const void* d),
                      (t, l, i, w, h, b, s, d),
                      if (d != nullptr && s > 0) Add(kTextureUploadBytes, static_cast<uint64_t>(s)))
TRIAEVUM_GL_PERF_HOOK(glBlitFramebuffer,
                      (GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h,
                       GLbitfield m, GLenum x),
                      (a, b, c, d, e, f, g, h, m, x), Add(kBlits))
TRIAEVUM_GL_PERF_HOOK(glCopyImageSubData,
                      (GLuint a, GLenum b, GLint c, GLint d, GLint e, GLint f, GLuint g,
                       GLenum h, GLint i, GLint j, GLint k, GLint l, GLsizei m, GLsizei n,
                       GLsizei o),
                      (a, b, c, d, e, f, g, h, i, j, k, l, m, n, o), Add(kBlits))
TRIAEVUM_GL_PERF_HOOK(glCopyTexSubImage2D,
                      (GLenum t, GLint l, GLint a, GLint b, GLint x, GLint y, GLsizei w,
                       GLsizei h),
                      (t, l, a, b, x, y, w, h), Add(kBlits))
TRIAEVUM_GL_PERF_HOOK(glMemoryBarrier, (GLbitfield b), (b), Add(kBarriers))
TRIAEVUM_GL_PERF_HOOK(glFenceSync, (GLenum c, GLbitfield f), (c, f), Add(kSyncs))
TRIAEVUM_GL_PERF_HOOK(glClientWaitSync, (GLsync s, GLbitfield f, GLuint64 t), (s, f, t),
                      Add(kSyncs))
TRIAEVUM_GL_PERF_HOOK(glWaitSync, (GLsync s, GLbitfield f, GLuint64 t), (s, f, t),
                      Add(kSyncs))
TRIAEVUM_GL_PERF_HOOK(glFinish, (void), (), Add(kFinish))
TRIAEVUM_GL_PERF_HOOK(glFlush, (void), (), Add(kFlush))
TRIAEVUM_GL_PERF_HOOK(glGetError, (void), (), Add(kGetError))
TRIAEVUM_GL_PERF_HOOK(glGetIntegerv, (GLenum p, GLint* d), (p, d), Add(kGets))
TRIAEVUM_GL_PERF_HOOK(glGetFloatv, (GLenum p, GLfloat* d), (p, d), Add(kGets))
TRIAEVUM_GL_PERF_HOOK(glGetBooleanv, (GLenum p, GLboolean* d), (p, d), Add(kGets))
TRIAEVUM_GL_PERF_HOOK(glIsEnabled, (GLenum c), (c), Add(kGets))
TRIAEVUM_GL_PERF_HOOK(glCheckFramebufferStatus, (GLenum t), (t), Add(kGets))
TRIAEVUM_GL_PERF_HOOK(glReadPixels,
                      (GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void* p),
                      (x, y, w, h, f, t, p), Add(kReadbacks))
TRIAEVUM_GL_PERF_HOOK(glGenTextures, (GLsizei n, GLuint* x), (n, x), Add(kGenDelete))
TRIAEVUM_GL_PERF_HOOK(glDeleteTextures, (GLsizei n, const GLuint* x), (n, x), Add(kGenDelete))
TRIAEVUM_GL_PERF_HOOK(glGenBuffers, (GLsizei n, GLuint* x), (n, x), Add(kGenDelete))
TRIAEVUM_GL_PERF_HOOK(glDeleteBuffers, (GLsizei n, const GLuint* x), (n, x), Add(kGenDelete))
TRIAEVUM_GL_PERF_HOOK(glGenFramebuffers, (GLsizei n, GLuint* x), (n, x), Add(kGenDelete))
TRIAEVUM_GL_PERF_HOOK(glDeleteFramebuffers, (GLsizei n, const GLuint* x), (n, x),
                      Add(kGenDelete))
TRIAEVUM_GL_PERF_HOOK(glEnable, (GLenum c), (c), Add(kEnables))
TRIAEVUM_GL_PERF_HOOK(glDisable, (GLenum c), (c), Add(kEnables))
TRIAEVUM_GL_PERF_HOOK(glUniform1i, (GLint l, GLint a), (l, a), Add(kUniforms))
TRIAEVUM_GL_PERF_HOOK(glUniform1f, (GLint l, GLfloat a), (l, a), Add(kUniforms))
TRIAEVUM_GL_PERF_HOOK(glUniform2f, (GLint l, GLfloat a, GLfloat b), (l, a, b), Add(kUniforms))
TRIAEVUM_GL_PERF_HOOK(glUniform4f, (GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d),
                      (l, a, b, c, d), Add(kUniforms))
TRIAEVUM_GL_PERF_HOOK(glUniform4ui, (GLint l, GLuint a, GLuint b, GLuint c, GLuint d),
                      (l, a, b, c, d), Add(kUniforms))
TRIAEVUM_GL_PERF_HOOK(glUniform1iv, (GLint l, GLsizei n, const GLint* v), (l, n, v),
                      Add(kUniforms))
TRIAEVUM_GL_PERF_HOOK(glUniform1uiv, (GLint l, GLsizei n, const GLuint* v), (l, n, v),
                      Add(kUniforms))
TRIAEVUM_GL_PERF_HOOK(glUniformMatrix4fv,
                      (GLint l, GLsizei n, GLboolean t, const GLfloat* v), (l, n, t, v),
                      Add(kUniforms))
#undef TRIAEVUM_GL_PERF_HOOK

bool InstallHooks() {
    bool any = false;
#define TRIAEVUM_GL_PERF_INSTALL(fn)                                           \
    if (glad_##fn != nullptr && real_##fn == nullptr) {                        \
        real_##fn = glad_##fn;                                                 \
        glad_##fn = hook_##fn;                                                 \
        any = true;                                                            \
    }
    TRIAEVUM_GL_PERF_INSTALL(glDrawArrays)
    TRIAEVUM_GL_PERF_INSTALL(glDrawElements)
    TRIAEVUM_GL_PERF_INSTALL(glDrawElementsBaseVertex)
    TRIAEVUM_GL_PERF_INSTALL(glDrawArraysInstanced)
    TRIAEVUM_GL_PERF_INSTALL(glDrawElementsInstanced)
    TRIAEVUM_GL_PERF_INSTALL(glDrawElementsInstancedBaseVertex)
    TRIAEVUM_GL_PERF_INSTALL(glDrawRangeElements)
    TRIAEVUM_GL_PERF_INSTALL(glClear)
    TRIAEVUM_GL_PERF_INSTALL(glClearBufferfv)
    TRIAEVUM_GL_PERF_INSTALL(glClearBufferiv)
    TRIAEVUM_GL_PERF_INSTALL(glClearBufferuiv)
    TRIAEVUM_GL_PERF_INSTALL(glClearBufferfi)
    TRIAEVUM_GL_PERF_INSTALL(glBindFramebuffer)
    TRIAEVUM_GL_PERF_INSTALL(glBindTexture)
    TRIAEVUM_GL_PERF_INSTALL(glBindImageTexture)
    TRIAEVUM_GL_PERF_INSTALL(glBindSampler)
    TRIAEVUM_GL_PERF_INSTALL(glUseProgram)
    TRIAEVUM_GL_PERF_INSTALL(glBindVertexArray)
    TRIAEVUM_GL_PERF_INSTALL(glBindBufferBase)
    TRIAEVUM_GL_PERF_INSTALL(glBindBufferRange)
    TRIAEVUM_GL_PERF_INSTALL(glBufferData)
    TRIAEVUM_GL_PERF_INSTALL(glBufferSubData)
    TRIAEVUM_GL_PERF_INSTALL(glMapBufferRange)
    TRIAEVUM_GL_PERF_INSTALL(glTexImage2D)
    TRIAEVUM_GL_PERF_INSTALL(glTexSubImage2D)
    TRIAEVUM_GL_PERF_INSTALL(glCompressedTexImage2D)
    TRIAEVUM_GL_PERF_INSTALL(glBlitFramebuffer)
    TRIAEVUM_GL_PERF_INSTALL(glCopyImageSubData)
    TRIAEVUM_GL_PERF_INSTALL(glCopyTexSubImage2D)
    TRIAEVUM_GL_PERF_INSTALL(glMemoryBarrier)
    TRIAEVUM_GL_PERF_INSTALL(glFenceSync)
    TRIAEVUM_GL_PERF_INSTALL(glClientWaitSync)
    TRIAEVUM_GL_PERF_INSTALL(glWaitSync)
    TRIAEVUM_GL_PERF_INSTALL(glFinish)
    TRIAEVUM_GL_PERF_INSTALL(glFlush)
    TRIAEVUM_GL_PERF_INSTALL(glGetError)
    TRIAEVUM_GL_PERF_INSTALL(glGetIntegerv)
    TRIAEVUM_GL_PERF_INSTALL(glGetFloatv)
    TRIAEVUM_GL_PERF_INSTALL(glGetBooleanv)
    TRIAEVUM_GL_PERF_INSTALL(glIsEnabled)
    TRIAEVUM_GL_PERF_INSTALL(glCheckFramebufferStatus)
    TRIAEVUM_GL_PERF_INSTALL(glReadPixels)
    TRIAEVUM_GL_PERF_INSTALL(glGenTextures)
    TRIAEVUM_GL_PERF_INSTALL(glDeleteTextures)
    TRIAEVUM_GL_PERF_INSTALL(glGenBuffers)
    TRIAEVUM_GL_PERF_INSTALL(glDeleteBuffers)
    TRIAEVUM_GL_PERF_INSTALL(glGenFramebuffers)
    TRIAEVUM_GL_PERF_INSTALL(glDeleteFramebuffers)
    TRIAEVUM_GL_PERF_INSTALL(glEnable)
    TRIAEVUM_GL_PERF_INSTALL(glDisable)
    TRIAEVUM_GL_PERF_INSTALL(glUniform1i)
    TRIAEVUM_GL_PERF_INSTALL(glUniform1f)
    TRIAEVUM_GL_PERF_INSTALL(glUniform2f)
    TRIAEVUM_GL_PERF_INSTALL(glUniform4f)
    TRIAEVUM_GL_PERF_INSTALL(glUniform4ui)
    TRIAEVUM_GL_PERF_INSTALL(glUniform1iv)
    TRIAEVUM_GL_PERF_INSTALL(glUniform1uiv)
    TRIAEVUM_GL_PERF_INSTALL(glUniformMatrix4fv)
#undef TRIAEVUM_GL_PERF_INSTALL
    return any;
}
#else
bool InstallHooks() {
    return false;
}
#endif

std::optional<std::string> ReadConfig() {
#if defined(__SWITCH__)
    std::ifstream stream("sdmc:/switch/oot3dre/config/gpu_timing");
    if (!stream) return std::nullopt;
    std::string text;
    std::getline(stream, text);
    return text;
#else
    const char* value = std::getenv("TRIAEVUM_GL_GPU_TIMING");
    if (value == nullptr || value[0] == '\0' || std::string(value) == "0") {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

FrameKind CurrentFrameKind(const State& state) {
    if (state.FrameAuthoritative) return kStep;
    if (state.FrameRepeated) return kInterp;
    return kHold;
}

void AppendMs(std::ostringstream& out, const char* prefix, const char* name,
              double nanoseconds, double count) {
    if (count <= 0.0 || nanoseconds <= 0.0) return;
    out << ' ' << prefix << name << '=' << (nanoseconds / count / 1.0e6);
}

} // namespace

namespace {

void FrameEndSlow();

Stage CurrentStage() {
    return S().Cpu;
}

void MarkSlow(Stage stage) {
    auto& state = S();
    if (stage == state.Cpu &&
        (stage >= Stage::Other || stage == state.Gpu)) {
        return;
    }
    if (stage != state.Cpu) {
        Charge(state, NowNs());
        state.Cpu = stage;
    }
    if (stage < Stage::Other && stage != state.Gpu) {
        state.Gpu = stage;
        if (state.FrameOpen) IssueTimestamp(stage, false);
    }
}

void MarkPicaDrawSlow(uint32_t framebufferWidth, uint32_t framebufferHeight) {
    const auto& state = S();
    Stage stage = Stage::PicaOffscreen;
    if (state.Canonical) {
        stage = Stage::PicaCanonical;
    } else if (framebufferWidth == 480U && framebufferHeight == 400U) {
        // kNativeTopScreenPhysicalWidth/Height (scene_presentation_policy.h).
        stage = Stage::PicaTop;
    } else if (framebufferWidth == 240U && framebufferHeight == 320U) {
        stage = Stage::PicaBottom;
    }
    MarkSlow(stage);
}

void CountPicaSlow(uint32_t bits) {
    for (size_t bit = 0; bit < kCounterCount - kPicaCountFirst; ++bit) {
        if ((bits & (1U << bit)) != 0U) ++gFrameCounters[kPicaCountFirst + bit];
    }
}

void FrameBeginSlow() {
    auto& state = S();
    const uint64_t now = NowNs();
    if (state.FrameOpen) {
        // A frame begun without its end (inactive frame, error): close it.
        FrameEndSlow();
        std::lock_guard lock(state.Mutex);
        ++state.Published.Unbalanced;
    }
    // Work and idle time since the previous frame end (and GL calls made
    // then) count in this frame.
    Charge(state, now);
    state.FrameStartNs = now;
    state.FrameAuthoritative = false;
    state.FrameRepeated = false;
    state.Cpu = Stage::Begin;
    state.Gpu = Stage::Begin;
    state.FrameOpen = true;
    ++state.FrameSerial;
    BeginGpuSample(state);
}

void FrameEndSlow() {
    auto& state = S();
    if (!state.FrameOpen) return;
    const uint64_t now = NowNs();
    Charge(state, now);
    const FrameKind kind = CurrentFrameKind(state);
    EndGpuSample(kind);
    state.FrameOpen = false;
    state.Cpu = Stage::Idle;
    state.Gpu = Stage::Begin;
    {
        std::lock_guard lock(state.Mutex);
        auto& window = state.Published.Kinds[kind];
        ++window.Frames;
        window.FrameNs += now - state.FrameStartNs;
        for (size_t index = 0; index < kStageCount; ++index) {
            window.CpuNs[index] += state.FrameCpuNs[index];
        }
        for (size_t index = 0; index < kCounterCount; ++index) {
            window.Counters[index] += gFrameCounters[index];
            window.CounterMax[index] =
                std::max(window.CounterMax[index], gFrameCounters[index]);
        }
    }
    state.FrameCpuNs = {};
    gFrameCounters = {};
}

void SampleBeginSlow(bool authoritative, bool present) {
    auto& state = S();
    state.Canonical = !present;
    if (authoritative) state.FrameAuthoritative = true;
    if (present && !authoritative) state.FrameRepeated = true;
}

void SampleEndSlow() {
    S().Canonical = false;
}

} // namespace

void Init() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;
    const auto config = ReadConfig();
    if (!config.has_value()) return;
    auto& state = S();
    const long every = std::strtol(config->c_str(), nullptr, 10);
    if (every >= 1 && every <= 100000) state.SampleEvery = static_cast<uint32_t>(every);
    InitGpu(state);
    state.Hooks = InstallHooks();
    auto& hooks = detail::gHooks;
    hooks.Mark = &MarkSlow;
    hooks.MarkPicaDraw = &MarkPicaDrawSlow;
    hooks.CountPica = &CountPicaSlow;
    hooks.FrameBegin = &FrameBeginSlow;
    hooks.FrameEnd = &FrameEndSlow;
    hooks.SampleBegin = &SampleBeginSlow;
    hooks.SampleEnd = &SampleEndSlow;
    hooks.Current = &CurrentStage;
    detail::gEnabled.store(true, std::memory_order_release);
}

std::string TakeReport(uint64_t hostFrames) {
    if (!Enabled()) return {};
    auto& state = S();
    Window window;
    {
        std::lock_guard lock(state.Mutex);
        window = state.Published;
        state.Published = {};
    }
    std::ostringstream out;
    out << "glperf host_frames=" << hostFrames << " sample_every=" << state.SampleEvery
        << " gpu_timer=" << (state.GpuTimer ? 1 : 0) << " timer_bits=" << state.TimerBits
        << " gl_hooks=" << (state.Hooks ? 1 : 0) << " gpu_dropped=" << window.GpuDropped
        << " gpu_overflows=" << window.GpuOverflows << " unbalanced=" << window.Unbalanced;
    for (size_t kindIndex = 0; kindIndex < kKindCount; ++kindIndex) {
        const auto& kind = window.Kinds[kindIndex];
        if (kind.Frames == 0U) continue;
        const std::string prefix = std::string(kKindNames[kindIndex]) + '_';
        const double count = static_cast<double>(kind.Frames);
        out << ' ' << prefix << "frames=" << kind.Frames << ' ' << prefix
            << "cpu_ms=" << (kind.FrameNs / count / 1.0e6);
        const std::string cpuPrefix = prefix + "cpu_";
        for (size_t stage = 0; stage < kStageCount; ++stage) {
            AppendMs(out, cpuPrefix.c_str(), kStageNames[stage],
                     static_cast<double>(kind.CpuNs[stage]), count);
        }
        if (kind.GpuSamples != 0U) {
            const double samples = static_cast<double>(kind.GpuSamples);
            out << ' ' << prefix << "gpu_samples=" << kind.GpuSamples << ' ' << prefix
                << "gpu_ms=" << (kind.GpuTotalNs / samples / 1.0e6) << ' ' << prefix
                << "gpu_max_ms=" << (kind.GpuMaxNs / 1.0e6);
            const std::string gpuPrefix = prefix + "gpu_";
            for (size_t stage = 0; stage < kGpuStageCount; ++stage) {
                AppendMs(out, gpuPrefix.c_str(), kStageNames[stage],
                         static_cast<double>(kind.GpuNs[stage]), samples);
            }
        }
        if (state.Hooks) {
            for (size_t counter = 0; counter < kCounterCount; ++counter) {
                const double scale = CounterIsBytes(counter) ? 1024.0 : 1.0;
                out << ' ' << prefix << "n_" << kCounterNames[counter] << '='
                    << (kind.Counters[counter] / scale / count);
                if (counter == kDraws || counter == kGetError || counter == kFinish ||
                    counter == kBufferReallocs) {
                    out << ' ' << prefix << "max_" << kCounterNames[counter] << '='
                        << (kind.CounterMax[counter] / scale);
                }
            }
        }
    }
    return out.str();
}

} // namespace Fast::GlPerf
