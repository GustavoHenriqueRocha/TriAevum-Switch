#version 460
// deko3d scanout (gfx_deko3d.cpp): a window-covering triangle. With
// OriginUpperLeft NDC y = -1 is the top of the window, so v is flipped to
// keep the GL scanout's uv (0,0) at the bottom-left of the screen.
layout(location = 0) out vec2 pica_uv;
void main() {
    const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 position = positions[gl_VertexID];
    gl_Position = vec4(position, 0.0, 1.0);
    pica_uv = vec2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
}
