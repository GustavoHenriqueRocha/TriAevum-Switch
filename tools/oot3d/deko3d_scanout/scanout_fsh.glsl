#version 460
// Same transform as the GL scanout (gfx_opengl_pica.cpp kScanoutFragment).
layout(binding = 0) uniform sampler2D pica_scanout;
layout(std140, binding = 0) uniform ScanoutParams {
    vec4 pica_params; // x: flip (0/1), zw: content uv scale
};
layout(location = 0) in vec2 pica_uv;
layout(location = 0) out vec4 pica_color;
void main() {
    vec2 uv = vec2(pica_uv.y, pica_uv.x);
    if (pica_params.x != 0.0) uv.y = 1.0 - uv.y;
    pica_color = texture(pica_scanout, uv * pica_params.zw);
}
