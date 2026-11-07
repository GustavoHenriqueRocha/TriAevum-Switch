// Shared by grass_tuft_vsh.glsl and grass_lod_vsh.glsl (config/grass_lod on;
// SWITCH_PORT.md "Grass on deko3d: cost per tuft"). gen.py pastes it where a
// program says #include "grass_lod_common.glsl" (uam has no #include).
// grass_vsh.glsl (config/grass_lod = 0, test.9's grass) does not use it.

// Per placement: GrassPlacementUniforms in gfx_deko3d_grass.cpp (as grass_vsh.glsl).
layout(std140, binding = 0) uniform GrassPlacement {
    mat4 grass_position_to_clip;   // world -> PICA clip of the source draw
    vec4 grass_depth;              // scale, offset, w-buffer, explicit depth
    vec4 grass_fog_color;          // rgb, mode (0 off, 1 regular, 2 flipped)
    vec4 grass_fog_lut[64];        // 128 (value, delta) pairs
    vec4 grass_light_direction[3]; // world, toward the source; w = valid
    vec4 grass_light_diffuse[3];
    vec4 grass_light_ambient[3];
    vec4 grass_eye;                // xyz eye; w = active light count
};

// Per frame: GrassFrameUniforms in gfx_deko3d_grass.cpp (as grass_vsh.glsl).
layout(std140, binding = 1) uniform GrassFrame {
    vec4 grass_wind;              // direction x, direction z, strength, seconds
    vec4 grass_wind_detail;       // speed, spatial scale, gust strength, gust frequency
    vec4 grass_shape;             // blades per tuft, card width scale, maximum bend, lighting
    vec4 grass_distance;          // draw distance, fade start, single blades to, tufts from
    vec4 grass_root_color;        // rgb, ground colour influence
    vec4 grass_tip_color;         // rgb, unused
    vec4 grass_brightness;        // root, tip, unused, collider count
    vec4 grass_collider[4];       // capsule centre xyz, radius
    vec4 grass_collider_extent[4];// half height, unused
};

// Per draw: GrassLodUniforms in gfx_deko3d_grass.cpp. The level coordinate
// (grass_lod_coordinate) is GrassLodCoordinate in grass_instance_stream.cpp.
layout(std140, binding = 2) uniform GrassLod {
    vec4 grass_lod_scale; // focal length in pixels of a 1200x720 target, level 0 to, 1 / ramp length, unused
    vec4 grass_lod_px;    // 5 -> 3 blades from x to y pixels of a blade slot, 3 -> 1 from z to w
    vec4 grass_lod_draw;  // 1 / blades drawn, blades it turns into, 1 / that, level of its first layout
    uvec4 grass_lod_index;// float bits of 1 / (3 x blades drawn), 3 x blades drawn, unused
};

// One tuft's per-frame work, written by grass_tuft_vsh.glsl (64 bytes, all
// float: no half or unorm unpacking per vertex): base xyz and height (fade
// applied); the card's half width along the camera-facing side (x, z) and
// the bend (x, z); the lit root colour and the blades' random seed (bits);
// the lit tip colour and the level coordinate (grass_lod_coordinate).
struct GrassTuft {
    vec4 base_height;
    vec4 side_bend;
    vec4 root_seed;
    vec4 tip_level;
};

vec3 grass_decode_normal(uint bits) {
    vec2 oct = unpackSnorm2x16(bits);
    vec3 n = vec3(oct, 1.0 - abs(oct.x) - abs(oct.y));
    float fold = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -fold : fold;
    n.y += n.y >= 0.0 ? -fold : fold;
    return normalize(n);
}

// Integer hash: "lowbias32" by Chris Wellons (hash-prospector,
// https://github.com/skeeto/hash-prospector, public domain / Unlicense),
// the same mix as MixGrassBits in grass_instance_stream.cpp. It replaces
// grass_vsh.glsl's fract(sin(x) * 43758.5453), whose sin runs on the
// quarter-rate special function unit and loses its low bits for large x.
uint grass_mix_bits(uint value) {
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

// A tuft's seed: its base (a fill tuft has its own) and wind phase.
uint grass_tuft_seed(vec3 base, uint widthPhaseFade) {
    uint x = grass_mix_bits(floatBitsToUint(base.x) ^ ((widthPhaseFade >> 16u) & 0xffu));
    return grass_mix_bits(floatBitsToUint(base.z) ^ x);
}

// Blade `blade`'s three random values in [0, 1) (slot jitter, tip lean,
// height; 10, 11 and 11 bits of one hash, put into a float's mantissa: no
// conversion instruction), the same in every level, so a blade keeps its
// look as the tuft loses blades.
vec3 grass_blade_random(uint seed, uint blade) {
    uint bits = grass_mix_bits(seed + blade * 0x9e3779b9u);
    uvec3 mantissa = uvec3((bits & 0x3ffu) << 13u, ((bits >> 10u) & 0x7ffu) << 12u, (bits >> 21u) << 12u);
    return uintBitsToFloat(mantissa | 0x3f800000u) - 1.0;
}

// Level coordinate from 0 (5 blades) through 1 (3 blades) to 2 (1 blade):
// the projected width of a blade slot (2 x half width, the 5-blade layout)
// on a 1200x720 target, never past 2 x the distance ramp from
// grass_lod_scale.y (single blades and their turn into tufts stay 5 blades).
float grass_lod_coordinate(float distanceToEye, float halfWidth) {
    float px = 2.0 * halfWidth * grass_lod_scale.x / max(distanceToEye, 1.0);
    float toThree = clamp((grass_lod_px.x - px) / (grass_lod_px.x - grass_lod_px.y), 0.0, 1.0);
    float toOne = clamp((grass_lod_px.z - px) / (grass_lod_px.z - grass_lod_px.w), 0.0, 1.0);
    float limit = 2.0 * clamp((distanceToEye - grass_lod_scale.y) * grass_lod_scale.z, 0.0, 1.0);
    return min(toThree + toOne, limit);
}

// Blade `blade` of a tuft of 1 / `from` blades turning into a `target`-blade
// one (1 / `to` blades; morph 0..1), across the card (0..1): its base
// centre and half width and its tip. Blades below `target` move to and
// widen into their slot of the target layout; the others narrow to nothing
// where they stand. The card keeps its width and the blades' summed width
// (so the covered area) stays 0.84 of it all the way. At morph 1 the
// layout is the target's at morph 0. grass_vsh.glsl's tuft is from = to =
// 1 / BladesPerTuft, morph 0.
void grass_lod_blade(float blade, float from, float target, float to, float morph, vec3 random,
                     out float centre, out float halfBase, out float tip) {
    float slot = blade + 0.5 + (random.x - 0.5) * 0.5;
    bool kept = blade < target;
    float scale = from + ((kept ? to : 0.0) - from) * morph;
    centre = slot * (kept ? scale : from);
    halfBase = 0.42 * scale;
    tip = centre + (random.y - 0.5) * 0.6 * scale;
}

// The PICA depth and fog of the native fragment program, per vertex (as grass_vsh.glsl).
float grass_pica_depth(vec4 clip) {
    float depth = clip.z / clip.w * grass_depth.x + grass_depth.y;
    if (grass_depth.z > 0.5) depth *= clip.w;
    return depth;
}

float grass_fog(float picaDepth) {
    if (grass_fog_color.w < 0.5) return 1.0;
    float index = (grass_fog_color.w > 1.5 ? 1.0 - picaDepth : picaDepth) * 128.0;
    float entryIndex = clamp(floor(index), 0.0, 127.0);
    int entry = int(entryIndex);
    vec4 pair = grass_fog_lut[entry >> 1];
    vec2 value = (entry & 1) == 0 ? pair.xy : pair.zw;
    return clamp(value.x + value.y * (index - entryIndex), 0.0, 1.0);
}

// EvaluateGrassLighting (as grass_vsh.glsl): ambient plus diffuse with |N.L|, clamped.
vec3 grass_light(vec3 normal) {
    int count = int(grass_eye.w);
    if (count == 0 || grass_shape.w < 0.5) return vec3(1.0);
    vec3 total = vec3(0.0);
    for (int i = 0; i < 3; ++i) {
        if (i >= count || grass_light_direction[i].w < 0.5) continue;
        float diffuse = abs(clamp(dot(normal, grass_light_direction[i].xyz), -1.0, 1.0));
        total += grass_light_ambient[i].rgb + grass_light_diffuse[i].rgb * diffuse;
    }
    return clamp(total, 0.0, 1.0);
}

// Clip position and colour out, as grass_vsh.glsl ends: fog per vertex, the
// PICA depth mapping folded into clip z.
void grass_emit(vec3 position, vec3 colour, out vec4 shade) {
    vec4 clip = grass_position_to_clip * vec4(position, 1.0);
    if (grass_fog_color.w > 0.5) colour = mix(grass_fog_color.rgb, colour, grass_fog(grass_pica_depth(clip)));
    shade = vec4(colour, 1.0);
    float zGl = grass_depth.z > 0.5 ? -2.0 * clip.z - clip.w
                                     : 2.0 * (clip.z * grass_depth.x + grass_depth.y * clip.w) - clip.w;
    gl_Position = vec4(clip.x, clip.y, zGl, clip.w);
}
