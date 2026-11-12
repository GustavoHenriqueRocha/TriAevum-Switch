#version 460
// deko3d simple shadow fragment program (gfx_deko3d_player_shadow.cpp): Link's
// opaque meshes, flattened onto the floor by their own PICA vertex program with
// patched view rows, darken what is under them by a constant colour. No
// discard and no depth write: the shadow is taken only from draws that leave
// depth to the rasterizer, so the clip z of their vertex program is the PICA
// depth the floor was tested with.
layout(std140, binding = 0) uniform PlayerShadow {
    vec4 shadow_color; // rgb black, a the opacity
};

layout(location = 0) out vec4 shadow_out_color;

void main() {
    shadow_out_color = shadow_color;
}
