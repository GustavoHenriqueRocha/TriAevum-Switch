#version 460
// deko3d grass fragment program (see grass_vsh.glsl). gen.py builds two
// variants: GRASS_EXPLICIT_DEPTH writes gl_FragDepth with the PICA depth
// mapping of the source draw (its native fragment program does the same),
// the other leaves depth to the rasterizer, for draws whose PICA depth is the
// raster depth (GrassDepthIsRasterDepth), and keeps early depth testing.
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
layout(location = 3) flat in vec4 grass_tuft; // blades, seed, blade mode, unused
layout(location = 0) out vec4 grass_out_color;

float grass_hash(float value) {
    return fract(sin(value * 12.9898 + 78.233) * 43758.5453);
}

void main() {
    if (grass_tuft.z < 0.5) {
        // The card holds its blades side by side: blade i stands at
        // (i + 0.5) / n (jittered), leans a little and narrows to its tip.
        float count = grass_tuft.x;
        float slot = clamp(floor(grass_card.x * count), 0.0, count - 1.0);
        bool covered = false;
        for (float i = max(slot - 1.0, 0.0); i <= min(slot + 1.0, count - 1.0); i += 1.0) {
            float r0 = grass_hash(grass_tuft.y * 17.0 + i);
            float r1 = grass_hash(grass_tuft.y * 31.0 + i * 1.7);
            float top = 0.7 + 0.3 * grass_hash(grass_tuft.y * 53.0 + i * 2.3);
            float v = grass_card.y / top;
            float centre = (i + 0.5 + (r0 - 0.5) * 0.5) / count + (r1 - 0.5) * 0.6 / count * v;
            float halfWidth = 0.42 / count * (1.0 - v);
            covered = covered || (v < 1.0 && abs(grass_card.x - centre) < halfWidth);
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
