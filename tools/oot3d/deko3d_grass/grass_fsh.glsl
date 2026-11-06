#version 460
// deko3d grass fragment program (see grass_vsh.glsl): the blades are
// triangles, lit and fogged per vertex, so a fragment only writes its colour
// (no discard: early depth testing stays on). gen.py builds two variants:
// GRASS_EXPLICIT_DEPTH writes gl_FragDepth with the PICA depth mapping of the
// source draw, only for w-buffered draws (that mapping is not linear in
// screen space); the other leaves depth to the rasterizer (the vertex
// program put the PICA depth into clip z).
#ifdef GRASS_EXPLICIT_DEPTH
layout(std140, binding = 0) uniform GrassPlacement {
    mat4 grass_position_to_clip;
    vec4 grass_depth;              // scale, offset, w-buffer, explicit depth
    vec4 grass_fog_color;          // rgb, mode (0 off, 1 regular, 2 flipped)
    vec4 grass_fog_lut[64];
    vec4 grass_light_direction[3];
    vec4 grass_light_diffuse[3];
    vec4 grass_light_ambient[3];
    vec4 grass_eye;
};
#endif

layout(location = 0) in vec4 grass_shade;
layout(location = 0) out vec4 grass_out_color;

void main() {
    grass_out_color = vec4(grass_shade.rgb, 1.0);
#ifdef GRASS_EXPLICIT_DEPTH
    float picaDepth = -gl_FragCoord.z * grass_depth.x + grass_depth.y;
    if (grass_depth.z > 0.5) picaDepth /= gl_FragCoord.w;
    gl_FragDepth = picaDepth;
#endif
}
