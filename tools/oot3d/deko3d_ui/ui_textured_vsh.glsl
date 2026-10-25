#version 460
// deko3d UI, textured program (TEXEL0 * shade): xyzw + uv + rgba.
layout(location = 0) in vec4 ui_position;
layout(location = 1) in vec2 ui_uv;
layout(location = 2) in vec4 ui_color;
layout(std140, binding = 0) uniform UiParams {
    mat4 ui_transform; // SetOot3dNativeTransform, row-major: position * matrix
    vec4 ui_params;    // x: clip-space y scale (UiClipYScale)
};
layout(location = 0) out vec4 ui_shade;
layout(location = 1) out vec2 ui_texcoord;
void main() {
    vec4 position = ui_position * ui_transform;
    gl_Position = vec4(position.x, position.y * ui_params.x, position.z, position.w);
    ui_shade = ui_color;
    ui_texcoord = ui_uv;
}
