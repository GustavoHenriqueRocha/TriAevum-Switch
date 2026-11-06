#version 460
// deko3d grass fragment program (see grass_vsh.glsl). gen.py builds two
// variants: GRASS_EXPLICIT_DEPTH writes gl_FragDepth with the PICA depth
// mapping of the source draw, only for w-buffered draws (that mapping is not
// linear in screen space); the other leaves depth to the rasterizer (the
// vertex program put the PICA depth into clip z) and keeps early depth
// testing.
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

layout(location = 0) in vec4 grass_shade;
layout(location = 1) in vec2 grass_card;
layout(location = 2) in float grass_fog_factor;
layout(location = 3) flat in vec4 grass_tuft; // blades, unused, blade mode, unused
layout(location = 4) flat in vec4 grass_blades[5]; // centre, 1 / top, lean (grass_vsh.glsl)
layout(location = 0) out vec4 grass_out_color;

void main() {
    if (grass_tuft.z < 0.5) {
        // The card holds its blades side by side; each narrows to its tip.
        // Only the blade of this slot and its two neighbours can cover it.
        int count = int(grass_tuft.x);
        int slot = clamp(int(grass_card.x * grass_tuft.x), 0, count - 1);
        float halfWidthBase = 0.42 / grass_tuft.x;
        bool covered = false;
        for (int i = max(slot - 1, 0); i <= min(slot + 1, count - 1); ++i) {
            vec4 blade = grass_blades[i];
            float v = grass_card.y * blade.y;
            covered = covered || (v < 1.0 && abs(grass_card.x - blade.x - blade.z * v) < halfWidthBase * (1.0 - v));
        }
        if (!covered) discard;
    }
    vec3 colour = grass_shade.rgb;
    if (grass_fog_color.w > 0.5) colour = mix(grass_fog_color.rgb, colour, grass_fog_factor);
    grass_out_color = vec4(colour, 1.0);
#ifdef GRASS_EXPLICIT_DEPTH
    float picaDepth = -gl_FragCoord.z * grass_depth.x + grass_depth.y;
    if (grass_depth.z > 0.5) picaDepth /= gl_FragCoord.w;
    gl_FragDepth = picaDepth;
#endif
}
