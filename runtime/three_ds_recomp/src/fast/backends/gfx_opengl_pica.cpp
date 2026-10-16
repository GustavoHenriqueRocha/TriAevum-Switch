#ifdef ENABLE_OPENGL

#include "fast/backends/gfx_opengl.h"

#if defined(__SWITCH__)
#include <EGL/egl.h>
#include <switch.h>
#endif


#include "fast/backends/gfx_opengl_pica_post.h"
#include "fast/oot3d/pica_geometry_registry.h"
#include "fast/oot3d/pica_shadow2d.h"
#include "fast/oot3d/smaa_lookup_data.h"
#include "fast/renderer3ds/pica_composition_schedule.h"
#include "fast/renderer3ds/pica_gl_shader_dialect.h"
#include "oot3d/renderer/pica_texture_decode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <chrono>
#include <memory>
#include <mutex>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Fast {
namespace {

void SetPicaGlError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

#if !defined(USE_OPENGLES) && !defined(__APPLE__)

uint64_t HashBytes(std::span<const uint8_t> bytes) {
    uint64_t hash = 1469598103934665603ULL;
    for (const uint8_t value : bytes) {
        hash = (hash ^ value) * 1099511628211ULL;
    }
    return hash == 0U ? 1U : hash;
}

// Internal resolution scale (integer, like Citra/Azahar's "resolution
// scale"). Every PICA render target -- color, depth/stencil and the Shadow2D
// image -- and every display-transfer image is allocated N times larger per
// axis; viewport, scissor and transfer blits are multiplied by N and the
// scanout samples the large image. Keys, guest addresses and logical extents
// stay native, so N=1 issues exactly the same GL calls as before. The GL
// backend never reads PICA pixels back into guest memory (sampled textures
// are always decoded from guest bytes; only the live Shadow2D image is
// shared, and its consumer filters it through textureSize()), so no
// downsample path is needed.
//
// Read once: on the Switch from sdmc:/switch/oot3dre/config/resolution_scale
// (text "1", "2" or "3"); elsewhere from TRIAEVUM_GL_RESOLUTION_SCALE.
// Missing or invalid means 1.
constexpr uint32_t kPicaGlMaxResolutionScale = 3U;

uint32_t ParsePicaGlResolutionScale(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '\n' || text.front() == '\r')) {
        text.remove_prefix(1U);
    }
    if (text.empty() || text.front() < '1' || text.front() > '9') {
        return 1U;
    }
    if (text.size() > 1U && text[1] >= '0' && text[1] <= '9') {
        return 1U;
    }
    const uint32_t value = static_cast<uint32_t>(text.front() - '0');
    return std::min(value, kPicaGlMaxResolutionScale);
}

uint32_t ReadPicaGlResolutionScale() {
    static const uint32_t scale = [] {
#if defined(__SWITCH__)
        std::ifstream file("sdmc:/switch/oot3dre/config/resolution_scale");
        std::string text;
        if (!file || !std::getline(file, text)) {
            return 2U; // console default: 800x480 costs only idle GPU time
        }
        return ParsePicaGlResolutionScale(text);
#else
        const char* value = std::getenv("TRIAEVUM_GL_RESOLUTION_SCALE");
        return value != nullptr ? ParsePicaGlResolutionScale(value) : 1U;
#endif
    }();
    return scale;
}

// Optional scanout post-processing and texture filtering (all off unless a
// file exists), read once like the resolution scale: on the Switch from
// sdmc:/switch/oot3dre/config/{antialiasing,anisotropy,upscaler,sharpen};
// elsewhere from TRIAEVUM_GL_{ANTIALIASING,ANISOTROPY,UPSCALER,SHARPEN}.
// See gfx_opengl_pica_post.h for the accepted values.
std::optional<std::string> ReadPicaGlConfigText(const char* file,
                                                const char* environment) {
#if defined(__SWITCH__)
    (void)environment;
    std::ifstream stream(std::string("sdmc:/switch/oot3dre/config/") + file);
    std::string text;
    if (!stream || !std::getline(stream, text)) {
        return std::nullopt;
    }
    return text;
#else
    (void)file;
    const char* value = std::getenv(environment);
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
#endif
}

const PicaGlPost::Config& ReadPicaGlPostConfig() {
    static const PicaGlPost::Config config = PicaGlPost::ResolveConfig(
        ReadPicaGlConfigText("antialiasing", "TRIAEVUM_GL_ANTIALIASING"),
        ReadPicaGlConfigText("anisotropy", "TRIAEVUM_GL_ANISOTROPY"),
        ReadPicaGlConfigText("upscaler", "TRIAEVUM_GL_UPSCALER"),
        ReadPicaGlConfigText("sharpen", "TRIAEVUM_GL_SHARPEN"));
    return config;
}

struct PicaGlTargetKey {
    uint64_t Namespace = 0U;
    uint32_t ColorAddress = 0U;
    uint32_t DepthAddress = 0U;
    uint16_t Width = 0U;
    uint16_t Height = 0U;
    uint8_t ColorFormat = 0U;
    uint8_t DepthFormat = 0U;

    bool operator<(const PicaGlTargetKey& other) const noexcept {
        return std::tie(Namespace, ColorAddress, DepthAddress, Width, Height,
                        ColorFormat, DepthFormat) <
               std::tie(other.Namespace, other.ColorAddress,
                        other.DepthAddress, other.Width, other.Height,
                        other.ColorFormat, other.DepthFormat);
    }
};

struct PicaGlShaderKey {
    Renderer3ds::PicaShaderSourceIdentity Vertex;
    Renderer3ds::PicaShaderSourceIdentity Fragment;

    bool operator<(const PicaGlShaderKey& other) const noexcept {
        return std::tie(Vertex.Id, Vertex.SecondaryHash, Vertex.Size,
                        Fragment.Id, Fragment.SecondaryHash, Fragment.Size) <
               std::tie(other.Vertex.Id, other.Vertex.SecondaryHash,
                        other.Vertex.Size, other.Fragment.Id,
                        other.Fragment.SecondaryHash, other.Fragment.Size);
    }
};

struct PicaGlTextureKey {
    uint64_t ContentHash = 0U;
    uint32_t PhysicalAddress = 0U;
    uint16_t Width = 0U;
    uint16_t Height = 0U;
    uint8_t Format = 0U;
    uint8_t Type = 0U;
    uint8_t WrapS = 0U;
    uint8_t WrapT = 0U;
    bool MinLinear = false;
    bool MagLinear = false;
    bool MipLinear = false;
    int16_t LodBiasRaw = 0;
    uint8_t MinMipLevel = 0U;
    uint8_t MaxMipLevel = 0U;

    bool operator<(const PicaGlTextureKey& other) const noexcept {
        return std::tie(ContentHash, PhysicalAddress, Width, Height, Format,
                        Type, WrapS, WrapT, MinLinear, MagLinear, MipLinear,
                        LodBiasRaw, MinMipLevel, MaxMipLevel) <
               std::tie(other.ContentHash, other.PhysicalAddress,
                        other.Width, other.Height, other.Format, other.Type,
                        other.WrapS, other.WrapT, other.MinLinear,
                        other.MagLinear, other.MipLinear, other.LodBiasRaw,
                        other.MinMipLevel, other.MaxMipLevel);
    }
};

struct PicaGlProgram {
    GLuint Handle = 0U;
};

struct PicaGlTarget {
    // Allocated extent: the key's native extent times the resolution scale.
    uint32_t Width = 0U;
    uint32_t Height = 0U;
    GLuint Framebuffer = 0U;
    GLuint Color = 0U;
    GLuint DepthStencil = 0U;
    GLuint ShadowFramebuffer = 0U;
    GLuint Shadow = 0U;
};

struct PicaGlTexture {
    GLuint Handle = 0U;
    GLenum Format = GL_RGBA8;
};

struct PicaGlDisplay {
    GLuint Framebuffer = 0U;
    GLuint Color = 0U;
    // Native (guest) extent; the texture is Width/Height times the scale.
    uint32_t Width = 0U;
    uint32_t Height = 0U;
    bool Initialized = false;
};

// Intermediate image of the scanout post-processing chain. Reallocated only
// when its extent changes (first frame, dock/undock), never per frame.
struct PicaGlPostImage {
    GLuint Framebuffer = 0U;
    GLuint Texture = 0U;
    uint32_t Width = 0U;
    uint32_t Height = 0U;
};

struct PicaGlPostProgram {
    GLuint Handle = 0U;
    GLint Orient = -1;
    GLint FlipY = -1;
    GLint OutputSize = -1;
    std::array<GLint, 4> Params{-1, -1, -1, -1};
    // Last uploaded values: uniforms are re-sent only when they change, so
    // a steady frame issues no glUniform calls for the chain.
    int LastOrient = -1;
    int LastFlipY = -1;
    uint64_t LastOutputKey = 0U;
    uint64_t LastInputKey = 0U;
    int LastResample = -1;
};

struct PicaGlPostState {
    // Effective configuration (effects whose setup failed are turned off).
    PicaGlPost::Config Config;
    PicaGlPostProgram Fxaa;
    PicaGlPostProgram SmaaEdges;
    PicaGlPostProgram SmaaWeights;
    PicaGlPostProgram SmaaBlend;
    PicaGlPostProgram Easu;
    PicaGlPostProgram Rcas;
    GLuint SmaaArea = 0U;
    GLuint SmaaSearch = 0U;
    // Anti-aliased image, source (render x scale) extent.
    PicaGlPostImage Color;
    // SMAA edges and blend weights, source extent.
    PicaGlPostImage Edges;
    PicaGlPostImage Weights;
    // EASU output, portrait extent of the window viewport (only when RCAS
    // follows; otherwise EASU draws the window directly).
    PicaGlPostImage Upscaled;
};

struct PicaGlAttributeState {
    GLuint Buffer = 0U;
    GLsizei ByteStride = 0;
    uintptr_t ByteOffset = 0U;
    GLenum Type = GL_FLOAT;
    uint8_t ComponentCount = 0U;
    bool Configured = false;
};

struct PicaGlGeometryBuffer {
    GLuint Handle = 0U;
    uint64_t ContentVersion = 0U;
    uint64_t StructuralSignature = 0U;
};

GLuint CompilePicaGlShader(GLenum stage, std::string_view source) {
    const GLuint shader = glCreateShader(stage);
    const GLchar* text = source.data();
    const GLint length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &text, &length);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) {
        return shader;
    }
    GLint logLength = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<size_t>(std::max(logLength, 1)), '\0');
    glGetShaderInfoLog(shader, logLength, nullptr, log.data());
    log.resize(std::strlen(log.c_str()));
    const GLenum error = glGetError();
    glDeleteShader(shader);
    const char* stageName = stage == GL_VERTEX_SHADER ? "vertex" : "fragment";
    // An empty driver log says little; record the context and the source so
    // a failure on a new driver (e.g. Switch Mesa) can be reproduced offline.
    const auto glString = [](GLenum name) {
        const auto* value = reinterpret_cast<const char*>(glGetString(name));
        return std::string(value != nullptr ? value : "(null)");
    };
    std::ofstream(std::string("logs/failed_pica_") + stageName + ".glsl", std::ios::trunc) << source;
    throw std::runtime_error(
        std::string(stageName) + " PICA OpenGL shader failed: " + log +
        " [shader=" + std::to_string(shader) + " glError=" + std::to_string(error) +
        " GL_VERSION=" + glString(GL_VERSION) +
        " GLSL=" + glString(GL_SHADING_LANGUAGE_VERSION) +
        " source_bytes=" + std::to_string(source.size()) +
#if defined(__SWITCH__)
        " thread=" + std::to_string(threadGetCurHandle()) +
        " egl_ctx=" + std::to_string(reinterpret_cast<uintptr_t>(eglGetCurrentContext())) +
#endif
        "]");
}

void BindPicaGlProgramSamplers(GLuint program) {
    for (GLint slot = 0; slot < 3; ++slot) {
        const std::string name = "pica_texture" + std::to_string(slot);
        const GLint location = glGetUniformLocation(program, name.c_str());
        if (location >= 0) {
            glUseProgram(program);
            glUniform1i(location, slot + 1);
        }
    }
    const GLint lighting = glGetUniformLocation(program, "pica_lighting_lut");
    if (lighting >= 0) {
        glUseProgram(program);
        glUniform1i(lighting, 13);
    }
}

GLuint LinkPicaGlProgram(std::string_view vertexSource, std::string_view fragmentSource) {
    const GLuint vertex = CompilePicaGlShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fragment = 0U;
    GLuint program = 0U;
    try {
        fragment = CompilePicaGlShader(GL_FRAGMENT_SHADER, fragmentSource);
        program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            GLint logLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
            std::string log(
                static_cast<size_t>(std::max(logLength, 1)), '\0');
            glGetProgramInfoLog(program, logLength, nullptr, log.data());
            throw std::runtime_error(
                "PICA OpenGL program link failed: " + log);
        }
    } catch (...) {
        if (program != 0U) {
            glDeleteProgram(program);
        }
        glDeleteShader(fragment);
        glDeleteShader(vertex);
        throw;
    }
    glDeleteShader(fragment);
    glDeleteShader(vertex);
    return program;
}

#if defined(__SWITCH__)
// Shader warm-up. The console's Mesa driver exposes no program binary
// formats, so linked programs cannot be stored. Instead every program the
// game links records its translated sources on the SD card, and the next
// launch links them all before the first draw (StartPicaGlWarmup) instead of
// stalling for ~0.1-0.4 s at each first use during play.
constexpr const char* kPicaGlProgramCacheDirectory = "sdmc:/switch/oot3dre/shadercache";
constexpr uint32_t kPicaGlProgramSourceMagic = 0x31534750U; // "PGS1"

struct PicaGlWarmup {
    std::mutex Mutex;
    std::unordered_map<uint64_t, GLuint> Ready;
};

PicaGlWarmup& PicaGlWarmupState() {
    static auto* state = new PicaGlWarmup(); // shared with the warm-up thread; never destroyed
    return *state;
}

uint64_t HashPicaGlProgramKey(std::string_view vertex, std::string_view fragment) {
    uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&](std::string_view text) {
        for (const unsigned char byte : text) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        hash ^= 0xFFU;
        hash *= 1099511628211ULL;
    };
    mix(vertex);
    mix(fragment);
    return hash;
}

std::string PicaGlProgramSourcePath(uint64_t key) {
    char name[32];
    std::snprintf(name, sizeof(name), "/%016llx.src", static_cast<unsigned long long>(key));
    return std::string(kPicaGlProgramCacheDirectory) + name;
}

void RecordPicaGlProgramSource(uint64_t key, std::string_view vertex, std::string_view fragment) {
    const auto path = PicaGlProgramSourcePath(key);
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        return;
    }
    std::filesystem::create_directories(kPicaGlProgramCacheDirectory, error);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const uint32_t header[3] = {kPicaGlProgramSourceMagic, static_cast<uint32_t>(vertex.size()),
                                static_cast<uint32_t>(fragment.size())};
    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    file.write(vertex.data(), static_cast<std::streamsize>(vertex.size()));
    file.write(fragment.data(), static_cast<std::streamsize>(fragment.size()));
    if (!file) {
        file.close();
        std::filesystem::remove(path, error);
    }
}

bool ReadPicaGlProgramSource(const std::filesystem::path& path, std::string& vertex,
                             std::string& fragment) {
    std::ifstream file(path, std::ios::binary);
    uint32_t header[3]{};
    if (!file.read(reinterpret_cast<char*>(header), sizeof(header)) ||
        header[0] != kPicaGlProgramSourceMagic) {
        return false;
    }
    vertex.resize(header[1]);
    fragment.resize(header[2]);
    return static_cast<bool>(file.read(vertex.data(), header[1])) &&
           static_cast<bool>(file.read(fragment.data(), header[2]));
}

// Called once from the render thread, with its context current, before the
// first PICA program is needed. A shared context on another thread would be
// faster but is unsafe here: nvc0 contexts in one share group use the same
// push buffer without a lock, and the main thread's next buffer upload
// crashed in pushbuf_submit while the warm-up thread linked. So the recorded
// programs are linked synchronously, as one pause at start-up instead of
// stalls in play.
void StartPicaGlWarmup() {
    auto& state = PicaGlWarmupState();
    std::error_code error;
    size_t sources = 0U;
    size_t linked = 0U;
    size_t failed = 0U;
    size_t skipped = 0U;
    const auto started = std::chrono::steady_clock::now();
    for (const auto& entry : std::filesystem::directory_iterator(kPicaGlProgramCacheDirectory, error)) {
        if (entry.path().extension() != ".src") {
            continue;
        }
        ++sources;
        // Programs already in Mesa's disk cache link in ~12 ms; one that is not
        // takes 1-3 s. Bound the pause; the rest compile on first use as before.
        if (std::chrono::steady_clock::now() - started > std::chrono::seconds(4)) {
            ++skipped;
            continue;
        }
        uint64_t key = 0U;
        try {
            key = std::stoull(entry.path().stem().string(), nullptr, 16);
        } catch (...) {
            ++failed;
            continue;
        }
        std::string vertex;
        std::string fragment;
        if (!ReadPicaGlProgramSource(entry.path(), vertex, fragment) ||
            HashPicaGlProgramKey(vertex, fragment) != key) {
            ++failed;
            continue;
        }
        try {
            state.Ready.emplace(key, LinkPicaGlProgram(vertex, fragment));
            ++linked;
        } catch (...) {
            ++failed;
        }
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::filesystem::create_directories(kPicaGlProgramCacheDirectory, error);
    GLint binaryFormats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &binaryFormats);
    std::ofstream(std::string(kPicaGlProgramCacheDirectory) + "/status.txt", std::ios::trunc)
        << "binary_formats=" << binaryFormats << "\nwarmup_sources=" << sources << "\nwarmup_linked=" << linked
        << "\nwarmup_failed=" << failed << "\nwarmup_skipped=" << skipped << "\nwarmup_seconds=" << seconds << '\n';
}
#endif

PicaGlProgram BuildPicaGlProgram(const GfxNativePicaDrawView& draw) {
    std::string vertexSource;
    std::string fragmentSource;
    std::string translateError;
    if (!Renderer3ds::TranslatePicaShaderToOpenGl43(
            draw.VertexShaderSource, vertexSource, &translateError)) {
        throw std::runtime_error(
            "PICA vertex shader translation failed: " + translateError);
    }
    if (!Renderer3ds::TranslatePicaShaderToOpenGl43(
            draw.FragmentShaderSource, fragmentSource, &translateError)) {
        throw std::runtime_error(
            "PICA fragment shader translation failed: " + translateError);
    }

#if defined(__SWITCH__)
    // Linking every recorded program up front took minutes on the console
    // (1-3 s per program with the stock Mesa). The warm-up stays available
    // for a Mesa built with its disk cache, behind config/shader_warmup.
    static const bool warmupStarted = [] {
        // A Mesa with its disk cache reports a program binary format; then a
        // recorded program loads in ~12 ms and warming all of them is cheap.
        GLint binaryFormats = 0;
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &binaryFormats);
        std::error_code error;
        if (binaryFormats > 0 ||
            std::filesystem::exists("sdmc:/switch/oot3dre/config/shader_warmup", error)) {
            StartPicaGlWarmup();
        }
        return true;
    }();
    (void)warmupStarted;
    const uint64_t cacheKey = HashPicaGlProgramKey(vertexSource, fragmentSource);
    {
        auto& state = PicaGlWarmupState();
        std::unique_lock lock(state.Mutex);
        if (const auto found = state.Ready.find(cacheKey); found != state.Ready.end()) {
            const GLuint program = found->second;
            state.Ready.erase(found);
            lock.unlock();
            BindPicaGlProgramSamplers(program);
            return {program};
        }
    }
#endif
    const GLuint program = LinkPicaGlProgram(vertexSource, fragmentSource);
    BindPicaGlProgramSamplers(program);
#if defined(__SWITCH__)
    RecordPicaGlProgramSource(cacheKey, vertexSource, fragmentSource);
#endif
    return {program};
}

GLenum ToPicaGlCompare(GfxNativePicaCompareFunction compare) {
    switch (compare) {
        case GfxNativePicaCompareFunction::Never: return GL_NEVER;
        case GfxNativePicaCompareFunction::Always: return GL_ALWAYS;
        case GfxNativePicaCompareFunction::Equal: return GL_EQUAL;
        case GfxNativePicaCompareFunction::NotEqual: return GL_NOTEQUAL;
        case GfxNativePicaCompareFunction::Less: return GL_LESS;
        case GfxNativePicaCompareFunction::LessOrEqual: return GL_LEQUAL;
        case GfxNativePicaCompareFunction::Greater: return GL_GREATER;
        case GfxNativePicaCompareFunction::GreaterOrEqual: return GL_GEQUAL;
    }
    throw std::runtime_error("invalid PICA compare function");
}

GLenum ToPicaGlStencil(GfxNativePicaStencilAction action) {
    switch (action) {
        case GfxNativePicaStencilAction::Keep: return GL_KEEP;
        case GfxNativePicaStencilAction::Zero: return GL_ZERO;
        case GfxNativePicaStencilAction::Replace: return GL_REPLACE;
        case GfxNativePicaStencilAction::Increment: return GL_INCR;
        case GfxNativePicaStencilAction::Decrement: return GL_DECR;
        case GfxNativePicaStencilAction::Invert: return GL_INVERT;
        case GfxNativePicaStencilAction::IncrementWrap: return GL_INCR_WRAP;
        case GfxNativePicaStencilAction::DecrementWrap: return GL_DECR_WRAP;
    }
    throw std::runtime_error("invalid PICA stencil action");
}

GLenum ToPicaGlLogic(GfxNativePicaLogicOperation operation) {
    switch (operation) {
        case GfxNativePicaLogicOperation::Clear: return GL_CLEAR;
        case GfxNativePicaLogicOperation::And: return GL_AND;
        case GfxNativePicaLogicOperation::AndReverse: return GL_AND_REVERSE;
        case GfxNativePicaLogicOperation::Copy: return GL_COPY;
        case GfxNativePicaLogicOperation::Set: return GL_SET;
        case GfxNativePicaLogicOperation::CopyInverted: return GL_COPY_INVERTED;
        case GfxNativePicaLogicOperation::NoOp: return GL_NOOP;
        case GfxNativePicaLogicOperation::Invert: return GL_INVERT;
        case GfxNativePicaLogicOperation::Nand: return GL_NAND;
        case GfxNativePicaLogicOperation::Or: return GL_OR;
        case GfxNativePicaLogicOperation::Nor: return GL_NOR;
        case GfxNativePicaLogicOperation::Xor: return GL_XOR;
        case GfxNativePicaLogicOperation::Equivalent: return GL_EQUIV;
        case GfxNativePicaLogicOperation::AndInverted: return GL_AND_INVERTED;
        case GfxNativePicaLogicOperation::OrReverse: return GL_OR_REVERSE;
        case GfxNativePicaLogicOperation::OrInverted: return GL_OR_INVERTED;
    }
    throw std::runtime_error("invalid PICA logic operation");
}

GLenum ToPicaGlBlendEquation(GfxNativeBlendEquation equation) {
    switch (equation) {
        case GfxNativeBlendEquation::Add: return GL_FUNC_ADD;
        case GfxNativeBlendEquation::Subtract: return GL_FUNC_SUBTRACT;
        case GfxNativeBlendEquation::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
        case GfxNativeBlendEquation::Min: return GL_MIN;
        case GfxNativeBlendEquation::Max: return GL_MAX;
    }
    throw std::runtime_error("invalid PICA blend equation");
}

GLenum ToPicaGlBlendFactor(GfxNativeBlendFactor factor) {
    switch (factor) {
        case GfxNativeBlendFactor::Zero: return GL_ZERO;
        case GfxNativeBlendFactor::One: return GL_ONE;
        case GfxNativeBlendFactor::SourceColor: return GL_SRC_COLOR;
        case GfxNativeBlendFactor::OneMinusSourceColor: return GL_ONE_MINUS_SRC_COLOR;
        case GfxNativeBlendFactor::DestColor: return GL_DST_COLOR;
        case GfxNativeBlendFactor::OneMinusDestColor: return GL_ONE_MINUS_DST_COLOR;
        case GfxNativeBlendFactor::SourceAlpha: return GL_SRC_ALPHA;
        case GfxNativeBlendFactor::OneMinusSourceAlpha: return GL_ONE_MINUS_SRC_ALPHA;
        case GfxNativeBlendFactor::DestAlpha: return GL_DST_ALPHA;
        case GfxNativeBlendFactor::OneMinusDestAlpha: return GL_ONE_MINUS_DST_ALPHA;
        case GfxNativeBlendFactor::ConstantColor: return GL_CONSTANT_COLOR;
        case GfxNativeBlendFactor::OneMinusConstantColor: return GL_ONE_MINUS_CONSTANT_COLOR;
        case GfxNativeBlendFactor::ConstantAlpha: return GL_CONSTANT_ALPHA;
        case GfxNativeBlendFactor::OneMinusConstantAlpha: return GL_ONE_MINUS_CONSTANT_ALPHA;
        case GfxNativeBlendFactor::SourceAlphaSaturate: return GL_SRC_ALPHA_SATURATE;
    }
    throw std::runtime_error("invalid PICA blend factor");
}

GLenum ToPicaGlTopology(GfxNativePicaTopology topology) {
    switch (topology) {
        case GfxNativePicaTopology::TriangleList:
        case GfxNativePicaTopology::GeometryShader:
            return GL_TRIANGLES;
        case GfxNativePicaTopology::TriangleStrip:
            return GL_TRIANGLE_STRIP;
        case GfxNativePicaTopology::TriangleFan:
            return GL_TRIANGLE_FAN;
    }
    throw std::runtime_error("invalid PICA topology");
}

GLenum ToPicaGlVertexType(GfxNativePicaVertexFormat format) {
    switch (format) {
        case GfxNativePicaVertexFormat::SignedByte: return GL_BYTE;
        case GfxNativePicaVertexFormat::UnsignedByte: return GL_UNSIGNED_BYTE;
        case GfxNativePicaVertexFormat::SignedShort: return GL_SHORT;
        case GfxNativePicaVertexFormat::Float: return GL_FLOAT;
    }
    throw std::runtime_error("invalid PICA vertex format");
}

GLenum ToPicaGlWrap(uint8_t wrap) {
    switch (wrap) {
        case 0U: return GL_CLAMP_TO_EDGE;
        case 2U:
        case 6U:
        case 7U: return GL_REPEAT;
        case 3U: return GL_MIRRORED_REPEAT;
        case 1U:
            throw std::runtime_error(
                "PICA clamp-to-border requires border color state");
        case 4U:
        case 5U:
            throw std::runtime_error("PICA asymmetric wrap is unsupported");
        default:
            throw std::runtime_error("invalid PICA texture wrap mode");
    }
}

uint32_t PicaMipCount(const GfxNativePicaTextureView& texture) {
    if (texture.Width == 0U || texture.Height == 0U) {
        return 0U;
    }
    uint32_t width = texture.Width;
    uint32_t height = texture.Height;
    uint32_t levels = 1U;
    while (width > 8U && height > 8U) {
        ++levels;
        width >>= 1U;
        height >>= 1U;
    }
    return std::min(
        levels, static_cast<uint32_t>(texture.MaxMipLevel) + 1U);
}

std::optional<size_t> PicaEncodedMipSize(
    uint8_t format, uint32_t width, uint32_t height) {
    static constexpr std::array<uint8_t, 14> kNibblesPerPixel{
        8U, 6U, 4U, 4U, 4U, 4U, 4U,
        2U, 2U, 2U, 1U, 1U, 1U, 2U};
    if (format >= kNibblesPerPixel.size() || width == 0U || height == 0U) {
        return std::nullopt;
    }
    const uint64_t tiledWidth = (std::max(8U, width) + 7U) & ~7ULL;
    const uint64_t tiledHeight = (std::max(8U, height) + 7U) & ~7ULL;
    const uint64_t bytes =
        (tiledWidth * tiledHeight * kNibblesPerPixel[format] + 1U) / 2U;
    if (bytes > std::numeric_limits<size_t>::max()) {
        return std::nullopt;
    }
    return static_cast<size_t>(bytes);
}

GLint PicaGlMinFilter(const GfxNativePicaTextureView& texture,
                      uint32_t mipLevels) {
    if (mipLevels <= 1U) {
        return texture.MinLinear ? GL_LINEAR : GL_NEAREST;
    }
    if (texture.MipLinear) {
        return texture.MinLinear ? GL_LINEAR_MIPMAP_LINEAR
                                 : GL_NEAREST_MIPMAP_LINEAR;
    }
    return texture.MinLinear ? GL_LINEAR_MIPMAP_NEAREST
                             : GL_NEAREST_MIPMAP_NEAREST;
}

uint32_t PicaColorBytesPerPixel(uint8_t format) {
    return format == 0U ? 4U : (format == 1U ? 3U : 2U);
}

uint32_t PicaDepthBytesPerPixel(uint8_t format) {
    return format == 0U ? 2U : (format == 2U ? 3U : 4U);
}

uint32_t PicaFillElementSize(uint16_t control) {
    return (control & (1U << 9U)) != 0U
        ? 4U
        : ((control & (1U << 8U)) != 0U ? 3U : 2U);
}

uint8_t PicaFillByte(const GfxNativePicaMemoryFillView& fill,
                     uint32_t index) {
    const uint32_t shift =
        (index % PicaFillElementSize(fill.Control)) * 8U;
    return static_cast<uint8_t>(fill.Value >> shift);
}

float PicaUnorm(uint32_t value, uint32_t maximum) {
    return static_cast<float>(value) / static_cast<float>(maximum);
}

std::array<float, 4> DecodePicaClearColor(
    const GfxNativePicaMemoryFillView& fill, uint8_t format) {
    const uint8_t b0 = PicaFillByte(fill, 0U);
    const uint8_t b1 = PicaFillByte(fill, 1U);
    const uint8_t b2 = PicaFillByte(fill, 2U);
    const uint8_t b3 = PicaFillByte(fill, 3U);
    if (format == 0U) {
        return {PicaUnorm(b3, 0xFFU), PicaUnorm(b2, 0xFFU),
                PicaUnorm(b1, 0xFFU), PicaUnorm(b0, 0xFFU)};
    }
    if (format == 1U) {
        return {PicaUnorm(b2, 0xFFU), PicaUnorm(b1, 0xFFU),
                PicaUnorm(b0, 0xFFU), 1.0F};
    }
    const uint16_t packed = static_cast<uint16_t>(b0) |
                            static_cast<uint16_t>(b1) << 8U;
    if (format == 2U) {
        return {PicaUnorm((packed >> 11U) & 0x1FU, 0x1FU),
                PicaUnorm((packed >> 5U) & 0x3FU, 0x3FU),
                PicaUnorm(packed & 0x1FU, 0x1FU), 1.0F};
    }
    if (format == 3U) {
        return {PicaUnorm((packed >> 11U) & 0x1FU, 0x1FU),
                PicaUnorm((packed >> 6U) & 0x1FU, 0x1FU),
                PicaUnorm((packed >> 1U) & 0x1FU, 0x1FU),
                static_cast<float>(packed & 1U)};
    }
    return {PicaUnorm((packed >> 12U) & 0xFU, 0xFU),
            PicaUnorm((packed >> 8U) & 0xFU, 0xFU),
            PicaUnorm((packed >> 4U) & 0xFU, 0xFU),
            PicaUnorm(packed & 0xFU, 0xFU)};
}

std::pair<float, uint8_t> DecodePicaClearDepthStencil(
    const GfxNativePicaMemoryFillView& fill, uint8_t format) {
    const uint32_t b0 = PicaFillByte(fill, 0U);
    const uint32_t b1 = PicaFillByte(fill, 1U);
    const uint32_t b2 = PicaFillByte(fill, 2U);
    const uint32_t depth = format == 0U
        ? b0 | (b1 << 8U)
        : b0 | (b1 << 8U) | (b2 << 16U);
    return {PicaUnorm(depth, format == 0U ? 0xFFFFU : 0xFFFFFFU),
            format == 3U ? PicaFillByte(fill, 3U) : 0U};
}

void ClearPicaShadow(const PicaGlTarget& target, uint32_t value) {
    glBindFramebuffer(GL_FRAMEBUFFER, target.ShadowFramebuffer);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glClearBufferuiv(GL_COLOR, 0, &value);
}

void DeletePicaTarget(PicaGlTarget& target) {
    glDeleteFramebuffers(1, &target.Framebuffer);
    glDeleteFramebuffers(1, &target.ShadowFramebuffer);
    glDeleteTextures(1, &target.Color);
    glDeleteTextures(1, &target.Shadow);
    glDeleteRenderbuffers(1, &target.DepthStencil);
    target = {};
}

void DeletePicaDisplay(PicaGlDisplay& display) {
    glDeleteFramebuffers(1, &display.Framebuffer);
    glDeleteTextures(1, &display.Color);
    display = {};
}

#endif

} // namespace

struct GfxRenderingAPIOGL::NativePicaState {
    std::vector<uint64_t> Completions;
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
    Renderer3ds::PicaCompositionSchedule Composition;
    std::map<PicaGlShaderKey, PicaGlProgram> Programs;
    std::map<PicaGlTargetKey, PicaGlTarget> Targets;
    std::map<PicaGlTextureKey, PicaGlTexture> Textures;
    std::map<uint64_t, PicaGlTexture> LightingLuts;
    std::map<std::pair<uint64_t, uint32_t>, PicaGlDisplay> Displays;
    std::vector<GfxNativePicaMemoryFillView> PendingFills;
    std::optional<GfxNativePicaDisplayTransferView> LastPresented;
    Oot3d::PicaGeometryRegistry GeometryRegistry;
    std::unordered_map<uint64_t, PicaGlGeometryBuffer> GeometryBuffers;
    std::vector<Oot3d::PicaNriSourceVertexStream> GeometrySourceStreams;
    std::vector<Oot3d::PicaNriSourceVertexAttribute> GeometrySourceAttributes;
    std::array<GLuint, 16> VertexBuffers{};
    GLuint VertexArray = 0U;
    GLuint IndexBuffer = 0U;
    GLuint VertexUniformBuffer = 0U;
    GLuint FragmentUniformBuffer = 0U;
    GLuint FallbackColor = 0U;
    GLuint FallbackInteger = 0U;
    GLuint ScanoutProgram = 0U;
    GLint ScanoutFlipLocation = -1;
    uint16_t EnabledAttributeMask = 0U;
    std::array<uint8_t, 16> AttributeDivisors{};
    std::array<PicaGlAttributeState, 16> AttributeStates{};
    bool GeometryCacheEnabled = true;
    uint64_t GeometryPersistentDraws = 0U;
    uint64_t GeometryStreamingDraws = 0U;
    uint64_t GeometryPersistentUploads = 0U;
    uint64_t GeometryPersistentUploadBytes = 0U;
    uint64_t GeometryStreamingUploads = 0U;
    uint64_t GeometryStreamingUploadBytes = 0U;
    uint64_t UniformUploads = 0U;
    uint64_t UniformUploadBytes = 0U;
    uint32_t ResolutionScale = 1U;
    PicaGlPostState Post;
#endif
};

#if !defined(USE_OPENGLES) && !defined(__APPLE__)
namespace {

// GL 4.6 / ARB_texture_filter_anisotropic / EXT_texture_filter_anisotropic
// share these enums; the Switch glad header (GL 4.3) does not define them.
#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#endif

// Init-time only: the extension list is walked once.
bool PicaGlAnisotropySupported() {
    GLint major = 0;
    GLint minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (major > 4 || (major == 4 && minor >= 6)) {
        return true;
    }
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint index = 0; index < count; ++index) {
        const auto* name = reinterpret_cast<const char*>(
            glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(index)));
        if (name != nullptr &&
            (std::strcmp(name, "GL_EXT_texture_filter_anisotropic") == 0 ||
             std::strcmp(name, "GL_ARB_texture_filter_anisotropic") == 0)) {
            return true;
        }
    }
    return false;
}

void DeletePicaGlPostImage(PicaGlPostImage& image) {
    glDeleteFramebuffers(1, &image.Framebuffer);
    glDeleteTextures(1, &image.Texture);
    image = {};
}

void DeletePicaGlPostProgram(PicaGlPostProgram& program) {
    glDeleteProgram(program.Handle);
    program = {};
}

void DeletePicaGlPost(PicaGlPostState& post) {
    DeletePicaGlPostProgram(post.Fxaa);
    DeletePicaGlPostProgram(post.SmaaEdges);
    DeletePicaGlPostProgram(post.SmaaWeights);
    DeletePicaGlPostProgram(post.SmaaBlend);
    DeletePicaGlPostProgram(post.Easu);
    DeletePicaGlPostProgram(post.Rcas);
    glDeleteTextures(1, &post.SmaaArea);
    glDeleteTextures(1, &post.SmaaSearch);
    post.SmaaArea = 0U;
    post.SmaaSearch = 0U;
    DeletePicaGlPostImage(post.Color);
    DeletePicaGlPostImage(post.Edges);
    DeletePicaGlPostImage(post.Weights);
    DeletePicaGlPostImage(post.Upscaled);
}

void SetPicaGlPostSampling(GLenum filter) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    static_cast<GLint>(filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    static_cast<GLint>(filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
}

// Steady state is one comparison; GL work only when the extent changes.
bool EnsurePicaGlPostImage(PicaGlPostImage& image, GLenum format,
                           uint32_t width, uint32_t height) {
    if (image.Texture != 0U && image.Width == width &&
        image.Height == height) {
        return true;
    }
    if (image.Texture == 0U) {
        glGenTextures(1, &image.Texture);
        glGenFramebuffers(1, &image.Framebuffer);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, image.Texture);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format),
                 static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    SetPicaGlPostSampling(GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, image.Framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, image.Texture, 0);
    image.Width = width;
    image.Height = height;
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
           GL_FRAMEBUFFER_COMPLETE;
}

GLuint CreatePicaGlPostLookup(const std::vector<uint8_t>& rgba8,
                              uint32_t width, uint32_t height) {
    GLuint texture = 0U;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width),
                 static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba8.data());
    // Same sampler as the NRI path (SMAA addresses lookup texel centres).
    SetPicaGlPostSampling(GL_LINEAR);
    return texture;
}

PicaGlPostProgram CreatePicaGlPostProgram(
    std::string_view vertexSource, const std::string& fragmentSource,
    std::initializer_list<const char*> parameters) {
    PicaGlPostProgram program;
    program.Handle = LinkPicaGlProgram(vertexSource, fragmentSource);
    program.Orient = glGetUniformLocation(program.Handle, "pica_post_orient");
    program.FlipY = glGetUniformLocation(program.Handle, "pica_flip_y");
    program.OutputSize =
        glGetUniformLocation(program.Handle, "pica_post_output_size");
    size_t index = 0U;
    for (const char* name : parameters) {
        program.Params[index++] = glGetUniformLocation(program.Handle, name);
    }
    return program;
}

void LogPicaGlPost(const std::string& message) {
    std::fprintf(stderr, "[pica-gl] %s\n", message.c_str());
}

// Builds only the programs the configuration needs, at init. A failure
// turns that effect off instead of failing the renderer.
void InitPicaGlPost(PicaGlPostState& post, std::string_view vertexSource) {
    post.Config = ReadPicaGlPostConfig();
    auto& config = post.Config;
    if (config.Anisotropy > 1U) {
        GLfloat maximum = 0.0F;
        if (PicaGlAnisotropySupported()) {
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maximum);
        }
        config.Anisotropy = std::min<uint32_t>(
            config.Anisotropy,
            static_cast<uint32_t>(std::max(maximum, 0.0F)));
        if (config.Anisotropy <= 1U) {
            config.Anisotropy = 0U;
            LogPicaGlPost("filtro anisotropico indisponivel neste driver");
        }
    }
    if (config.Aa == PicaGlPost::Antialiasing::Fxaa) {
        try {
            post.Fxaa = CreatePicaGlPostProgram(
                vertexSource, PicaGlPost::BuildFxaaFragmentShader(),
                {"scanout.inverse_size"});
        } catch (const std::exception& exception) {
            LogPicaGlPost(std::string("FXAA desligado: ") + exception.what());
            config.Aa = PicaGlPost::Antialiasing::Off;
        }
    } else if (config.Aa == PicaGlPost::Antialiasing::Smaa) {
        try {
            const auto lookup = Oot3d::DecodeSmaaLookupData();
            if (!lookup.Valid()) {
                throw std::runtime_error("SMAA lookup data is invalid");
            }
            using Stage = PicaGlPost::SmaaStage;
            post.SmaaEdges = CreatePicaGlPostProgram(
                vertexSource,
                PicaGlPost::BuildSmaaFragmentShader(Stage::EdgeDetection),
                {"pica_smaa_metrics"});
            post.SmaaWeights = CreatePicaGlPostProgram(
                vertexSource,
                PicaGlPost::BuildSmaaFragmentShader(
                    Stage::BlendWeightCalculation),
                {"pica_smaa_metrics"});
            post.SmaaBlend = CreatePicaGlPostProgram(
                vertexSource,
                PicaGlPost::BuildSmaaFragmentShader(
                    Stage::NeighborhoodBlending),
                {"pica_smaa_metrics"});
            post.SmaaArea = CreatePicaGlPostLookup(
                lookup.AreaRgba8, Oot3d::SmaaLookupData::AreaWidth,
                Oot3d::SmaaLookupData::AreaHeight);
            post.SmaaSearch = CreatePicaGlPostLookup(
                lookup.SearchRgba8, Oot3d::SmaaLookupData::SearchWidth,
                Oot3d::SmaaLookupData::SearchHeight);
        } catch (const std::exception& exception) {
            LogPicaGlPost(std::string("SMAA desligado: ") + exception.what());
            DeletePicaGlPostProgram(post.SmaaEdges);
            DeletePicaGlPostProgram(post.SmaaWeights);
            DeletePicaGlPostProgram(post.SmaaBlend);
            glDeleteTextures(1, &post.SmaaArea);
            glDeleteTextures(1, &post.SmaaSearch);
            post.SmaaArea = 0U;
            post.SmaaSearch = 0U;
            config.Aa = PicaGlPost::Antialiasing::Off;
        }
    }
    if (config.Upscale == PicaGlPost::Upscaler::Fsr) {
        try {
            post.Easu = CreatePicaGlPostProgram(
                vertexSource, PicaGlPost::BuildEasuFragmentShader(),
                {"pica_easu_con0", "pica_easu_con1", "pica_easu_con2",
                 "pica_easu_con3"});
        } catch (const std::exception& exception) {
            LogPicaGlPost(std::string("FSR EASU desligado: ") +
                          exception.what());
            config.Upscale = PicaGlPost::Upscaler::Linear;
        }
    }
    if (config.Sharpen > 0.0F) {
        try {
            post.Rcas = CreatePicaGlPostProgram(
                vertexSource, PicaGlPost::BuildRcasFragmentShader(),
                {"pica_rcas_con", "pica_rcas_resample"});
            const auto con = PicaGlPost::FsrRcasConstants(config.Sharpen);
            glUseProgram(post.Rcas.Handle);
            glUniform4ui(post.Rcas.Params[0], con[0], con[1], con[2], con[3]);
        } catch (const std::exception& exception) {
            LogPicaGlPost(std::string("RCAS desligado: ") + exception.what());
            config.Sharpen = 0.0F;
        }
    }
    if (config.ScanoutEffects() || config.Anisotropy != 0U) {
        static constexpr std::array<const char*, 3> kAa{"off", "fxaa", "smaa"};
        char summary[160];
        std::snprintf(summary, sizeof(summary),
                      "pos-processamento: aa=%s upscaler=%s sharpen=%.2f "
                      "anisotropia=%u",
                      kAa[static_cast<size_t>(config.Aa)],
                      config.Upscale == PicaGlPost::Upscaler::Fsr ? "fsr"
                                                                  : "linear",
                      static_cast<double>(config.Sharpen),
                      static_cast<unsigned>(config.Anisotropy));
        LogPicaGlPost(summary);
    }
}

uint64_t PicaGlPostExtentKey(uint32_t width, uint32_t height) {
    return (static_cast<uint64_t>(width) << 32U) | height;
}

// Uploads the destination description of the bound post program when it
// changed.
void SetPicaGlPostDestination(PicaGlPostProgram& program, bool window,
                              bool flip, uint32_t width, uint32_t height) {
    const int orient = window ? 1 : 0;
    if (program.LastOrient != orient) {
        glUniform1i(program.Orient, orient);
        program.LastOrient = orient;
    }
    const int flipY = window && flip ? 1 : 0;
    if (program.LastFlipY != flipY) {
        glUniform1i(program.FlipY, flipY);
        program.LastFlipY = flipY;
    }
    const uint64_t key = PicaGlPostExtentKey(width, height);
    if (program.LastOutputKey != key) {
        glUniform2f(program.OutputSize, static_cast<float>(width),
                    static_cast<float>(height));
        program.LastOutputKey = key;
    }
}

void DrawPicaGlPostTriangle(GLuint source) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void BeginPicaGlPostImage(const PicaGlPostImage& image) {
    glBindFramebuffer(GL_FRAMEBUFFER, image.Framebuffer);
    glViewport(0, 0, static_cast<GLsizei>(image.Width),
               static_cast<GLsizei>(image.Height));
}

void SetPicaGlSmaaMetrics(PicaGlPostProgram& program, uint32_t width,
                          uint32_t height) {
    const uint64_t key = PicaGlPostExtentKey(width, height);
    if (program.LastInputKey != key) {
        glUniform4f(program.Params[0], 1.0F / static_cast<float>(width),
                    1.0F / static_cast<float>(height),
                    static_cast<float>(width), static_cast<float>(height));
        program.LastInputKey = key;
    }
}

void DrawPicaGlFxaa(PicaGlPostProgram& program, GLuint source,
                    uint32_t sourceWidth, uint32_t sourceHeight, bool window,
                    bool flip, uint32_t outputWidth, uint32_t outputHeight) {
    glUseProgram(program.Handle);
    SetPicaGlPostDestination(program, window, flip, outputWidth, outputHeight);
    const uint64_t key = PicaGlPostExtentKey(sourceWidth, sourceHeight);
    if (program.LastInputKey != key) {
        glUniform2f(program.Params[0], 1.0F / static_cast<float>(sourceWidth),
                    1.0F / static_cast<float>(sourceHeight));
        program.LastInputKey = key;
    }
    DrawPicaGlPostTriangle(source);
}

void DrawPicaGlEasu(PicaGlPostProgram& program, GLuint source,
                    uint32_t sourceWidth, uint32_t sourceHeight, bool window,
                    bool flip, uint32_t outputWidth, uint32_t outputHeight) {
    glUseProgram(program.Handle);
    SetPicaGlPostDestination(program, window, flip, outputWidth, outputHeight);
    // Extents are below 2^16, so this packs both pairs without collisions.
    const uint64_t key =
        (static_cast<uint64_t>(sourceWidth) << 48U) |
        (static_cast<uint64_t>(sourceHeight) << 32U) |
        (static_cast<uint64_t>(outputWidth) << 16U) | outputHeight;
    if (program.LastInputKey != key) {
        const auto con = PicaGlPost::FsrEasuConstants(
            static_cast<float>(sourceWidth), static_cast<float>(sourceHeight),
            static_cast<float>(outputWidth), static_cast<float>(outputHeight));
        for (size_t index = 0U; index < 4U; ++index) {
            glUniform4ui(program.Params[index], con[index * 4U],
                         con[index * 4U + 1U], con[index * 4U + 2U],
                         con[index * 4U + 3U]);
        }
        program.LastInputKey = key;
    }
    DrawPicaGlPostTriangle(source);
}

// Runs the configured chain for one Replace-mode scanout and draws its last
// pass into the window viewport. The caller has prepared the window (cleared,
// blend/depth/stencil/scissor off) and bound the VAO. Steady-state cost per
// pass: one FBO bind, viewport, program, texture bind and draw; no
// allocation, no queries.
void PresentPicaGlPost(GfxRenderingAPIOGL::NativePicaState& state,
                       const PicaGlDisplay& display, bool flip,
                       GLint viewportX, GLint viewportY,
                       GLsizei viewportWidth, GLsizei viewportHeight) {
    auto& post = state.Post;
    auto& config = post.Config;
    const uint32_t sourceWidth = display.Width * state.ResolutionScale;
    const uint32_t sourceHeight = display.Height * state.ResolutionScale;
    // The window viewport, in the portrait layout of the source image.
    const uint32_t outputWidth = static_cast<uint32_t>(viewportHeight);
    const uint32_t outputHeight = static_cast<uint32_t>(viewportWidth);

    // EASU only enlarges; at 1:1 (e.g. scale 3 on the 720p handheld screen)
    // FSR reduces to RCAS.
    const bool easu = config.Upscale == PicaGlPost::Upscaler::Fsr &&
                      (outputWidth > sourceWidth || outputHeight > sourceHeight);
    const bool rcas = config.Sharpen > 0.0F;
    // FXAA alone near 1:1 samples the source from the window pass, exactly
    // like the PC scanout. When the window has at least twice the source's
    // pixels (scale 1-2, or docked), FXAA at source size plus the plain
    // scanout is cheaper (measured), and it resolves into an image first.
    const uint64_t sourcePixels =
        static_cast<uint64_t>(sourceWidth) * sourceHeight;
    const uint64_t outputPixels =
        static_cast<uint64_t>(outputWidth) * outputHeight;
    const bool fxaaInWindow =
        config.Aa == PicaGlPost::Antialiasing::Fxaa && !easu && !rcas &&
        outputPixels < sourcePixels * 2U;

    GLuint current = display.Color;
    uint32_t currentWidth = sourceWidth;
    uint32_t currentHeight = sourceHeight;
    bool complete = true;

    if (config.Aa == PicaGlPost::Antialiasing::Smaa) {
        complete = EnsurePicaGlPostImage(post.Edges, GL_RG8, sourceWidth,
                                         sourceHeight) &&
                   EnsurePicaGlPostImage(post.Weights, GL_RGBA8, sourceWidth,
                                         sourceHeight) &&
                   EnsurePicaGlPostImage(post.Color, GL_RGBA8, sourceWidth,
                                         sourceHeight);
        if (complete) {
            BeginPicaGlPostImage(post.Edges);
            // The official edge pass discards edge-free pixels.
            glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
            glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(post.SmaaEdges.Handle);
            SetPicaGlSmaaMetrics(post.SmaaEdges, sourceWidth, sourceHeight);
            DrawPicaGlPostTriangle(current);

            BeginPicaGlPostImage(post.Weights);
            glUseProgram(post.SmaaWeights.Handle);
            SetPicaGlSmaaMetrics(post.SmaaWeights, sourceWidth, sourceHeight);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, post.SmaaArea);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, post.SmaaSearch);
            DrawPicaGlPostTriangle(post.Edges.Texture);

            BeginPicaGlPostImage(post.Color);
            glUseProgram(post.SmaaBlend.Handle);
            SetPicaGlSmaaMetrics(post.SmaaBlend, sourceWidth, sourceHeight);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, post.Weights.Texture);
            DrawPicaGlPostTriangle(current);
            current = post.Color.Texture;
        }
    } else if (config.Aa == PicaGlPost::Antialiasing::Fxaa && !fxaaInWindow) {
        complete = EnsurePicaGlPostImage(post.Color, GL_RGBA8, sourceWidth,
                                         sourceHeight);
        if (complete) {
            BeginPicaGlPostImage(post.Color);
            DrawPicaGlFxaa(post.Fxaa, current, sourceWidth, sourceHeight,
                           false, flip, sourceWidth, sourceHeight);
            current = post.Color.Texture;
        }
    }
    if (complete && easu && rcas) {
        complete = EnsurePicaGlPostImage(post.Upscaled, GL_RGBA8, outputWidth,
                                         outputHeight);
        if (complete) {
            BeginPicaGlPostImage(post.Upscaled);
            DrawPicaGlEasu(post.Easu, current, currentWidth, currentHeight,
                           false, flip, outputWidth, outputHeight);
            current = post.Upscaled.Texture;
            currentWidth = outputWidth;
            currentHeight = outputHeight;
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
    if (!complete) {
        // An intermediate image could not be attached: keep the plain
        // scanout from now on rather than a broken chain.
        LogPicaGlPost("imagem intermediaria incompleta; pos-processamento "
                      "desligado");
        config.Aa = PicaGlPost::Antialiasing::Off;
        config.Upscale = PicaGlPost::Upscaler::Linear;
        config.Sharpen = 0.0F;
        current = display.Color;
    } else if (rcas) {
        glUseProgram(post.Rcas.Handle);
        SetPicaGlPostDestination(post.Rcas, true, flip, outputWidth,
                                 outputHeight);
        const int resample =
            currentWidth != outputWidth || currentHeight != outputHeight ? 1
                                                                         : 0;
        if (post.Rcas.LastResample != resample) {
            glUniform1i(post.Rcas.Params[1], resample);
            post.Rcas.LastResample = resample;
        }
        DrawPicaGlPostTriangle(current);
        return;
    } else if (easu) {
        DrawPicaGlEasu(post.Easu, current, currentWidth, currentHeight, true,
                       flip, outputWidth, outputHeight);
        return;
    } else if (fxaaInWindow) {
        DrawPicaGlFxaa(post.Fxaa, current, currentWidth, currentHeight, true,
                       flip, outputWidth, outputHeight);
        return;
    }
    // AA alone resolved into an image (or the fallback): the plain scanout
    // of the current image.
    glUseProgram(state.ScanoutProgram);
    if (state.ScanoutFlipLocation >= 0) {
        glUniform1i(state.ScanoutFlipLocation, flip ? 1 : 0);
    }
    DrawPicaGlPostTriangle(current);
}

} // namespace
#endif

GfxRenderingAPIOGL::GfxRenderingAPIOGL() = default;

GfxRenderingAPIOGL::~GfxRenderingAPIOGL() {
    ShutdownNativePica();
}

void GfxRenderingAPIOGL::InitNativePica() {
    ShutdownNativePica();
    mNativePica = std::make_unique<NativePicaState>();
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
    auto& state = *mNativePica;
    state.GeometryCacheEnabled = mNativePicaGeometryCacheEnabled;
    state.ResolutionScale = ReadPicaGlResolutionScale();
    if (state.ResolutionScale > 1U) {
        // Keep the largest PICA target (1024 per axis) within the driver's
        // limits; otherwise fall back to a smaller scale.
        GLint maxTexture = 0;
        GLint maxRenderbuffer = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture);
        glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRenderbuffer);
        const GLint limit = std::min(maxTexture, maxRenderbuffer);
        while (state.ResolutionScale > 1U &&
               static_cast<GLint>(1024U * state.ResolutionScale) > limit) {
            --state.ResolutionScale;
        }
    }
    glGenVertexArrays(1, &state.VertexArray);
    glGenBuffers(static_cast<GLsizei>(state.VertexBuffers.size()),
                 state.VertexBuffers.data());
    glGenBuffers(1, &state.IndexBuffer);
    glGenBuffers(1, &state.VertexUniformBuffer);
    glGenBuffers(1, &state.FragmentUniformBuffer);

    const std::array<uint8_t, 4> white{255U, 255U, 255U, 255U};
    glGenTextures(1, &state.FallbackColor);
    glBindTexture(GL_TEXTURE_2D, state.FallbackColor);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, white.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    const uint32_t emptyShadow = 0xFFFFFFFFU;
    glGenTextures(1, &state.FallbackInteger);
    glBindTexture(GL_TEXTURE_2D, state.FallbackInteger);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, 1, 1, 0, GL_RED_INTEGER,
                 GL_UNSIGNED_INT, &emptyShadow);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    static constexpr std::string_view kScanoutVertex = R"glsl(#version 430
out vec2 pica_uv;
void main() {
    const vec2 positions[3] = vec2[3](
        vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 position = positions[gl_VertexID];
    gl_Position = vec4(position, 0.0, 1.0);
    pica_uv = position * 0.5 + 0.5;
}
)glsl";
    static constexpr std::string_view kScanoutFragment = R"glsl(#version 430
layout(binding=0) uniform sampler2D pica_scanout;
uniform int pica_flip_y;
in vec2 pica_uv;
layout(location=0) out vec4 pica_color;
void main() {
    // CTR display-transfer images use the physical portrait layout. OpenGL's
    // framebuffer origin is the opposite of Vulkan's, so compensate that
    // origin before applying the same portrait-to-landscape transform.
    vec2 uv = vec2(pica_uv.y, pica_uv.x);
    if (pica_flip_y != 0) uv.y = 1.0 - uv.y;
    pica_color = texture(pica_scanout, uv);
}
)glsl";
    const GLuint vertex = CompilePicaGlShader(GL_VERTEX_SHADER, kScanoutVertex);
    GLuint fragment = 0U;
    try {
        fragment = CompilePicaGlShader(GL_FRAGMENT_SHADER, kScanoutFragment);
        state.ScanoutProgram = glCreateProgram();
        glAttachShader(state.ScanoutProgram, vertex);
        glAttachShader(state.ScanoutProgram, fragment);
        glLinkProgram(state.ScanoutProgram);
        GLint linked = GL_FALSE;
        glGetProgramiv(state.ScanoutProgram, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            throw std::runtime_error("PICA OpenGL scanout program failed to link");
        }
        glUseProgram(state.ScanoutProgram);
        const GLint sampler =
            glGetUniformLocation(state.ScanoutProgram, "pica_scanout");
        if (sampler >= 0) {
            glUniform1i(sampler, 0);
        }
        state.ScanoutFlipLocation =
            glGetUniformLocation(state.ScanoutProgram, "pica_flip_y");
    } catch (...) {
        glDeleteShader(fragment);
        glDeleteShader(vertex);
        ShutdownNativePica();
        throw;
    }
    glDeleteShader(fragment);
    glDeleteShader(vertex);
    // Optional scanout effects and anisotropy; nothing is created when the
    // configuration files are absent.
    InitPicaGlPost(state.Post, kScanoutVertex);
#endif
}

void GfxRenderingAPIOGL::ShutdownNativePica() noexcept {
    if (!mNativePica) {
        return;
    }
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
    auto& state = *mNativePica;
    for (auto& [key, program] : state.Programs) {
        glDeleteProgram(program.Handle);
    }
    for (auto& [key, target] : state.Targets) {
        DeletePicaTarget(target);
    }
    for (auto& [key, texture] : state.Textures) {
        glDeleteTextures(1, &texture.Handle);
    }
    for (auto& [key, texture] : state.LightingLuts) {
        glDeleteTextures(1, &texture.Handle);
    }
    for (auto& [key, display] : state.Displays) {
        DeletePicaDisplay(display);
    }
    for (auto& [identity, geometry] : state.GeometryBuffers) {
        glDeleteBuffers(1, &geometry.Handle);
    }
    DeletePicaGlPost(state.Post);
    glDeleteProgram(state.ScanoutProgram);
    glDeleteTextures(1, &state.FallbackColor);
    glDeleteTextures(1, &state.FallbackInteger);
    glDeleteBuffers(static_cast<GLsizei>(state.VertexBuffers.size()),
                    state.VertexBuffers.data());
    glDeleteBuffers(1, &state.IndexBuffer);
    glDeleteBuffers(1, &state.VertexUniformBuffer);
    glDeleteBuffers(1, &state.FragmentUniformBuffer);
    glDeleteVertexArrays(1, &state.VertexArray);
#endif
    mNativePica.reset();
}

void GfxRenderingAPIOGL::InvalidateLegacyStateAfterPica() noexcept {
    mLastLoadedShader = nullptr;
    mCurrentArrayBuffer = 0U;
    mLastActiveTexture = -1;
    mLastBlendEnabled = -1;
    mLastScissorEnabled = -1;
    mLastDepthTest = -1;
    mLastDepthMask = -1;
    mLastZmodeDecal = -1;
    std::fill(std::begin(mLastBoundTextures),
              std::end(mLastBoundTextures), 0U);
}

bool GfxRenderingAPIOGL::PublishPicaCompositionSequence(
    const Renderer3ds::PicaCompositionSequenceView& sequence,
    std::string* error) {
#if defined(USE_OPENGLES) || defined(__APPLE__)
    SetPicaGlError(error, "native PICA requires OpenGL 4.3");
    return false;
#else
    if (!mNativePica) {
        SetPicaGlError(error, "PICA OpenGL backend is not initialized");
        return false;
    }
    if (mNativePica->Composition.Valid()) {
        const auto previous = mNativePica->Composition.Stats();
        if (previous.ConsumedDrawCount != previous.DrawCount) {
            SetPicaGlError(
                error, "previous PICA composition sequence was not consumed");
            return false;
        }
        if (mNativePica->Composition.SequenceId() == sequence.SequenceId) {
            SetPicaGlError(error, "PICA composition identity was reused");
            return false;
        }
    }
    Renderer3ds::PicaCompositionSchedule compiled;
    if (!compiled.Compile(sequence, error)) {
        return false;
    }
    mNativePica->Composition = std::move(compiled);
    return true;
#endif
}

#if !defined(USE_OPENGLES) && !defined(__APPLE__)
namespace {

void DeletePicaGlGeometry(
    GfxRenderingAPIOGL::NativePicaState& state, uint64_t identity) {
    const auto found = state.GeometryBuffers.find(identity);
    if (found == state.GeometryBuffers.end()) {
        return;
    }
    const GLuint handle = found->second.Handle;
    for (auto& attribute : state.AttributeStates) {
        if (attribute.Buffer == handle) {
            attribute = {};
        }
    }
    glDeleteBuffers(1, &found->second.Handle);
    state.GeometryBuffers.erase(found);
}

PicaGlTarget& GetPicaGlTarget(
    GfxRenderingAPIOGL::NativePicaState& state,
    const GfxNativePicaDrawView& draw) {
    const PicaGlTargetKey key{
        draw.RenderTargetNamespace,
        draw.FramebufferColorPhysicalAddress,
        draw.FramebufferDepthPhysicalAddress,
        draw.FramebufferWidth,
        draw.FramebufferHeight,
        draw.FramebufferColorFormat,
        draw.FramebufferDepthFormat,
    };
    auto [found, inserted] = state.Targets.try_emplace(key);
    if (!inserted) {
        return found->second;
    }
    auto& target = found->second;
    target.Width = static_cast<uint32_t>(key.Width) * state.ResolutionScale;
    target.Height = static_cast<uint32_t>(key.Height) * state.ResolutionScale;
    try {
        glGenTextures(1, &target.Color);
        glBindTexture(GL_TEXTURE_2D, target.Color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     static_cast<GLsizei>(target.Width),
                     static_cast<GLsizei>(target.Height), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glGenRenderbuffers(1, &target.DepthStencil);
        glBindRenderbuffer(GL_RENDERBUFFER, target.DepthStencil);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                              static_cast<GLsizei>(target.Width),
                              static_cast<GLsizei>(target.Height));

        glGenFramebuffers(1, &target.Framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, target.Framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, target.Color, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, target.DepthStencil);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
            throw std::runtime_error("PICA OpenGL render target is incomplete");
        }

        glGenTextures(1, &target.Shadow);
        glBindTexture(GL_TEXTURE_2D, target.Shadow);
        // The shadow producer addresses this image with gl_FragCoord and the
        // consumer filters it through textureSize(), so both follow the scale.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI,
                     static_cast<GLsizei>(target.Width),
                     static_cast<GLsizei>(target.Height), 0,
                     GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &target.ShadowFramebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, target.ShadowFramebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, target.Shadow, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
            throw std::runtime_error("PICA OpenGL shadow target is incomplete");
        }
        ClearPicaShadow(target, 0xFFFFFFFFU);

        glBindFramebuffer(GL_FRAMEBUFFER, target.Framebuffer);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFFU);
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClearDepth(1.0);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                GL_STENCIL_BUFFER_BIT);
    } catch (...) {
        DeletePicaTarget(target);
        state.Targets.erase(found);
        throw;
    }
    return target;
}

PicaGlProgram& GetPicaGlProgram(
    GfxRenderingAPIOGL::NativePicaState& state,
    const GfxNativePicaDrawView& draw) {
    auto vertexIdentity = draw.VertexShaderSourceIdentity;
    auto fragmentIdentity = draw.FragmentShaderSourceIdentity;
    if (!vertexIdentity.Available()) {
        vertexIdentity =
            Renderer3ds::IdentifyPicaShaderSource(draw.VertexShaderSource);
    }
    if (!fragmentIdentity.Available()) {
        fragmentIdentity =
            Renderer3ds::IdentifyPicaShaderSource(draw.FragmentShaderSource);
    }
    const PicaGlShaderKey key{vertexIdentity, fragmentIdentity};
    auto found = state.Programs.find(key);
    if (found != state.Programs.end()) {
        return found->second;
    }

    // The frontend publishes stable source identities with every draw. Hash
    // the complete GLSL text only while creating a new program; repeating
    // that work on every cache hit made shader lookup scale with source size
    // instead of draw count. The cache-miss check retains the rejecting
    // identity contract before any program is compiled.
    if (draw.VertexShaderSourceIdentity.Available() &&
        Renderer3ds::IdentifyPicaShaderSource(draw.VertexShaderSource) !=
            vertexIdentity) {
        throw std::runtime_error("PICA vertex source identity mismatch");
    }
    if (draw.FragmentShaderSourceIdentity.Available() &&
        Renderer3ds::IdentifyPicaShaderSource(draw.FragmentShaderSource) !=
            fragmentIdentity) {
        throw std::runtime_error("PICA fragment source identity mismatch");
    }
    return state.Programs.emplace(key, BuildPicaGlProgram(draw)).first->second;
}

GLuint FindLivePicaShadow(
    const GfxRenderingAPIOGL::NativePicaState& state,
    const GfxNativePicaTextureView& texture) {
    for (const auto& [key, target] : state.Targets) {
        if (key.ColorAddress == texture.PhysicalAddress &&
            key.Width == texture.Width && key.Height == texture.Height) {
            return target.Shadow;
        }
    }
    return 0U;
}

PicaGlTexture& GetPicaGlTexture(
    GfxRenderingAPIOGL::NativePicaState& state,
    const GfxNativePicaTextureView& texture) {
    const uint64_t contentHash = texture.NativeContentHashAvailable
        ? texture.NativeContentHash
        : HashBytes(texture.NativeBytes);
    const PicaGlTextureKey key{
        contentHash, texture.PhysicalAddress, texture.Width, texture.Height,
        texture.NativeFormat, texture.NativeType, texture.NativeWrapS,
        texture.NativeWrapT, texture.MinLinear, texture.MagLinear,
        texture.MipLinear, texture.LodBiasRaw, texture.MinMipLevel,
        texture.MaxMipLevel,
    };
    auto [found, inserted] = state.Textures.try_emplace(key);
    if (!inserted) {
        return found->second;
    }
    auto& result = found->second;
    try {
        glGenTextures(1, &result.Handle);
        glBindTexture(GL_TEXTURE_2D, result.Handle);
        if (texture.NativeType == 2U) {
            std::vector<uint32_t> pixels;
            std::string decodeError;
            if (!Oot3d::DetilePicaShadow2d(
                    texture.NativeBytes, texture.Width, texture.Height,
                    pixels, &decodeError)) {
                throw std::runtime_error(
                    "PICA Shadow2D decode failed: " + decodeError);
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, texture.Width,
                         texture.Height, 0, GL_RED_INTEGER, GL_UNSIGNED_INT,
                         pixels.data());
            result.Format = GL_R32UI;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        } else {
            const uint32_t mipLevels = PicaMipCount(texture);
            if (mipLevels == 0U) {
                throw std::runtime_error("PICA texture has no mip levels");
            }
            size_t nativeOffset = 0U;
            for (uint32_t level = 0U; level < mipLevels; ++level) {
                const uint32_t width =
                    std::max(1U, static_cast<uint32_t>(texture.Width) >> level);
                const uint32_t height =
                    std::max(1U, static_cast<uint32_t>(texture.Height) >> level);
                const auto encoded = PicaEncodedMipSize(
                    texture.NativeFormat, width, height);
                if (!encoded.has_value() ||
                    nativeOffset + *encoded > texture.NativeBytes.size()) {
                    throw std::runtime_error("PICA texture mip chain is truncated");
                }
                std::vector<uint8_t> rgba8;
                std::string decodeError;
                if (!::Oot3d::Renderer::DecodePicaTextureRgba8(
                        texture.NativeFormat, static_cast<uint16_t>(width),
                        static_cast<uint16_t>(height),
                        texture.NativeBytes.subspan(nativeOffset, *encoded),
                        rgba8, &decodeError)) {
                    throw std::runtime_error(
                        "PICA texture decode failed: " + decodeError);
                }
                glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level),
                             GL_RGBA8, static_cast<GLsizei>(width),
                             static_cast<GLsizei>(height), 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, rgba8.data());
                nativeOffset += *encoded;
            }
            result.Format = GL_RGBA8;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            PicaGlMinFilter(texture, mipLevels));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                            texture.MagLinear ? GL_LINEAR : GL_NEAREST);
            // Optional anisotropic filtering, set once per texture object:
            // only for a linear minification filter over a real mip chain,
            // so NEAREST and unmipmapped sampling keep the PICA result.
            if (state.Post.Config.Anisotropy > 1U && texture.MinLinear &&
                mipLevels > 1U &&
                std::min<uint32_t>(texture.MaxMipLevel, mipLevels - 1U) >
                    texture.MinMipLevel) {
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                                static_cast<GLfloat>(
                                    state.Post.Config.Anisotropy));
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL,
                            texture.MinMipLevel);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                            std::min<uint32_t>(texture.MaxMipLevel,
                                               mipLevels - 1U));
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS,
                            static_cast<float>(texture.LodBiasRaw) / 256.0F);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        ToPicaGlWrap(texture.NativeWrapS));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        ToPicaGlWrap(texture.NativeWrapT));
    } catch (...) {
        glDeleteTextures(1, &result.Handle);
        state.Textures.erase(found);
        throw;
    }
    return result;
}

GLuint GetPicaLightingLut(
    GfxRenderingAPIOGL::NativePicaState& state,
    const Renderer3ds::PicaLightingLutView& lut) {
    if (!lut.Valid()) {
        throw std::runtime_error("PICA lighting LUT is invalid");
    }
    auto [found, inserted] = state.LightingLuts.try_emplace(lut.ContentHash);
    if (!inserted) {
        return found->second.Handle;
    }
    auto& texture = found->second;
    glGenTextures(1, &texture.Handle);
    glBindTexture(GL_TEXTURE_2D, texture.Handle);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, 256, 24, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_INT, lut.PackedEntries.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    texture.Format = GL_R32UI;
    return texture.Handle;
}

void ApplyPicaFill(const GfxNativePicaMemoryFillView& fill,
                   const PicaGlTargetKey& key,
                   const PicaGlTarget& target) {
    const uint64_t pixels = static_cast<uint64_t>(key.Width) * key.Height;
    const uint64_t colorEnd = static_cast<uint64_t>(key.ColorAddress) +
                              pixels * PicaColorBytesPerPixel(key.ColorFormat);
    const uint64_t depthEnd = static_cast<uint64_t>(key.DepthAddress) +
                              pixels * PicaDepthBytesPerPixel(key.DepthFormat);
    const bool color = fill.StartPhysicalAddress <= key.ColorAddress &&
                       fill.EndPhysicalAddress >= colorEnd;
    const bool depth = key.DepthAddress != 0U &&
                       fill.StartPhysicalAddress <= key.DepthAddress &&
                       fill.EndPhysicalAddress >= depthEnd;
    if (!color && !depth) {
        return;
    }
    if (color) {
        const uint32_t shadow =
            static_cast<uint32_t>(PicaFillByte(fill, 0U)) |
            (static_cast<uint32_t>(PicaFillByte(fill, 1U)) << 8U) |
            (static_cast<uint32_t>(PicaFillByte(fill, 2U)) << 16U) |
            (static_cast<uint32_t>(PicaFillByte(fill, 3U)) << 24U);
        ClearPicaShadow(target, shadow);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target.Framebuffer);
    glDisable(GL_SCISSOR_TEST);
    GLbitfield mask = 0U;
    if (color) {
        const auto decoded = DecodePicaClearColor(fill, key.ColorFormat);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(decoded[0], decoded[1], decoded[2], decoded[3]);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (depth) {
        const auto [depthValue, stencil] =
            DecodePicaClearDepthStencil(fill, key.DepthFormat);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFFU);
        glClearDepth(depthValue);
        glClearStencil(stencil);
        mask |= GL_DEPTH_BUFFER_BIT;
        if (key.DepthFormat == 3U) {
            mask |= GL_STENCIL_BUFFER_BIT;
        }
    }
    glClear(mask);
}

void ApplyPendingPicaFills(GfxRenderingAPIOGL::NativePicaState& state,
                           const PicaGlTargetKey& key,
                           const PicaGlTarget& target) {
    auto fill = state.PendingFills.begin();
    while (fill != state.PendingFills.end()) {
        if (fill->RenderTargetNamespace != key.Namespace) {
            ++fill;
            continue;
        }
        const uint64_t pixels = static_cast<uint64_t>(key.Width) * key.Height;
        const uint64_t colorEnd = static_cast<uint64_t>(key.ColorAddress) +
                                  pixels * PicaColorBytesPerPixel(key.ColorFormat);
        const uint64_t depthEnd = static_cast<uint64_t>(key.DepthAddress) +
                                  pixels * PicaDepthBytesPerPixel(key.DepthFormat);
        const bool covers =
            (fill->StartPhysicalAddress <= key.ColorAddress &&
             fill->EndPhysicalAddress >= colorEnd) ||
            (key.DepthAddress != 0U &&
             fill->StartPhysicalAddress <= key.DepthAddress &&
             fill->EndPhysicalAddress >= depthEnd);
        if (!covers) {
            ++fill;
            continue;
        }
        ApplyPicaFill(*fill, key, target);
        fill = state.PendingFills.erase(fill);
    }
}

PicaGlDisplay& GetPicaGlDisplay(
    GfxRenderingAPIOGL::NativePicaState& state,
    uint64_t targetNamespace, uint32_t outputAddress,
    uint32_t width, uint32_t height) {
    const auto key = std::make_pair(targetNamespace, outputAddress);
    auto [found, inserted] = state.Displays.try_emplace(key);
    auto& display = found->second;
    if (!inserted && display.Width == width && display.Height == height) {
        return display;
    }
    if (!inserted) {
        DeletePicaDisplay(display);
    }
    glGenTextures(1, &display.Color);
    glBindTexture(GL_TEXTURE_2D, display.Color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 static_cast<GLsizei>(width * state.ResolutionScale),
                 static_cast<GLsizei>(height * state.ResolutionScale), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &display.Framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, display.Framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, display.Color, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
        GL_FRAMEBUFFER_COMPLETE) {
        DeletePicaDisplay(display);
        throw std::runtime_error("PICA OpenGL display image is incomplete");
    }
    display.Width = width;
    display.Height = height;
    return display;
}

} // namespace
#endif

GfxNativePicaBackendStats
GfxRenderingAPIOGL::GetNativePicaBackendStats() const noexcept {
    GfxNativePicaBackendStats result;
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
    if (mNativePica != nullptr) {
        result.Available = true;
        const auto registry = mNativePica->GeometryRegistry.Stats();
        result.GeometryCacheEnabled = mNativePica->GeometryCacheEnabled;
        result.GeometryRegistryHits = registry.Hits;
        result.GeometryRegistryMisses = registry.Misses;
        result.GeometryRegistryUpdates = registry.Updates;
        result.GeometryRegistryEvictions = registry.Evictions;
        result.GeometryRegistryEntries = registry.Entries;
        result.GeometryPersistentDraws =
            mNativePica->GeometryPersistentDraws;
        result.GeometryStreamingDraws = mNativePica->GeometryStreamingDraws;
        result.GeometryPersistentUploads =
            mNativePica->GeometryPersistentUploads;
        result.GeometryPersistentUploadBytes =
            mNativePica->GeometryPersistentUploadBytes;
        result.GeometryStreamingUploads =
            mNativePica->GeometryStreamingUploads;
        result.GeometryStreamingUploadBytes =
            mNativePica->GeometryStreamingUploadBytes;
        result.UniformUploads = mNativePica->UniformUploads;
        result.UniformUploadBytes = mNativePica->UniformUploadBytes;
    }
#endif
    return result;
}

void GfxRenderingAPIOGL::SetNativePicaGeometryCacheEnabled(
    bool enabled) noexcept {
    mNativePicaGeometryCacheEnabled = enabled;
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
    if (mNativePica != nullptr) {
        mNativePica->GeometryCacheEnabled = enabled;
    }
#else
    static_cast<void>(enabled);
#endif
}

bool GfxRenderingAPIOGL::SubmitPicaDraw(
    const GfxNativePicaDrawView& draw, std::string* error) {
#if defined(USE_OPENGLES) || defined(__APPLE__)
    SetPicaGlError(error, "native PICA requires OpenGL 4.3");
    return false;
#else
    try {
        if (!mNativePica || draw.VertexShaderSource.empty() ||
            draw.FragmentShaderSource.empty() || draw.VertexCount == 0U ||
            draw.VertexBindings.empty() || draw.VertexAttributes.empty()) {
            throw std::runtime_error("PICA OpenGL draw is incomplete");
        }
        if (draw.FragmentOperationMode != 0U &&
            draw.FragmentOperationMode != 3U) {
            throw std::runtime_error("PICA fragment operation is unsupported");
        }
        if (draw.ScissorMode == 1U) {
            throw std::runtime_error("PICA exclude scissor requires shader emulation");
        }
        if (draw.ScissorMode != 0U && draw.ScissorMode != 3U) {
            throw std::runtime_error("invalid PICA scissor mode");
        }
        if (draw.FramebufferWidth == 0U || draw.FramebufferHeight == 0U) {
            throw std::runtime_error("PICA framebuffer has zero extent");
        }
        if (mNativePica->Composition.Valid() &&
            !mNativePica->Composition.ConsumeDraw(
                Renderer3ds::BuildPicaCompositionDrawReference(draw), error)) {
            return false;
        }

        auto& program = GetPicaGlProgram(*mNativePica, draw);
        auto& target = GetPicaGlTarget(*mNativePica, draw);
        const PicaGlTargetKey targetKey{
            draw.RenderTargetNamespace,
            draw.FramebufferColorPhysicalAddress,
            draw.FramebufferDepthPhysicalAddress,
            draw.FramebufferWidth,
            draw.FramebufferHeight,
            draw.FramebufferColorFormat,
            draw.FramebufferDepthFormat,
        };
        ApplyPendingPicaFills(*mNativePica, targetKey, target);

        glBindFramebuffer(GL_FRAMEBUFFER, target.Framebuffer);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glBindVertexArray(mNativePica->VertexArray);
        glUseProgram(program.Handle);

        glBindBuffer(GL_UNIFORM_BUFFER, mNativePica->VertexUniformBuffer);
        glBufferData(GL_UNIFORM_BUFFER,
                     static_cast<GLsizeiptr>(draw.VertexUniformBytes.size()),
                     draw.VertexUniformBytes.data(), GL_STREAM_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0,
                         mNativePica->VertexUniformBuffer);
        glBindBuffer(GL_UNIFORM_BUFFER, mNativePica->FragmentUniformBuffer);
        glBufferData(GL_UNIFORM_BUFFER,
                     static_cast<GLsizeiptr>(draw.FragmentUniformBytes.size()),
                     draw.FragmentUniformBytes.data(), GL_STREAM_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 4,
                         mNativePica->FragmentUniformBuffer);
        mNativePica->UniformUploads += 2U;
        mNativePica->UniformUploadBytes += draw.VertexUniformBytes.size() +
                                           draw.FragmentUniformBytes.size();

        uint16_t nextAttributeMask = 0U;
        for (const auto& binding : draw.VertexBindings) {
            if (binding.Binding >= mNativePica->VertexBuffers.size() ||
                binding.ByteStride == 0U || binding.Bytes.empty() ||
                binding.ByteStride > static_cast<uint32_t>(
                                         std::numeric_limits<GLsizei>::max()) ||
                binding.Bytes.size() > static_cast<size_t>(
                                           std::numeric_limits<GLsizeiptr>::max())) {
                throw std::runtime_error("PICA vertex binding is invalid");
            }
        }
        for (const auto& attribute : draw.VertexAttributes) {
            if (attribute.Location >= 16U || attribute.Binding >= 16U ||
                attribute.ComponentCount == 0U ||
                attribute.ComponentCount > 4U) {
                throw std::runtime_error("PICA vertex attribute is invalid");
            }
            nextAttributeMask |= static_cast<uint16_t>(
                1U << attribute.Location);
        }
        const uint16_t disabledAttributes =
            mNativePica->EnabledAttributeMask & ~nextAttributeMask;
        for (GLuint location = 0U; location < 16U; ++location) {
            if ((disabledAttributes & (1U << location)) != 0U) {
                glDisableVertexAttribArray(location);
            }
        }

        const bool persistentGeometry =
            mNativePica->GeometryCacheEnabled &&
            draw.GeometryIdentityAvailable && draw.GeometryIdentity != 0U &&
            draw.GeometryContentVersion != 0U;
        const Oot3d::PicaPreparedGeometry* preparedGeometry = nullptr;
        GLuint persistentGeometryBuffer = 0U;
        if (persistentGeometry) {
            auto& streams = mNativePica->GeometrySourceStreams;
            streams.clear();
            streams.reserve(draw.VertexBindings.size());
            for (const auto& binding : draw.VertexBindings) {
                streams.push_back({
                    {binding.Binding, binding.ByteStride,
                     binding.PerInstance},
                    binding.Bytes,
                });
            }
            auto& attributes = mNativePica->GeometrySourceAttributes;
            attributes.clear();
            attributes.reserve(draw.VertexAttributes.size());
            for (const auto& attribute : draw.VertexAttributes) {
                attributes.push_back({
                    attribute.Location,
                    attribute.Binding,
                    static_cast<Oot3d::PicaNriVertexScalar>(
                        attribute.Format),
                    attribute.ComponentCount,
                    attribute.ByteOffset,
                });
            }
            const auto resolution = mNativePica->GeometryRegistry.Resolve({
                draw.GeometryIdentity,
                draw.GeometryContentVersion,
                true,
                mFrameCount,
                streams,
                attributes,
                draw.IndexBytes,
                draw.Indexed,
                draw.IndicesAre16Bit,
                draw.VertexCount,
                false,
            });
            if (resolution.Geometry == nullptr) {
                throw std::runtime_error(
                    "PICA OpenGL geometry preparation failed");
            }
            for (const uint64_t evicted :
                 mNativePica->GeometryRegistry.TakeEvictedIdentities()) {
                DeletePicaGlGeometry(*mNativePica, evicted);
            }
            preparedGeometry = resolution.Geometry;
            auto& buffer = mNativePica
                               ->GeometryBuffers[preparedGeometry->Identity];
            if (buffer.Handle == 0U) {
                glGenBuffers(1, &buffer.Handle);
                if (buffer.Handle == 0U) {
                    throw std::runtime_error(
                        "PICA OpenGL geometry buffer allocation failed");
                }
            }
            if (buffer.ContentVersion !=
                    preparedGeometry->ContentVersion ||
                buffer.StructuralSignature !=
                    preparedGeometry->StructuralSignature) {
                if (preparedGeometry->Payload.size() >
                    static_cast<size_t>(
                        std::numeric_limits<GLsizeiptr>::max())) {
                    throw std::runtime_error(
                        "PICA OpenGL geometry payload is too large");
                }
                glBindBuffer(GL_ARRAY_BUFFER, buffer.Handle);
                glBufferData(
                    GL_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(
                        preparedGeometry->Payload.size()),
                    preparedGeometry->Payload.data(), GL_STATIC_DRAW);
                buffer.ContentVersion = preparedGeometry->ContentVersion;
                buffer.StructuralSignature =
                    preparedGeometry->StructuralSignature;
                ++mNativePica->GeometryPersistentUploads;
                mNativePica->GeometryPersistentUploadBytes +=
                    preparedGeometry->Payload.size();
            }
            persistentGeometryBuffer = buffer.Handle;
            ++mNativePica->GeometryPersistentDraws;
        } else {
            for (const auto& binding : draw.VertexBindings) {
                const GLuint buffer =
                    mNativePica->VertexBuffers[binding.Binding];
                glBindBuffer(GL_ARRAY_BUFFER, buffer);
                glBufferData(GL_ARRAY_BUFFER,
                             static_cast<GLsizeiptr>(binding.Bytes.size()),
                             binding.Bytes.data(), GL_STREAM_DRAW);
                ++mNativePica->GeometryStreamingUploads;
                mNativePica->GeometryStreamingUploadBytes +=
                    binding.Bytes.size();
            }
            ++mNativePica->GeometryStreamingDraws;
        }
        for (const auto& attribute : draw.VertexAttributes) {
            const auto binding = std::find_if(
                draw.VertexBindings.begin(), draw.VertexBindings.end(),
                [&](const auto& candidate) {
                    return candidate.Binding == attribute.Binding;
                });
            if (binding == draw.VertexBindings.end()) {
                throw std::runtime_error("PICA vertex attribute has no binding");
            }
            const GLuint buffer = persistentGeometry
                                      ? persistentGeometryBuffer
                                      : mNativePica->VertexBuffers[
                                            attribute.Binding];
            const GLenum type = ToPicaGlVertexType(attribute.Format);
            uint64_t byteOffset = attribute.ByteOffset;
            if (persistentGeometry) {
                const auto preparedBinding = std::find_if(
                    preparedGeometry->SourceBindings.begin(),
                    preparedGeometry->SourceBindings.end(),
                    [&](const auto& candidate) {
                        return candidate.Binding == attribute.Binding;
                    });
                if (preparedBinding ==
                    preparedGeometry->SourceBindings.end()) {
                    throw std::runtime_error(
                        "PICA prepared geometry has no vertex binding");
                }
                if (preparedBinding->Offset >
                    std::numeric_limits<uint64_t>::max() - byteOffset) {
                    throw std::runtime_error(
                        "PICA OpenGL vertex offset overflow");
                }
                byteOffset += preparedBinding->Offset;
            }
            if (byteOffset > std::numeric_limits<uintptr_t>::max()) {
                throw std::runtime_error(
                    "PICA OpenGL vertex offset is too large");
            }
            auto& state =
                mNativePica->AttributeStates[attribute.Location];
            if (!state.Configured || state.Buffer != buffer ||
                state.ByteStride != binding->ByteStride ||
                state.ByteOffset != static_cast<uintptr_t>(byteOffset) ||
                state.Type != type ||
                state.ComponentCount != attribute.ComponentCount) {
                glBindBuffer(GL_ARRAY_BUFFER, buffer);
                glVertexAttribPointer(
                    attribute.Location, attribute.ComponentCount, type,
                    GL_FALSE, binding->ByteStride,
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(
                        byteOffset)));
                state.Buffer = buffer;
                state.ByteStride = binding->ByteStride;
                state.ByteOffset = static_cast<uintptr_t>(byteOffset);
                state.Type = type;
                state.ComponentCount = attribute.ComponentCount;
                state.Configured = true;
            }
            const uint16_t attributeBit = static_cast<uint16_t>(
                1U << attribute.Location);
            if ((mNativePica->EnabledAttributeMask & attributeBit) == 0U) {
                glEnableVertexAttribArray(attribute.Location);
            }
            const uint8_t divisor = binding->PerInstance ? 1U : 0U;
            if (mNativePica->AttributeDivisors[attribute.Location] !=
                divisor) {
                glVertexAttribDivisor(attribute.Location, divisor);
                mNativePica->AttributeDivisors[attribute.Location] = divisor;
            }
        }
        mNativePica->EnabledAttributeMask = nextAttributeMask;

        std::array<GLuint, 3> textures{
            mNativePica->FallbackColor,
            mNativePica->FallbackColor,
            mNativePica->FallbackColor,
        };
        for (const auto& texture : draw.Textures) {
            if (texture.Slot >= textures.size()) {
                throw std::runtime_error("PICA texture slot is invalid");
            }
            if (texture.NativeType != 0U &&
                !(texture.Slot == 0U &&
                  (texture.NativeType == 2U || texture.NativeType == 3U))) {
                throw std::runtime_error("PICA texture type is unsupported");
            }
            GLuint handle = texture.NativeType == 2U
                ? FindLivePicaShadow(*mNativePica, texture)
                : 0U;
            if (handle == 0U) {
                handle = GetPicaGlTexture(*mNativePica, texture).Handle;
            }
            textures[texture.Slot] = handle;
        }
        for (size_t slot = 0U; slot < textures.size(); ++slot) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(slot + 1U));
            glBindTexture(GL_TEXTURE_2D, textures[slot]);
        }
        GLuint lighting = mNativePica->FallbackInteger;
        if (!draw.LightingLut.PackedEntries.empty()) {
            lighting = GetPicaLightingLut(*mNativePica, draw.LightingLut);
        }
        glActiveTexture(GL_TEXTURE13);
        glBindTexture(GL_TEXTURE_2D, lighting);

        const bool shadowProducer = draw.FragmentOperationMode == 3U;
        glBindImageTexture(5, target.Shadow, 0, GL_FALSE, 0,
                           GL_READ_WRITE, GL_R32UI);
        if (shadowProducer) {
            glDisable(GL_BLEND);
            glDisable(GL_COLOR_LOGIC_OP);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        } else {
            glColorMask((draw.ColorWriteMask & 1U) != 0U,
                        (draw.ColorWriteMask & 2U) != 0U,
                        (draw.ColorWriteMask & 4U) != 0U,
                        (draw.ColorWriteMask & 8U) != 0U);
            if (draw.Blend.Enabled) {
                glEnable(GL_BLEND);
                glDisable(GL_COLOR_LOGIC_OP);
                glBlendEquationSeparate(
                    ToPicaGlBlendEquation(draw.Blend.EquationRgb),
                    ToPicaGlBlendEquation(draw.Blend.EquationAlpha));
                glBlendFuncSeparate(
                    ToPicaGlBlendFactor(draw.Blend.SourceRgb),
                    ToPicaGlBlendFactor(draw.Blend.DestRgb),
                    ToPicaGlBlendFactor(draw.Blend.SourceAlpha),
                    ToPicaGlBlendFactor(draw.Blend.DestAlpha));
                glBlendColor(draw.Blend.ConstantColor[0],
                             draw.Blend.ConstantColor[1],
                             draw.Blend.ConstantColor[2],
                             draw.Blend.ConstantColor[3]);
            } else {
                glDisable(GL_BLEND);
                glEnable(GL_COLOR_LOGIC_OP);
                glLogicOp(ToPicaGlLogic(draw.LogicOperation));
            }
        }

        if (draw.DepthTestEnabled || draw.DepthWriteEnabled) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(draw.DepthTestEnabled
                ? ToPicaGlCompare(draw.DepthCompare)
                : GL_ALWAYS);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        glDepthMask(draw.DepthWriteEnabled ? GL_TRUE : GL_FALSE);
        if (draw.Stencil.Enabled) {
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(ToPicaGlCompare(draw.Stencil.Compare),
                          draw.Stencil.Reference, draw.Stencil.CompareMask);
            glStencilMask(draw.Stencil.WriteMask);
            glStencilOp(ToPicaGlStencil(draw.Stencil.Fail),
                        ToPicaGlStencil(draw.Stencil.DepthFail),
                        ToPicaGlStencil(draw.Stencil.Pass));
        } else {
            glDisable(GL_STENCIL_TEST);
        }
        if (draw.CullMode == GfxNativeCullMode::KeepAll) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
            // Vulkan's positive-height viewport reverses window-space winding
            // relative to OpenGL. Keep the PICA semantic winding here instead
            // of copying the Vulkan pipeline's compensating front-face value.
            glFrontFace(draw.CullMode ==
                            GfxNativeCullMode::KeepCounterClockwise
                        ? GL_CCW
                        : GL_CW);
            glCullFace(draw.FramebufferFlipped ? GL_FRONT : GL_BACK);
        }

        // Guest viewport and scissor are in native pixels; the target holds
        // ResolutionScale host pixels per native pixel on each axis. The
        // guest values are truncated first, so N=1 is the original call and
        // N>1 covers exactly the same native pixels.
        const GLint resolutionScale =
            static_cast<GLint>(mNativePica->ResolutionScale);
        glViewport(static_cast<GLint>(draw.ViewportX) * resolutionScale,
                   static_cast<GLint>(draw.ViewportY) * resolutionScale,
                   static_cast<GLsizei>(draw.ViewportWidth) * resolutionScale,
                   static_cast<GLsizei>(draw.ViewportHeight) * resolutionScale);
        glEnable(GL_SCISSOR_TEST);
        if (draw.ScissorMode == 3U) {
            const GLsizei scissorWidth = draw.ScissorX2 >= draw.ScissorX1
                ? static_cast<GLsizei>(draw.ScissorX2 - draw.ScissorX1 + 1U)
                : 0;
            const GLsizei scissorHeight = draw.ScissorY2 >= draw.ScissorY1
                ? static_cast<GLsizei>(draw.ScissorY2 - draw.ScissorY1 + 1U)
                : 0;
            glScissor(static_cast<GLint>(draw.ScissorX1) * resolutionScale,
                      static_cast<GLint>(draw.ScissorY1) * resolutionScale,
                      scissorWidth * resolutionScale,
                      scissorHeight * resolutionScale);
        } else {
            glScissor(0, 0, static_cast<GLsizei>(target.Width),
                      static_cast<GLsizei>(target.Height));
        }

        const GLenum topology = ToPicaGlTopology(draw.Topology);
        if (draw.Indexed) {
            if (draw.IndexBytes.empty()) {
                throw std::runtime_error("PICA indexed draw has no indices");
            }
            const size_t indexSize = draw.IndicesAre16Bit
                                         ? sizeof(uint16_t)
                                         : sizeof(uint8_t);
            if (draw.IndexBytes.size() % indexSize != 0U) {
                throw std::runtime_error(
                    "PICA indexed draw has a partial index");
            }
            if (persistentGeometry) {
                if (!preparedGeometry->Indexed ||
                    preparedGeometry->VertexOrIndexCount >
                        static_cast<uint32_t>(
                            std::numeric_limits<GLsizei>::max()) ||
                    preparedGeometry->IndexOffset >
                        std::numeric_limits<uintptr_t>::max()) {
                    throw std::runtime_error(
                        "PICA OpenGL prepared index data is invalid");
                }
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
                             persistentGeometryBuffer);
                glDrawElementsBaseVertex(
                    topology,
                    static_cast<GLsizei>(
                        preparedGeometry->VertexOrIndexCount),
                    GL_UNSIGNED_SHORT,
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(
                        preparedGeometry->IndexOffset)),
                    draw.BaseVertex);
            } else {
                const size_t indexCount = draw.IndexBytes.size() / indexSize;
                if (draw.IndexBytes.size() >
                        static_cast<size_t>(
                            std::numeric_limits<GLsizeiptr>::max()) ||
                    indexCount > static_cast<size_t>(
                                     std::numeric_limits<GLsizei>::max())) {
                    throw std::runtime_error(
                        "PICA OpenGL streaming index data is too large");
                }
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
                             mNativePica->IndexBuffer);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                             static_cast<GLsizeiptr>(draw.IndexBytes.size()),
                             draw.IndexBytes.data(), GL_STREAM_DRAW);
                ++mNativePica->GeometryStreamingUploads;
                mNativePica->GeometryStreamingUploadBytes +=
                    draw.IndexBytes.size();
                const GLenum type = draw.IndicesAre16Bit
                                        ? GL_UNSIGNED_SHORT
                                        : GL_UNSIGNED_BYTE;
                glDrawElementsBaseVertex(
                    topology, static_cast<GLsizei>(indexCount), type, nullptr,
                    draw.BaseVertex);
            }
        } else {
            if (draw.VertexCount > static_cast<uint32_t>(
                                       std::numeric_limits<GLsizei>::max())) {
                throw std::runtime_error(
                    "PICA non-indexed draw has too many vertices");
            }
            glDrawArrays(topology, 0,
                         static_cast<GLsizei>(draw.VertexCount));
        }
        // Ordinary raster and framebuffer commands are ordered by GL. Only
        // the PICA shadow producer writes through an image binding and needs
        // an explicit visibility barrier before a later shadow consumer.
        if (shadowProducer) {
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                            GL_TEXTURE_FETCH_BARRIER_BIT);
        }
        InvalidateLegacyStateAfterPica();
        return true;
    } catch (const std::exception& exception) {
        InvalidateLegacyStateAfterPica();
        SetPicaGlError(error, exception.what());
        return false;
    }
#endif
}

bool GfxRenderingAPIOGL::SubmitPicaDisplayTransfer(
    const GfxNativePicaDisplayTransferView& transfer,
    std::string* error) {
#if defined(USE_OPENGLES) || defined(__APPLE__)
    SetPicaGlError(error, "native PICA requires OpenGL 4.3");
    return false;
#else
    try {
        if (!mNativePica || transfer.InputPhysicalAddress == 0U ||
            transfer.OutputPhysicalAddress == 0U ||
            transfer.InputWidth == 0U || transfer.InputHeight == 0U ||
            transfer.OutputWidth == 0U || transfer.OutputHeight == 0U) {
            throw std::runtime_error("PICA display transfer is incomplete");
        }
        const uint32_t scaling = (transfer.Flags >> 24U) & 3U;
        if ((transfer.Flags & 8U) != 0U ||
            (transfer.Flags & 0x10000U) != 0U || scaling > 2U) {
            throw std::runtime_error("PICA display transfer mode is unsupported");
        }
        const auto source = std::find_if(
            mNativePica->Targets.begin(), mNativePica->Targets.end(),
            [&](const auto& entry) {
                return entry.first.Namespace == transfer.RenderTargetNamespace &&
                       entry.first.ColorAddress ==
                           transfer.InputPhysicalAddress;
            });
        if (source == mNativePica->Targets.end()) {
            throw std::runtime_error("PICA display transfer source is missing");
        }
        if (transfer.InputWidth > source->first.Width ||
            transfer.InputHeight > source->first.Height) {
            throw std::runtime_error("PICA display transfer exceeds its source");
        }

        const uint32_t outputWidth = std::max<uint32_t>(1U,
            (static_cast<uint64_t>(transfer.OutputWidth) *
                 source->first.Width + transfer.InputWidth / 2U) /
                transfer.InputWidth);
        const uint32_t outputHeight = std::max<uint32_t>(1U,
            (static_cast<uint64_t>(transfer.OutputHeight) *
                 source->first.Height + transfer.InputHeight / 2U) /
                transfer.InputHeight);
        const uint32_t horizontalSamples = scaling != 0U ? 2U : 1U;
        const uint32_t verticalSamples = scaling == 2U ? 2U : 1U;
        if (static_cast<uint64_t>(outputWidth) * horizontalSamples >
                source->first.Width ||
            static_cast<uint64_t>(outputHeight) * verticalSamples >
                source->first.Height) {
            throw std::runtime_error("PICA display transfer samples out of bounds");
        }
        auto& display = GetPicaGlDisplay(
            *mNativePica, transfer.RenderTargetNamespace,
            transfer.OutputPhysicalAddress, outputWidth, outputHeight);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, source->second.Framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, display.Framebuffer);
        glDisable(GL_SCISSOR_TEST);
        // Both images hold ResolutionScale host pixels per native pixel. A
        // 2:1 linear blit samples each destination centre on the boundary of
        // two source texels, so the PICA box filter stays exact at any scale.
        const uint32_t resolutionScale = mNativePica->ResolutionScale;
        glBlitFramebuffer(
            0, 0,
            static_cast<GLint>(outputWidth * horizontalSamples * resolutionScale),
            static_cast<GLint>(outputHeight * verticalSamples * resolutionScale),
            0, 0, static_cast<GLint>(outputWidth * resolutionScale),
            static_cast<GLint>(outputHeight * resolutionScale),
            GL_COLOR_BUFFER_BIT,
            scaling == 0U ? GL_NEAREST : GL_LINEAR);
        display.Initialized = true;

        if (transfer.Present) {
            SDL_Window* window = SDL_GL_GetCurrentWindow();
            if (window == nullptr) {
                throw std::runtime_error("PICA OpenGL scanout has no SDL window");
            }
            int windowWidth = 0;
            int windowHeight = 0;
#ifdef __SWITCH__
            SDL_GetWindowSize(window, &windowWidth, &windowHeight);
#else
            SDL_GL_GetDrawableSize(window, &windowWidth, &windowHeight);
#endif
            if (windowWidth <= 0 || windowHeight <= 0) {
                throw std::runtime_error("PICA OpenGL scanout extent is invalid");
            }
            // The transfer image is stored in the CTR's physical portrait
            // layout; presentation uses the transposed logical dimensions.
            // The placement uses the native extent; the sampled image holds
            // ResolutionScale times as many pixels (e.g. 3x: 1200x720, an
            // exact 1:1 fit of the 1280x720 handheld screen).
            const float scale = std::min(
                static_cast<float>(windowWidth) / outputHeight,
                static_cast<float>(windowHeight) / outputWidth);
            const int viewportWidth = std::max(
                1, static_cast<int>(std::lround(outputHeight * scale)));
            const int viewportHeight = std::max(
                1, static_cast<int>(std::lround(outputWidth * scale)));
            const int viewportX = (windowWidth - viewportWidth) / 2;
            const int viewportY = (windowHeight - viewportHeight) / 2;

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_COLOR_LOGIC_OP);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            if (transfer.PresentationMode ==
                GfxNativePicaPresentationMode::AlphaOverlay) {
                glEnable(GL_BLEND);
                glBlendEquation(GL_FUNC_ADD);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            } else {
                glDisable(GL_BLEND);
                glViewport(0, 0, windowWidth, windowHeight);
                glClearColor(mClearColor[0], mClearColor[1],
                             mClearColor[2], mClearColor[3]);
                glClear(GL_COLOR_BUFFER_BIT);
                glViewport(viewportX, viewportY,
                           viewportWidth, viewportHeight);
            }
            glBindVertexArray(mNativePica->VertexArray);
            const bool transferInputLinear =
                (transfer.Flags & (1U << 1U)) != 0U;
            const bool dontSwizzle =
                (transfer.Flags & (1U << 5U)) != 0U;
            const bool flip =
                ((!transferInputLinear !=
                  (transferInputLinear != dontSwizzle)) ^
                 ((transfer.Flags & 1U) != 0U));
            // Opaque (top-screen) presentations may run the optional
            // AA/FSR/sharpen chain; alpha overlays never do, as on PC.
            if (transfer.PresentationMode !=
                    GfxNativePicaPresentationMode::AlphaOverlay &&
                mNativePica->Post.Config.ScanoutEffects()) {
                PresentPicaGlPost(*mNativePica, display, flip, viewportX,
                                  viewportY, viewportWidth, viewportHeight);
            } else {
                glUseProgram(mNativePica->ScanoutProgram);
                if (mNativePica->ScanoutFlipLocation >= 0) {
                    glUniform1i(mNativePica->ScanoutFlipLocation,
                                flip ? 1 : 0);
                }
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, display.Color);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
            mNativePica->LastPresented = transfer;
        }
        InvalidateLegacyStateAfterPica();
        return true;
    } catch (const std::exception& exception) {
        InvalidateLegacyStateAfterPica();
        SetPicaGlError(error, exception.what());
        return false;
    }
#endif
}

bool GfxRenderingAPIOGL::ClearPicaRenderTarget(
    uint64_t renderTargetNamespace, uint32_t colorPhysicalAddress,
    std::string* error) {
#if defined(USE_OPENGLES) || defined(__APPLE__)
    SetPicaGlError(error, "native PICA requires OpenGL 4.3");
    return false;
#else
    if (!mNativePica || colorPhysicalAddress == 0U) {
        SetPicaGlError(error, "PICA render-target clear is incomplete");
        return false;
    }
    for (auto& [key, target] : mNativePica->Targets) {
        if (key.Namespace != renderTargetNamespace ||
            key.ColorAddress != colorPhysicalAddress) {
            continue;
        }
        ClearPicaShadow(target, 0xFFFFFFFFU);
        glBindFramebuffer(GL_FRAMEBUFFER, target.Framebuffer);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFFU);
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClearDepth(1.0);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                GL_STENCIL_BUFFER_BIT);
    }
    InvalidateLegacyStateAfterPica();
    return true;
#endif
}

bool GfxRenderingAPIOGL::SubmitPicaMemoryFill(
    const GfxNativePicaMemoryFillView& fill, std::string* error) {
#if defined(USE_OPENGLES) || defined(__APPLE__)
    SetPicaGlError(error, "native PICA requires OpenGL 4.3");
    return false;
#else
    if (!mNativePica || fill.StartPhysicalAddress == 0U ||
        fill.EndPhysicalAddress <= fill.StartPhysicalAddress ||
        (fill.Control & 1U) == 0U) {
        SetPicaGlError(error, "PICA memory fill is incomplete");
        return false;
    }
    const auto previous = std::find_if(
        mNativePica->PendingFills.begin(),
        mNativePica->PendingFills.end(),
        [&](const auto& pending) {
            return pending.RenderTargetNamespace == fill.RenderTargetNamespace &&
                   pending.StartPhysicalAddress == fill.StartPhysicalAddress &&
                   pending.EndPhysicalAddress == fill.EndPhysicalAddress;
        });
    if (previous != mNativePica->PendingFills.end()) {
        *previous = fill;
    } else {
        mNativePica->PendingFills.push_back(fill);
    }
    for (const auto& [key, target] : mNativePica->Targets) {
        ApplyPendingPicaFills(*mNativePica, key, target);
    }
    InvalidateLegacyStateAfterPica();
    return true;
#endif
}

bool GfxRenderingAPIOGL::QueuePicaCompletion(
    uint64_t completionId, std::string* error) {
    if (!mNativePica || completionId == 0U) {
        SetPicaGlError(error, "PICA completion identity is invalid");
        return false;
    }
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
    // SubmitPicaDraw has already copied every guest source span into
    // backend-owned GL buffers/textures. A completion acknowledges that copy;
    // it does not need to idle the GPU. GL preserves command ordering through
    // the later display blit and SDL swap.
#endif
    mNativePica->Completions.push_back(completionId);
    return true;
}

std::vector<uint64_t> GfxRenderingAPIOGL::TakePicaCompletions() {
    if (!mNativePica) {
        return {};
    }
    auto result = std::move(mNativePica->Completions);
    mNativePica->Completions.clear();
    return result;
}

bool GfxRenderingAPIOGL::ResetPicaState(std::string* error) {
    try {
        if (!mNativePica) {
            throw std::runtime_error("PICA OpenGL backend is not initialized");
        }
#if !defined(USE_OPENGLES) && !defined(__APPLE__)
        glFinish();
#endif
        InitNativePica();
        InvalidateLegacyStateAfterPica();
        return true;
    } catch (const std::exception& exception) {
        SetPicaGlError(error, exception.what());
        return false;
    }
}

} // namespace Fast

#endif
