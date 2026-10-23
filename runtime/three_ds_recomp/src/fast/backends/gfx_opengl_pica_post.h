#pragma once

// Optional post-processing for the OpenGL PICA scanout of the top screen
// (Switch homebrew and desktop GL 4.3): FXAA or SMAA 1x, AMD FSR 1 (EASU
// upscale + RCAS sharpening) and plain RCAS sharpening. Everything here is
// pure (config parsing, constant setup and GLSL sources) so it can be checked
// offline; gfx_opengl_pica.cpp owns the GL objects.
//
// Pipeline, in the CTR's physical portrait image space (every effect is
// orientation independent; only the last pass applies the same
// portrait-to-landscape transform as the plain scanout):
//   render (resolution scale N) -> AA -> EASU -> RCAS -> window.
//
// FXAA reuses the PC/NRI scanout library (fast/oot3d/spatial_aa.cpp) and
// SMAA 1x the official SMAA 2.8 source and lookup textures already embedded
// for the NRI pipeline, with the same preset (HIGH), so both match the PC
// look. FSR 1 is a GLSL 4.30 port of the official ffx_fsr1.h (see the
// license notice below); EASU and RCAS are kept verbatim, only the minimal
// subset of ffx_a.h they need is spelled out.

#include "fast/oot3d/smaa_official_shader_source.h"
#include "fast/oot3d/spatial_aa.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace Fast::PicaGlPost {

enum class Antialiasing : uint8_t {
    Off,
    Fxaa,
    Smaa,
};

enum class Upscaler : uint8_t {
    Linear,
    Fsr,
};

// RCAS strength used by FSR when config/sharpen is absent.
inline constexpr float kFsrDefaultSharpen = 0.2F;

struct Config {
    Antialiasing Aa = Antialiasing::Off;
    Upscaler Upscale = Upscaler::Linear;
    // Linear RCAS strength in [0, 1]: 1.0 is FSR's maximum (0 stops), 0.0
    // disables the pass. Equivalent to FsrRcasCon(exp2(-stops)).
    float Sharpen = 0.0F;
    // GL_TEXTURE_MAX_ANISOTROPY for linear, mipmapped PICA textures; 0 = off.
    uint32_t Anisotropy = 0U;

    [[nodiscard]] bool ScanoutEffects() const noexcept {
        return Aa != Antialiasing::Off || Upscale != Upscaler::Linear ||
               Sharpen > 0.0F;
    }
};

inline std::string_view Trim(std::string_view text) noexcept {
    const auto blank = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    while (!text.empty() && blank(text.front())) {
        text.remove_prefix(1U);
    }
    while (!text.empty() && blank(text.back())) {
        text.remove_suffix(1U);
    }
    return text;
}

inline bool EqualsIgnoreCase(std::string_view text,
                             std::string_view expected) noexcept {
    if (text.size() != expected.size()) {
        return false;
    }
    for (size_t index = 0U; index < text.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(text[index])) !=
            expected[index]) {
            return false;
        }
    }
    return true;
}

inline Antialiasing ParseAntialiasing(std::string_view text) noexcept {
    text = Trim(text);
    if (EqualsIgnoreCase(text, "fxaa")) {
        return Antialiasing::Fxaa;
    }
    if (EqualsIgnoreCase(text, "smaa") || EqualsIgnoreCase(text, "smaa1x")) {
        return Antialiasing::Smaa;
    }
    return Antialiasing::Off;
}

inline Upscaler ParseUpscaler(std::string_view text) noexcept {
    text = Trim(text);
    return EqualsIgnoreCase(text, "fsr") || EqualsIgnoreCase(text, "fsr1")
        ? Upscaler::Fsr
        : Upscaler::Linear;
}

// Only 2, 4, 8 and 16 are accepted; anything else leaves filtering as is.
inline uint32_t ParseAnisotropy(std::string_view text) noexcept {
    text = Trim(text);
    if (text == "2" || text == "4" || text == "8" || text == "16") {
        return static_cast<uint32_t>(
            std::strtoul(std::string(text).c_str(), nullptr, 10));
    }
    return 0U;
}

// A decimal in [0, 1] (clamped); invalid text is treated as absent.
inline std::optional<float> ParseSharpen(std::string_view text) {
    const std::string value(Trim(text));
    if (value.empty()) {
        return std::nullopt;
    }
    char* end = nullptr;
    const float parsed = std::strtof(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0' || !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed < 0.0F ? 0.0F : (parsed > 1.0F ? 1.0F : parsed);
}

inline Config ResolveConfig(const std::optional<std::string>& antialiasing,
                            const std::optional<std::string>& anisotropy,
                            const std::optional<std::string>& upscaler,
                            const std::optional<std::string>& sharpen) {
    Config config;
#if defined(__SWITCH__)
    // Measured on the console: FSR 1 and 4x anisotropy cost only GPU time,
    // which is idle there. A config file still overrides each (e.g. "linear"
    // in config/upscaler, "1" in config/anisotropy).
    config.Upscale = Upscaler::Fsr;
    config.Anisotropy = 4U;
#endif
    if (antialiasing.has_value()) {
        config.Aa = ParseAntialiasing(*antialiasing);
    }
    if (anisotropy.has_value()) {
        config.Anisotropy = ParseAnisotropy(*anisotropy);
    }
    if (upscaler.has_value()) {
        config.Upscale = ParseUpscaler(*upscaler);
    }
    std::optional<float> strength;
    if (sharpen.has_value()) {
        strength = ParseSharpen(*sharpen);
    }
    if (strength.has_value()) {
        config.Sharpen = *strength;
    } else if (config.Upscale == Upscaler::Fsr) {
        config.Sharpen = kFsrDefaultSharpen;
    }
    return config;
}

inline uint32_t FloatBits(float value) noexcept {
    uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// FsrEasuCon() from ffx_fsr1.h (CPU side), for an input image used whole.
// Returns con0..con3 as four consecutive uvec4.
// FsrEasuCon() of ffx_fsr1.h: the input viewport (the rendered part, at
// the bottom-left of the image) may be smaller than the input image
// (dynamic resolution draws a sub-rectangle of the allocated target).
inline std::array<uint32_t, 16> FsrEasuConstants(float inputViewportWidth,
                                                 float inputViewportHeight,
                                                 float inputWidth,
                                                 float inputHeight,
                                                 float outputWidth,
                                                 float outputHeight) noexcept {
    std::array<uint32_t, 16> con{};
    con[0] = FloatBits(inputViewportWidth * (1.0F / outputWidth));
    con[1] = FloatBits(inputViewportHeight * (1.0F / outputHeight));
    con[2] = FloatBits(0.5F * inputViewportWidth * (1.0F / outputWidth) - 0.5F);
    con[3] = FloatBits(0.5F * inputViewportHeight * (1.0F / outputHeight) - 0.5F);
    con[4] = FloatBits(1.0F / inputWidth);
    con[5] = FloatBits(1.0F / inputHeight);
    con[6] = FloatBits(1.0F * (1.0F / inputWidth));
    con[7] = FloatBits(-1.0F * (1.0F / inputHeight));
    con[8] = FloatBits(-1.0F * (1.0F / inputWidth));
    con[9] = FloatBits(2.0F * (1.0F / inputHeight));
    con[10] = FloatBits(1.0F * (1.0F / inputWidth));
    con[11] = FloatBits(2.0F * (1.0F / inputHeight));
    con[12] = FloatBits(0.0F * (1.0F / inputWidth));
    con[13] = FloatBits(4.0F * (1.0F / inputHeight));
    con[14] = 0U;
    con[15] = 0U;
    return con;
}

// The whole image is the input viewport.
inline std::array<uint32_t, 16> FsrEasuConstants(float inputWidth,
                                                 float inputHeight,
                                                 float outputWidth,
                                                 float outputHeight) noexcept {
    return FsrEasuConstants(inputWidth, inputHeight, inputWidth, inputHeight,
                            outputWidth, outputHeight);
}

// FsrRcasCon() takes stops (sharpness = exp2(-stops)); the config already
// holds the linear value, so con.x is that value.
inline std::array<uint32_t, 4> FsrRcasConstants(float linearSharpness) noexcept {
    return {FloatBits(linearSharpness), 0U, 0U, 0U};
}

// ---------------------------------------------------------------------------
// GLSL. All passes use the scanout's full-screen-triangle vertex shader
// (out vec2 pica_uv). Every intermediate image is in the physical portrait
// layout of the display-transfer image; a pass drawn into the window
// (pica_post_orient = 1) maps the landscape viewport onto it exactly as the
// plain scanout does.

inline constexpr std::string_view kPreludeSource = R"glsl(#version 430
in vec2 pica_uv;
layout(location=0) out vec4 pica_color;
uniform int pica_post_orient;
uniform int pica_flip_y;
// Portrait extent of the pixel grid this pass writes.
uniform vec2 pica_post_output_size;
vec2 pica_post_uv() {
    if (pica_post_orient == 0) return pica_uv;
    // Same transform as the plain scanout (CTR physical portrait image,
    // OpenGL framebuffer origin).
    vec2 uv = vec2(pica_uv.y, pica_uv.x);
    if (pica_flip_y != 0) uv.y = 1.0 - uv.y;
    return uv;
}
uvec2 pica_post_output_pixel() {
    if (pica_post_orient == 0) return uvec2(gl_FragCoord.xy);
    return uvec2(clamp(floor(pica_post_uv() * pica_post_output_size),
                       vec2(0.0), pica_post_output_size - 1.0));
}
)glsl";

inline std::string BuildFxaaFragmentShader() {
    std::string source(kPreludeSource);
    source += R"glsl(
layout(binding=0) uniform sampler2D pica_post_source;
// The PC scanout library reads scanout.input_linear and
// scanout.inverse_size; the GL scanout image is always sRGB-encoded RGBA8.
struct PicaPostFxaaState { uint input_linear; vec2 inverse_size; };
uniform PicaPostFxaaState scanout;
vec3 oot3d_linear_to_srgb(vec3 value) {
    value=max(value,vec3(0.0));
    bvec3 cutoff=lessThanEqual(value,vec3(0.0031308));
    vec3 lower=value*12.92;
    vec3 higher=1.055*pow(value,vec3(1.0/2.4))-0.055;
    return mix(higher,lower,cutoff);
}
vec4 oot3d_compose_at(vec2 uv) {
    return textureLod(pica_post_source, uv, 0.0);
}
)glsl";
    source += ::Fast::Oot3d::BuildSpatialAaShaderLibrary();
    source += R"glsl(
void main() {
    vec2 uv = pica_post_uv();
    pica_color = oot3d_fxaa(uv, oot3d_compose_at(uv));
}
)glsl";
    return source;
}

enum class SmaaStage : uint8_t {
    EdgeDetection,
    BlendWeightCalculation,
    NeighborhoodBlending,
};

// Fragment versions of the NRI compute passes in fast/oot3d/smaa_1x.cpp:
// same official source, preset and entry points. GL keeps combined samplers
// so the official SMAA_GLSL_4 macros are used unchanged; the edge pass keeps
// its official discard (the edge image is cleared first).
inline std::string BuildSmaaFragmentShader(SmaaStage stage) {
    std::string source = R"glsl(#version 430
uniform vec4 pica_smaa_metrics;
#define SMAA_GLSL_4 1
#define SMAA_PRESET_HIGH 1
#define SMAA_RT_METRICS pica_smaa_metrics
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 1
)glsl";
    for (const std::string_view chunk :
         ::Fast::Oot3d::kOfficialSmaaShaderSourceChunks) {
        source.append(chunk);
    }
    source += R"glsl(
in vec2 pica_uv;
layout(location=0) out vec4 pica_color;
)glsl";
    switch (stage) {
        case SmaaStage::EdgeDetection:
            source += R"glsl(
layout(binding=0) uniform sampler2D pica_smaa_color;
void main() {
    vec4 offset[3];
    SMAAEdgeDetectionVS(pica_uv, offset);
    vec2 edges = SMAALumaEdgeDetectionPS(pica_uv, offset, pica_smaa_color);
    pica_color = vec4(edges, 0.0, 1.0);
}
)glsl";
            break;
        case SmaaStage::BlendWeightCalculation:
            source += R"glsl(
layout(binding=0) uniform sampler2D pica_smaa_edges;
layout(binding=1) uniform sampler2D pica_smaa_area;
layout(binding=2) uniform sampler2D pica_smaa_search;
void main() {
    vec2 pixcoord;
    vec4 offset[3];
    SMAABlendingWeightCalculationVS(pica_uv, pixcoord, offset);
    pica_color = SMAABlendingWeightCalculationPS(
        pica_uv, pixcoord, offset, pica_smaa_edges, pica_smaa_area,
        pica_smaa_search, vec4(0.0));
}
)glsl";
            break;
        case SmaaStage::NeighborhoodBlending:
            source += R"glsl(
layout(binding=0) uniform sampler2D pica_smaa_color;
layout(binding=1) uniform sampler2D pica_smaa_weights;
void main() {
    vec4 offset;
    SMAANeighborhoodBlendingVS(pica_uv, offset);
    pica_color = SMAANeighborhoodBlendingPS(
        pica_uv, offset, pica_smaa_color, pica_smaa_weights);
}
)glsl";
            break;
    }
    return source;
}

// ---------------------------------------------------------------------------
// AMD FidelityFX Super Resolution 1.0 (FSR 1), EASU and RCAS, 32-bit paths.
// Source: https://github.com/GPUOpen-Effects/FidelityFX-FSR
//         ffx-fsr/ffx_fsr1.h (v1.20210629) and ffx-fsr/ffx_a.h, MIT license.
// The functions between the markers below are copied verbatim; the license
// notice of ffx_fsr1.h is reproduced here and inside the GLSL source:
//
//_____________________________________________________________/\_______________________________________________________________
//==============================================================================================================================
//
//
//                    AMD FidelityFX SUPER RESOLUTION [FSR 1] ::: SPATIAL SCALING & EXTRAS - v1.20210629
//
//
//------------------------------------------------------------------------------------------------------------------------------
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------------------------------------------------------
// FidelityFX Super Resolution Sample
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//------------------------------------------------------------------------------------------------------------------------------
inline constexpr std::string_view kFsrCommonSource = R"FSR(
//_____________________________________________________________/\_______________________________________________________________
//==============================================================================================================================
//
//
//                    AMD FidelityFX SUPER RESOLUTION [FSR 1] ::: SPATIAL SCALING & EXTRAS - v1.20210629
//
//
//------------------------------------------------------------------------------------------------------------------------------
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------------------------------------------------------
// FidelityFX Super Resolution Sample
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//------------------------------------------------------------------------------------------------------------------------------
// Minimal GLSL subset of ffx_a.h used by the FSR 1 32-bit EASU/RCAS paths.
#define A_GPU 1
#define A_GLSL 1
#define AP1 bool
#define AF1 float
#define AF2 vec2
#define AF3 vec3
#define AF4 vec4
#define AU1 uint
#define AU2 uvec2
#define AU4 uvec4
#define ASU2 ivec2
#define AF1_(a) AF1(a)
#define AF2_(a) AF2(a)
#define AF3_(a) AF3(a)
#define AF4_(a) AF4(a)
#define AU1_(a) AU1(a)
#define AF1_AU1(x) uintBitsToFloat(AU1(x))
#define AF2_AU2(x) uintBitsToFloat(AU2(x))
#define AU1_AF1(x) floatBitsToUint(AF1(x))
AF1 ARcpF1(AF1 x){return AF1_(1.0)/x;}
AF1 ASatF1(AF1 x){return clamp(x,AF1_(0.0),AF1_(1.0));}
AF1 AMax3F1(AF1 x,AF1 y,AF1 z){return max(x,max(y,z));}
AF1 AMin3F1(AF1 x,AF1 y,AF1 z){return min(x,min(y,z));}
AF3 AMax3F3(AF3 x,AF3 y,AF3 z){return max(x,max(y,z));}
AF3 AMin3F3(AF3 x,AF3 y,AF3 z){return min(x,min(y,z));}
AF1 APrxLoRcpF1(AF1 a){return AF1_AU1(AU1_(0x7ef07ebb)-AU1_AF1(a));}
AF1 APrxMedRcpF1(AF1 a){AF1 b=AF1_AU1(AU1_(0x7ef19fff)-AU1_AF1(a));return b*(-b*a+AF1_(2.0));}
AF1 APrxLoRsqF1(AF1 a){return AF1_AU1(AU1_(0x5f347d74)-(AU1_AF1(a)>>AU1_(1)));}
#define FSR_RCAS_LIMIT (0.25-(1.0/16.0))
)FSR";

inline constexpr std::string_view kFsrEasuSource = R"FSR(
// ---- begin verbatim ffx_fsr1.h: FSR_EASU_F (NON-PACKED 32-BIT VERSION) ----
 // Input callback prototypes, need to be implemented by calling shader
 AF4 FsrEasuRF(AF2 p);
 AF4 FsrEasuGF(AF2 p);
 AF4 FsrEasuBF(AF2 p);
//------------------------------------------------------------------------------------------------------------------------------
 // Filtering for a given tap for the scalar.
 void FsrEasuTapF(
 inout AF3 aC, // Accumulated color, with negative lobe.
 inout AF1 aW, // Accumulated weight.
 AF2 off, // Pixel offset from resolve position to tap.
 AF2 dir, // Gradient direction.
 AF2 len, // Length.
 AF1 lob, // Negative lobe strength.
 AF1 clp, // Clipping point.
 AF3 c){ // Tap color.
  // Rotate offset by direction.
  AF2 v;
  v.x=(off.x*( dir.x))+(off.y*dir.y);
  v.y=(off.x*(-dir.y))+(off.y*dir.x);
  // Anisotropy.
  v*=len;
  // Compute distance^2.
  AF1 d2=v.x*v.x+v.y*v.y;
  // Limit to the window as at corner, 2 taps can easily be outside.
  d2=min(d2,clp);
  // Approximation of lancos2 without sin() or rcp(), or sqrt() to get x.
  //  (25/16 * (2/5 * x^2 - 1)^2 - (25/16 - 1)) * (1/4 * x^2 - 1)^2
  //  |_______________________________________|   |_______________|
  //                   base                             window
  // The general form of the 'base' is,
  //  (a*(b*x^2-1)^2-(a-1))
  // Where 'a=1/(2*b-b^2)' and 'b' moves around the negative lobe.
  AF1 wB=AF1_(2.0/5.0)*d2+AF1_(-1.0);
  AF1 wA=lob*d2+AF1_(-1.0);
  wB*=wB;
  wA*=wA;
  wB=AF1_(25.0/16.0)*wB+AF1_(-(25.0/16.0-1.0));
  AF1 w=wB*wA;
  // Do weighted average.
  aC+=c*w;aW+=w;}
//------------------------------------------------------------------------------------------------------------------------------
 // Accumulate direction and length.
 void FsrEasuSetF(
 inout AF2 dir,
 inout AF1 len,
 AF2 pp,
 AP1 biS,AP1 biT,AP1 biU,AP1 biV,
 AF1 lA,AF1 lB,AF1 lC,AF1 lD,AF1 lE){
  // Compute bilinear weight, branches factor out as predicates are compiler time immediates.
  //  s t
  //  u v
  AF1 w = AF1_(0.0);
  if(biS)w=(AF1_(1.0)-pp.x)*(AF1_(1.0)-pp.y);
  if(biT)w=           pp.x *(AF1_(1.0)-pp.y);
  if(biU)w=(AF1_(1.0)-pp.x)*           pp.y ;
  if(biV)w=           pp.x *           pp.y ;
  // Direction is the '+' diff.
  //    a
  //  b c d
  //    e
  // Then takes magnitude from abs average of both sides of 'c'.
  // Length converts gradient reversal to 0, smoothly to non-reversal at 1, shaped, then adding horz and vert terms.
  AF1 dc=lD-lC;
  AF1 cb=lC-lB;
  AF1 lenX=max(abs(dc),abs(cb));
  lenX=APrxLoRcpF1(lenX);
  AF1 dirX=lD-lB;
  dir.x+=dirX*w;
  lenX=ASatF1(abs(dirX)*lenX);
  lenX*=lenX;
  len+=lenX*w;
  // Repeat for the y axis.
  AF1 ec=lE-lC;
  AF1 ca=lC-lA;
  AF1 lenY=max(abs(ec),abs(ca));
  lenY=APrxLoRcpF1(lenY);
  AF1 dirY=lE-lA;
  dir.y+=dirY*w;
  lenY=ASatF1(abs(dirY)*lenY);
  lenY*=lenY;
  len+=lenY*w;}
//------------------------------------------------------------------------------------------------------------------------------
 void FsrEasuF(
 out AF3 pix,
 AU2 ip, // Integer pixel position in output.
 AU4 con0, // Constants generated by FsrEasuCon().
 AU4 con1,
 AU4 con2,
 AU4 con3){
//------------------------------------------------------------------------------------------------------------------------------
  // Get position of 'f'.
  AF2 pp=AF2(ip)*AF2_AU2(con0.xy)+AF2_AU2(con0.zw);
  AF2 fp=floor(pp);
  pp-=fp;
//------------------------------------------------------------------------------------------------------------------------------
  // 12-tap kernel.
  //    b c
  //  e f g h
  //  i j k l
  //    n o
  // Gather 4 ordering.
  //  a b
  //  r g
  // For packed FP16, need either {rg} or {ab} so using the following setup for gather in all versions,
  //    a b    <- unused (z)
  //    r g
  //  a b a b
  //  r g r g
  //    a b
  //    r g    <- unused (z)
  // Allowing dead-code removal to remove the 'z's.
  AF2 p0=fp*AF2_AU2(con1.xy)+AF2_AU2(con1.zw);
  // These are from p0 to avoid pulling two constants on pre-Navi hardware.
  AF2 p1=p0+AF2_AU2(con2.xy);
  AF2 p2=p0+AF2_AU2(con2.zw);
  AF2 p3=p0+AF2_AU2(con3.xy);
  AF4 bczzR=FsrEasuRF(p0);
  AF4 bczzG=FsrEasuGF(p0);
  AF4 bczzB=FsrEasuBF(p0);
  AF4 ijfeR=FsrEasuRF(p1);
  AF4 ijfeG=FsrEasuGF(p1);
  AF4 ijfeB=FsrEasuBF(p1);
  AF4 klhgR=FsrEasuRF(p2);
  AF4 klhgG=FsrEasuGF(p2);
  AF4 klhgB=FsrEasuBF(p2);
  AF4 zzonR=FsrEasuRF(p3);
  AF4 zzonG=FsrEasuGF(p3);
  AF4 zzonB=FsrEasuBF(p3);
//------------------------------------------------------------------------------------------------------------------------------
  // Simplest multi-channel approximate luma possible (luma times 2, in 2 FMA/MAD).
  AF4 bczzL=bczzB*AF4_(0.5)+(bczzR*AF4_(0.5)+bczzG);
  AF4 ijfeL=ijfeB*AF4_(0.5)+(ijfeR*AF4_(0.5)+ijfeG);
  AF4 klhgL=klhgB*AF4_(0.5)+(klhgR*AF4_(0.5)+klhgG);
  AF4 zzonL=zzonB*AF4_(0.5)+(zzonR*AF4_(0.5)+zzonG);
  // Rename.
  AF1 bL=bczzL.x;
  AF1 cL=bczzL.y;
  AF1 iL=ijfeL.x;
  AF1 jL=ijfeL.y;
  AF1 fL=ijfeL.z;
  AF1 eL=ijfeL.w;
  AF1 kL=klhgL.x;
  AF1 lL=klhgL.y;
  AF1 hL=klhgL.z;
  AF1 gL=klhgL.w;
  AF1 oL=zzonL.z;
  AF1 nL=zzonL.w;
  // Accumulate for bilinear interpolation.
  AF2 dir=AF2_(0.0);
  AF1 len=AF1_(0.0);
  FsrEasuSetF(dir,len,pp,true, false,false,false,bL,eL,fL,gL,jL);
  FsrEasuSetF(dir,len,pp,false,true ,false,false,cL,fL,gL,hL,kL);
  FsrEasuSetF(dir,len,pp,false,false,true ,false,fL,iL,jL,kL,nL);
  FsrEasuSetF(dir,len,pp,false,false,false,true ,gL,jL,kL,lL,oL);
//------------------------------------------------------------------------------------------------------------------------------
  // Normalize with approximation, and cleanup close to zero.
  AF2 dir2=dir*dir;
  AF1 dirR=dir2.x+dir2.y;
  AP1 zro=dirR<AF1_(1.0/32768.0);
  dirR=APrxLoRsqF1(dirR);
  dirR=zro?AF1_(1.0):dirR;
  dir.x=zro?AF1_(1.0):dir.x;
  dir*=AF2_(dirR);
  // Transform from {0 to 2} to {0 to 1} range, and shape with square.
  len=len*AF1_(0.5);
  len*=len;
  // Stretch kernel {1.0 vert|horz, to sqrt(2.0) on diagonal}.
  AF1 stretch=(dir.x*dir.x+dir.y*dir.y)*APrxLoRcpF1(max(abs(dir.x),abs(dir.y)));
  // Anisotropic length after rotation,
  //  x := 1.0 lerp to 'stretch' on edges
  //  y := 1.0 lerp to 2x on edges
  AF2 len2=AF2(AF1_(1.0)+(stretch-AF1_(1.0))*len,AF1_(1.0)+AF1_(-0.5)*len);
  // Based on the amount of 'edge',
  // the window shifts from +/-{sqrt(2.0) to slightly beyond 2.0}.
  AF1 lob=AF1_(0.5)+AF1_((1.0/4.0-0.04)-0.5)*len;
  // Set distance^2 clipping point to the end of the adjustable window.
  AF1 clp=APrxLoRcpF1(lob);
//------------------------------------------------------------------------------------------------------------------------------
  // Accumulation mixed with min/max of 4 nearest.
  //    b c
  //  e f g h
  //  i j k l
  //    n o
  AF3 min4=min(AMin3F3(AF3(ijfeR.z,ijfeG.z,ijfeB.z),AF3(klhgR.w,klhgG.w,klhgB.w),AF3(ijfeR.y,ijfeG.y,ijfeB.y)),
               AF3(klhgR.x,klhgG.x,klhgB.x));
  AF3 max4=max(AMax3F3(AF3(ijfeR.z,ijfeG.z,ijfeB.z),AF3(klhgR.w,klhgG.w,klhgB.w),AF3(ijfeR.y,ijfeG.y,ijfeB.y)),
               AF3(klhgR.x,klhgG.x,klhgB.x));
  // Accumulation.
  AF3 aC=AF3_(0.0);
  AF1 aW=AF1_(0.0);
  FsrEasuTapF(aC,aW,AF2( 0.0,-1.0)-pp,dir,len2,lob,clp,AF3(bczzR.x,bczzG.x,bczzB.x)); // b
  FsrEasuTapF(aC,aW,AF2( 1.0,-1.0)-pp,dir,len2,lob,clp,AF3(bczzR.y,bczzG.y,bczzB.y)); // c
  FsrEasuTapF(aC,aW,AF2(-1.0, 1.0)-pp,dir,len2,lob,clp,AF3(ijfeR.x,ijfeG.x,ijfeB.x)); // i
  FsrEasuTapF(aC,aW,AF2( 0.0, 1.0)-pp,dir,len2,lob,clp,AF3(ijfeR.y,ijfeG.y,ijfeB.y)); // j
  FsrEasuTapF(aC,aW,AF2( 0.0, 0.0)-pp,dir,len2,lob,clp,AF3(ijfeR.z,ijfeG.z,ijfeB.z)); // f
  FsrEasuTapF(aC,aW,AF2(-1.0, 0.0)-pp,dir,len2,lob,clp,AF3(ijfeR.w,ijfeG.w,ijfeB.w)); // e
  FsrEasuTapF(aC,aW,AF2( 1.0, 1.0)-pp,dir,len2,lob,clp,AF3(klhgR.x,klhgG.x,klhgB.x)); // k
  FsrEasuTapF(aC,aW,AF2( 2.0, 1.0)-pp,dir,len2,lob,clp,AF3(klhgR.y,klhgG.y,klhgB.y)); // l
  FsrEasuTapF(aC,aW,AF2( 2.0, 0.0)-pp,dir,len2,lob,clp,AF3(klhgR.z,klhgG.z,klhgB.z)); // h
  FsrEasuTapF(aC,aW,AF2( 1.0, 0.0)-pp,dir,len2,lob,clp,AF3(klhgR.w,klhgG.w,klhgB.w)); // g
  FsrEasuTapF(aC,aW,AF2( 1.0, 2.0)-pp,dir,len2,lob,clp,AF3(zzonR.z,zzonG.z,zzonB.z)); // o
  FsrEasuTapF(aC,aW,AF2( 0.0, 2.0)-pp,dir,len2,lob,clp,AF3(zzonR.w,zzonG.w,zzonB.w)); // n
//------------------------------------------------------------------------------------------------------------------------------
  // Normalize and dering.
  pix=min(max4,max(min4,aC*AF3_(ARcpF1(aW))));}
// ---- end verbatim ffx_fsr1.h ----
)FSR";

inline constexpr std::string_view kFsrRcasSource = R"FSR(
// ---- begin verbatim ffx_fsr1.h: FSR_RCAS_F (NON-PACKED 32-BIT VERSION) ----
 // Input callback prototypes that need to be implemented by calling shader
 AF4 FsrRcasLoadF(ASU2 p);
 void FsrRcasInputF(inout AF1 r,inout AF1 g,inout AF1 b);
//------------------------------------------------------------------------------------------------------------------------------
 void FsrRcasF(
 out AF1 pixR, // Output values, non-vector so port between RcasFilter() and RcasFilterH() is easy.
 out AF1 pixG,
 out AF1 pixB,
 #ifdef FSR_RCAS_PASSTHROUGH_ALPHA
  out AF1 pixA,
 #endif
 AU2 ip, // Integer pixel position in output.
 AU4 con){ // Constant generated by RcasSetup().
  // Algorithm uses minimal 3x3 pixel neighborhood.
  //    b
  //  d e f
  //    h
  ASU2 sp=ASU2(ip);
  AF3 b=FsrRcasLoadF(sp+ASU2( 0,-1)).rgb;
  AF3 d=FsrRcasLoadF(sp+ASU2(-1, 0)).rgb;
  #ifdef FSR_RCAS_PASSTHROUGH_ALPHA
   AF4 ee=FsrRcasLoadF(sp);
   AF3 e=ee.rgb;pixA=ee.a;
  #else
   AF3 e=FsrRcasLoadF(sp).rgb;
  #endif
  AF3 f=FsrRcasLoadF(sp+ASU2( 1, 0)).rgb;
  AF3 h=FsrRcasLoadF(sp+ASU2( 0, 1)).rgb;
  // Rename (32-bit) or regroup (16-bit).
  AF1 bR=b.r;
  AF1 bG=b.g;
  AF1 bB=b.b;
  AF1 dR=d.r;
  AF1 dG=d.g;
  AF1 dB=d.b;
  AF1 eR=e.r;
  AF1 eG=e.g;
  AF1 eB=e.b;
  AF1 fR=f.r;
  AF1 fG=f.g;
  AF1 fB=f.b;
  AF1 hR=h.r;
  AF1 hG=h.g;
  AF1 hB=h.b;
  // Run optional input transform.
  FsrRcasInputF(bR,bG,bB);
  FsrRcasInputF(dR,dG,dB);
  FsrRcasInputF(eR,eG,eB);
  FsrRcasInputF(fR,fG,fB);
  FsrRcasInputF(hR,hG,hB);
  // Luma times 2.
  AF1 bL=bB*AF1_(0.5)+(bR*AF1_(0.5)+bG);
  AF1 dL=dB*AF1_(0.5)+(dR*AF1_(0.5)+dG);
  AF1 eL=eB*AF1_(0.5)+(eR*AF1_(0.5)+eG);
  AF1 fL=fB*AF1_(0.5)+(fR*AF1_(0.5)+fG);
  AF1 hL=hB*AF1_(0.5)+(hR*AF1_(0.5)+hG);
  // Noise detection.
  AF1 nz=AF1_(0.25)*bL+AF1_(0.25)*dL+AF1_(0.25)*fL+AF1_(0.25)*hL-eL;
  nz=ASatF1(abs(nz)*APrxMedRcpF1(AMax3F1(AMax3F1(bL,dL,eL),fL,hL)-AMin3F1(AMin3F1(bL,dL,eL),fL,hL)));
  nz=AF1_(-0.5)*nz+AF1_(1.0);
  // Min and max of ring.
  AF1 mn4R=min(AMin3F1(bR,dR,fR),hR);
  AF1 mn4G=min(AMin3F1(bG,dG,fG),hG);
  AF1 mn4B=min(AMin3F1(bB,dB,fB),hB);
  AF1 mx4R=max(AMax3F1(bR,dR,fR),hR);
  AF1 mx4G=max(AMax3F1(bG,dG,fG),hG);
  AF1 mx4B=max(AMax3F1(bB,dB,fB),hB);
  // Immediate constants for peak range.
  AF2 peakC=AF2(1.0,-1.0*4.0);
  // Limiters, these need to be high precision RCPs.
  AF1 hitMinR=min(mn4R,eR)*ARcpF1(AF1_(4.0)*mx4R);
  AF1 hitMinG=min(mn4G,eG)*ARcpF1(AF1_(4.0)*mx4G);
  AF1 hitMinB=min(mn4B,eB)*ARcpF1(AF1_(4.0)*mx4B);
  AF1 hitMaxR=(peakC.x-max(mx4R,eR))*ARcpF1(AF1_(4.0)*mn4R+peakC.y);
  AF1 hitMaxG=(peakC.x-max(mx4G,eG))*ARcpF1(AF1_(4.0)*mn4G+peakC.y);
  AF1 hitMaxB=(peakC.x-max(mx4B,eB))*ARcpF1(AF1_(4.0)*mn4B+peakC.y);
  AF1 lobeR=max(-hitMinR,hitMaxR);
  AF1 lobeG=max(-hitMinG,hitMaxG);
  AF1 lobeB=max(-hitMinB,hitMaxB);
  AF1 lobe=max(AF1_(-FSR_RCAS_LIMIT),min(AMax3F1(lobeR,lobeG,lobeB),AF1_(0.0)))*AF1_AU1(con.x);
  // Apply noise removal.
  #ifdef FSR_RCAS_DENOISE
   lobe*=nz;
  #endif
  // Resolve, which needs the medium precision rcp approximation to avoid visible tonality changes.
  AF1 rcpL=APrxMedRcpF1(AF1_(4.0)*lobe+AF1_(1.0));
  pixR=(lobe*bR+lobe*dR+lobe*hR+lobe*fR+eR)*rcpL;
  pixG=(lobe*bG+lobe*dG+lobe*hG+lobe*fG+eG)*rcpL;
  pixB=(lobe*bB+lobe*dB+lobe*hB+lobe*fB+eB)*rcpL;
  return;}
// ---- end verbatim ffx_fsr1.h ----
)FSR";

// EASU: upscales the portrait source image to pica_post_output_size.
inline std::string BuildEasuFragmentShader() {
    std::string source(kPreludeSource);
    source += R"glsl(
layout(binding=0) uniform sampler2D pica_post_source;
uniform uvec4 pica_easu_con0;
uniform uvec4 pica_easu_con1;
uniform uvec4 pica_easu_con2;
uniform uvec4 pica_easu_con3;
)glsl";
    source += kFsrCommonSource;
    source += kFsrEasuSource;
    source += R"glsl(
AF4 FsrEasuRF(AF2 p){return textureGather(pica_post_source,p,0);}
AF4 FsrEasuGF(AF2 p){return textureGather(pica_post_source,p,1);}
AF4 FsrEasuBF(AF2 p){return textureGather(pica_post_source,p,2);}
void main() {
    uvec2 ip = pica_post_output_pixel();
    vec3 color;
    FsrEasuF(color, ip, pica_easu_con0, pica_easu_con1, pica_easu_con2,
             pica_easu_con3);
    // EASU filters colour only; alpha follows the bilinear source.
    vec2 input_position = vec2(ip) * uintBitsToFloat(pica_easu_con0.xy) +
                          uintBitsToFloat(pica_easu_con0.zw) + 0.5;
    float alpha = textureLod(pica_post_source,
        input_position * uintBitsToFloat(pica_easu_con1.xy), 0.0).a;
    pica_color = vec4(color, alpha);
}
)glsl";
    return source;
}

// RCAS. With pica_rcas_resample = 0 the source has the output's extent
// (after EASU, or a 1:1 scanout) and RCAS runs exactly as in ffx_fsr1.h.
// Otherwise (sharpening without FSR at a non-1:1 scanout) the five taps are
// bilinear samples one source texel apart around the output position.
inline std::string BuildRcasFragmentShader() {
    std::string source(kPreludeSource);
    source += R"glsl(
#define FSR_RCAS_PASSTHROUGH_ALPHA 1
layout(binding=0) uniform sampler2D pica_post_source;
uniform uvec4 pica_rcas_con;
uniform int pica_rcas_resample;
// Resample mode: the source's content sub-rectangle (dynamic resolution).
uniform vec2 pica_rcas_uv_scale;
ivec2 pica_rcas_center;
vec2 pica_rcas_uv;
vec2 pica_rcas_texel;
)glsl";
    source += kFsrCommonSource;
    source += kFsrRcasSource;
    source += R"glsl(
AF4 FsrRcasLoadF(ASU2 p) {
    if (pica_rcas_resample != 0)
        return textureLod(pica_post_source,
            pica_rcas_uv + vec2(p - pica_rcas_center) * pica_rcas_texel, 0.0);
    return texelFetch(pica_post_source,
        clamp(p, ivec2(0), textureSize(pica_post_source, 0) - 1), 0);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
void main() {
    uvec2 ip = pica_post_output_pixel();
    pica_rcas_center = ivec2(ip);
    pica_rcas_uv = pica_post_uv() * pica_rcas_uv_scale;
    pica_rcas_texel = 1.0 / vec2(textureSize(pica_post_source, 0));
    float r, g, b, a;
    FsrRcasF(r, g, b, a, ip, pica_rcas_con);
    pica_color = vec4(r, g, b, a);
}
)glsl";
    return source;
}

} // namespace Fast::PicaGlPost
