#version 460
// deko3d simple shadow, method "texture" (gfx_deko3d_player_shadow.cpp): the
// floor region under Link as a triangle strip of four corners, whose
// gl_Position (his camera, with the PICA dialect's depth) and silhouette
// coordinates the renderer computes.
layout(std140, binding = 0) uniform PlayerShadowFloor {
    vec4 positions[4];
    vec4 texcoords[4]; // xy
    vec4 shade;        // x: opacity, yz: one texel of the silhouette
};

layout(location = 0) out vec2 silhouette_uv;

void main() {
    gl_Position = positions[gl_VertexID];
    silhouette_uv = texcoords[gl_VertexID].xy;
}
