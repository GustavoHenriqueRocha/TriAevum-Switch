#version 460
// deko3d UI, solid program (shade colour): the GL Fast3D layout xyzw + rgba.
layout(location = 0) in vec4 ui_position;
layout(location = 1) in vec4 ui_color;
layout(std140, binding = 0) uniform UiParams {
    mat4 ui_transform; // SetOot3dNativeTransform, row-major: position * matrix
    vec4 ui_params;    // x: clip-space y scale (UiClipYScale)
};
layout(location = 0) out vec4 ui_shade;
void main() {
    vec4 position = ui_position * ui_transform;
    gl_Position = vec4(position.x, position.y * ui_params.x, position.z, position.w);
    ui_shade = ui_color;
}
