// Near-plane clipping of PICA draws in the OpenGL backend, on a real GL
// context (EGL surfaceless, Mesa; skipped when none is available).
//
// Bug seen on the Switch: in first person (the slingshot aim) a huge brown
// polygon covered half the screen for a moment. GfxRenderingAPIOGL::Init
// enables GL_DEPTH_CLAMP for the legacy renderer and the PICA path drew with
// it: GL then skips the near/far clip planes, so a triangle with a vertex
// between the eye and the near plane (Link's arm, sleeve or the slingshot
// right in front of the camera) is rasterized whole instead of being clipped
// as the PICA (and the Vulkan backend) do. The test draws such a triangle
// through the real GL dialect (TranslatePicaShaderToOpenGl43) with the
// backend's GL_DEPTH_CLAMP sequence (PicaGlStateCache::DepthClamp) and
// compares the covered pixels with a CPU reference that clips like the PICA.

#include "fast/backends/gfx_opengl_pica_state_cache.h"
#include "fast/renderer3ds/pica_gl_shader_dialect.h"

#include <gtest/gtest.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using Vec4 = std::array<float, 4>;

constexpr int kWidth = 400;
constexpr int kHeight = 240;
constexpr float kNear = 1.0F;
constexpr float kFar = 100.0F;

// PICA clip coordinates of a view-space point `depth` units in front of the
// eye: w = depth, z = 0 at the near plane and -w at the far plane (visible
// when -w <= z <= 0).
Vec4 PicaClip(float x, float y, float depth) {
    return {x, y, -(depth - kNear) * kFar / (kFar - kNear), depth};
}

// The canonical vertex epilogue of the PICA shader generator
// (oot3d_native_pica_shader_gen.cpp), fed straight from the attribute.
const char* kCanonicalVertex =
    "#version 450\n"
    "layout(location=0) in vec4 pica_input;\n"
    "void main() {\n"
    "    vec4 pica_position = pica_input;\n"
    "    float pica_ndc_z = pica_position.z / pica_position.w;\n"
    "    if (pica_ndc_z > 0.0 && pica_ndc_z < 0.000001) pica_position.z = 0.0;\n"
    "    if (pica_ndc_z < -1.0 && pica_ndc_z > -1.00001) pica_position.z = -pica_position.w;\n"
    "    gl_Position = vec4(pica_position.x, pica_position.y, -pica_position.z, pica_position.w);\n"
    "}\n";

const char* kFragment =
    "#version 430\n"
    "layout(location=0) out vec4 color;\n"
    "void main() { color = vec4(0.6, 0.4, 0.2, 1.0); }\n";

// Sutherland-Hodgman against the PICA's clip volume (x, y within +-w,
// -w <= z <= 0), then pixel centres inside the projected polygon.
size_t ReferenceCoverage(const std::array<Vec4, 3>& triangle) {
    std::vector<Vec4> polygon(triangle.begin(), triangle.end());
    const std::array<Vec4, 6> planes{{
        {1, 0, 0, 1}, {-1, 0, 0, 1}, {0, 1, 0, 1}, {0, -1, 0, 1},
        {0, 0, -1, 0},  // z <= 0 (near)
        {0, 0, 1, 1},   // z >= -w (far)
    }};
    for (const auto& plane : planes) {
        std::vector<Vec4> clipped;
        const auto distance = [&](const Vec4& v) {
            return double(plane[0]) * v[0] + double(plane[1]) * v[1] + double(plane[2]) * v[2] +
                   double(plane[3]) * v[3];
        };
        for (size_t i = 0; i < polygon.size(); ++i) {
            const Vec4& a = polygon[i];
            const Vec4& b = polygon[(i + 1) % polygon.size()];
            const double da = distance(a), db = distance(b);
            if (da >= 0) clipped.push_back(a);
            if ((da >= 0) != (db >= 0)) {
                const double t = da / (da - db);
                Vec4 v{};
                for (size_t c = 0; c < 4; ++c) v[c] = float(a[c] + (b[c] - a[c]) * t);
                clipped.push_back(v);
            }
        }
        polygon = std::move(clipped);
        if (polygon.empty()) return 0;
    }
    std::vector<std::array<double, 2>> screen;
    for (const auto& v : polygon)
        screen.push_back({(v[0] / v[3] * 0.5 + 0.5) * kWidth, (v[1] / v[3] * 0.5 + 0.5) * kHeight});
    size_t covered = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const double px = x + 0.5, py = y + 0.5;
            bool inside = false;
            for (size_t i = 0, j = screen.size() - 1; i < screen.size(); j = i++) {
                const auto& a = screen[i];
                const auto& b = screen[j];
                if ((a[1] > py) != (b[1] > py) &&
                    px < (b[0] - a[0]) * (py - a[1]) / (b[1] - a[1]) + a[0])
                    inside = !inside;
            }
            covered += inside ? 1U : 0U;
        }
    }
    return covered;
}

class GlContext {
public:
    GlContext() {
        const auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (getDisplay == nullptr) return;
        mDisplay = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (mDisplay == EGL_NO_DISPLAY || !eglInitialize(mDisplay, nullptr, nullptr)) return;
        if (!eglBindAPI(EGL_OPENGL_API)) return;
        const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3,
                                     EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                     EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
        mContext = eglCreateContext(mDisplay, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
        if (mContext == EGL_NO_CONTEXT) return;
        mReady = eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, mContext);
    }
    ~GlContext() {
        if (mContext != EGL_NO_CONTEXT) {
            eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(mDisplay, mContext);
        }
        if (mDisplay != EGL_NO_DISPLAY) eglTerminate(mDisplay);
    }
    bool Ready() const { return mReady; }

private:
    EGLDisplay mDisplay = EGL_NO_DISPLAY;
    EGLContext mContext = EGL_NO_CONTEXT;
    bool mReady = false;
};

GLuint CompileShader(GLenum type, const std::string& source) {
    const GLuint shader = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        ADD_FAILURE() << log;
    }
    return shader;
}

struct Scene {
    GLuint Framebuffer = 0, Color = 0, Depth = 0, Program = 0, VertexArray = 0, Buffer = 0;

    Scene() {
        glGenFramebuffers(1, &Framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, Framebuffer);
        glGenRenderbuffers(1, &Color);
        glBindRenderbuffer(GL_RENDERBUFFER, Color);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kWidth, kHeight);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, Color);
        glGenRenderbuffers(1, &Depth);
        glBindRenderbuffer(GL_RENDERBUFFER, Depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, kWidth, kHeight);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, Depth);

        std::string vertex, error;
        EXPECT_TRUE(Fast::Renderer3ds::TranslatePicaShaderToOpenGl43(kCanonicalVertex, vertex, &error))
            << error;
        Program = glCreateProgram();
        glAttachShader(Program, CompileShader(GL_VERTEX_SHADER, vertex));
        glAttachShader(Program, CompileShader(GL_FRAGMENT_SHADER, kFragment));
        glLinkProgram(Program);
        glUseProgram(Program);
        glGenVertexArrays(1, &VertexArray);
        glBindVertexArray(VertexArray);
        glGenBuffers(1, &Buffer);
        glBindBuffer(GL_ARRAY_BUFFER, Buffer);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
        glViewport(0, 0, kWidth, kHeight);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    }

    size_t Draw(const std::array<Vec4, 3>& triangle) {
        glBufferData(GL_ARRAY_BUFFER, sizeof(triangle), triangle.data(), GL_STREAM_DRAW);
        glClearColor(0, 0, 0, 0);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        std::vector<unsigned char> pixels(size_t(kWidth) * kHeight * 4U);
        glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        size_t covered = 0;
        for (size_t i = 0; i < size_t(kWidth) * kHeight; ++i) covered += pixels[i * 4U + 3U] != 0 ? 1U : 0U;
        return covered;
    }
};

// Pixel-centre rules differ at the edges: allow a band of pixels.
void ExpectCoverage(size_t drawn, size_t reference, const char* what) {
    const double tolerance = 0.03 * double(reference) + 200.0;
    EXPECT_LE(std::fabs(double(drawn) - double(reference)), tolerance)
        << what << ": drawn " << drawn << " pixels, PICA clipping covers " << reference;
}

TEST(PicaGlNearClip, PicaDrawsClipGeometryInsideTheNearPlane) {
    GlContext context;
    if (!context.Ready()) GTEST_SKIP() << "no EGL surfaceless OpenGL 4.3 context";
    Scene scene;
    glEnable(GL_DEPTH_CLAMP); // GfxRenderingAPIOGL::Init (legacy renderer)
    Fast::PicaGlStateCache::DepthClamp clamp;

    // A sleeve of the first-person arm: one vertex 0.2 units from the eye
    // (near plane at 1), the others in plain view.
    const std::array<Vec4, 3> sleeve{PicaClip(-0.5F, 0.6F, 0.2F), PicaClip(0.5F, -0.5F, 3.0F),
                                     PicaClip(1.2F, 0.2F, 3.0F)};
    // Fully between the eye and the near plane: the PICA draws nothing.
    const std::array<Vec4, 3> inside{PicaClip(-0.3F, -0.3F, 0.5F), PicaClip(0.3F, -0.3F, 0.5F),
                                     PicaClip(0.0F, 0.3F, 0.5F)};
    const size_t sleeveReference = ReferenceCoverage(sleeve);
    const size_t insideReference = ReferenceCoverage(inside);
    EXPECT_GT(sleeveReference, 1000U);
    EXPECT_EQ(insideReference, 0U);

    // What the PICA draw path did before: the legacy clamp left on.
    const size_t sleeveClamped = scene.Draw(sleeve);
    const size_t insideClamped = scene.Draw(inside);
    std::printf("GL_DEPTH_CLAMP on: sleeve %zu px, inside %zu px; PICA reference %zu / %zu px\n",
                sleeveClamped, insideClamped, sleeveReference, insideReference);
    EXPECT_GT(double(sleeveClamped), 1.5 * double(sleeveReference));
    EXPECT_GT(insideClamped, 10000U);

    // The backend's sequence before a PICA draw.
    if (clamp.EnterPica()) glDisable(GL_DEPTH_CLAMP);
    EXPECT_FALSE(glIsEnabled(GL_DEPTH_CLAMP));
    const size_t sleeveDrawn = scene.Draw(sleeve);
    const size_t insideDrawn = scene.Draw(inside);
    std::printf("PICA draw path:    sleeve %zu px, inside %zu px\n", sleeveDrawn, insideDrawn);
    ExpectCoverage(sleeveDrawn, sleeveReference, "sleeve");
    EXPECT_EQ(insideDrawn, 0U);

    // And back for the legacy renderer.
    if (clamp.LeavePica()) glEnable(GL_DEPTH_CLAMP);
    EXPECT_TRUE(glIsEnabled(GL_DEPTH_CLAMP));
    EXPECT_EQ(glGetError(), GLenum(GL_NO_ERROR));
}

} // namespace
