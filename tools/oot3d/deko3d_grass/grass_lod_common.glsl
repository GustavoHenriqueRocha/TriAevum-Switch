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
    vec4 grass_tip_color;         // rgb, wind turbulence
    vec4 grass_brightness;        // root, tip, wind randomness, collider count
    vec4 grass_collider[4];       // capsule centre xyz, radius
    vec4 grass_collider_extent[4];// half height, unused
};

// Per draw: GrassLodUniforms in gfx_deko3d_grass.cpp. The level coordinate
// (grass_lod_coordinate) is GrassLodCoordinate in grass_instance_stream.cpp.
layout(std140, binding = 2) uniform GrassLod {
    vec4 grass_lod_scale; // focal length in pixels of a 1200x720 target, level 0 to, 1 / ramp length, unused
    vec4 grass_lod_px;    // 5 -> 3 blades from x to y pixels of a blade slot, 3 -> 1 from z to w
    vec4 grass_lod_draw;  // 1 / blades drawn, blades it turns into, 1 / that, level of its first layout
    uvec4 grass_lod_index;// float bits of 1 / (vertices per tuft), vertices per tuft, unused
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

// The near draw's single blades (GRASS_LOD_NEAR) are the PC's curved blade
// (grass_blade_shape.h) in GRASS_NEAR_SEGMENTS segments, with the PC preset's
// shape (graphics_settings_persistence.cpp, "Grass" "Appearance"): curvature,
// droop, variation, twist in radians (141 degrees). kGrassNearBladeSegments
// in grass_instance_stream.h is the same count (test_grass_lod.py).
const uint GRASS_NEAR_SEGMENTS = 3u;
const vec4 GRASS_NEAR_SHAPE = vec4(0.64, 0.35, 0.38, 2.4609142);
// The far draws' tuft blades: the same curve on the card in fewer segments
// (kGrassFarBladeSegments in grass_instance_stream.h).
const uint GRASS_FAR_SEGMENTS = 2u;

// Vertex k (0 .. 6 x count - 4) of a blade in `count` segments: its height
// along the blade (0..1) and its side (-1, 1; 0 at the tip). Every segment
// but the last is two triangles (row r left, r right, r+1 left; r+1 left,
// r right, r+1 right), the last one a triangle to the tip.
vec2 grass_row_side(uint k, uint count) {
    uint quads = count - 1u;
    float segments = float(count);
    if (k >= 6u * quads) {
        uint c = k - 6u * quads;
        if (c == 2u) return vec2(1.0, 0.0);
        return vec2(float(quads) / segments, c == 0u ? -1.0 : 1.0);
    }
    uint quad = k / 6u;
    uint q = k - quad * 6u;
    float up = (q == 2u || q == 3u || q == 5u) ? 1.0 : 0.0;
    float side = (q == 0u || q == 2u || q == 3u) ? -1.0 : 1.0;
    return vec2((float(quad) + up) / segments, side);
}

vec2 grass_near_row_side(uint k) {
    return grass_row_side(k, GRASS_NEAR_SEGMENTS);
}

// grass_blade_shape of grass_blade_shape.h without its sin hash: the blade's
// own random in [-1, 1] comes in. The offset of the point at height t from
// the root, in blade heights.
vec3 grass_near_blade_shape(float t, float phase, float signedRandom, vec2 rootAxis) {
    vec4 shape = GRASS_NEAR_SHAPE;
    float angle = signedRandom * shape.z * 3.14159265;
    float c = cos(angle);
    float s = sin(angle);
    vec2 direction = vec2(c * rootAxis.x - s * rootAxis.y, s * rootAxis.x + c * rootAxis.y);
    vec2 side = vec2(-direction.y, direction.x);
    float curve = shape.x * (1.0 + signedRandom * shape.z);
    vec2 offset = direction * (curve * t * t);
    offset += side * (0.35 * shape.z * sin(3.14159265 * t + phase) * t * t);
    return vec3(offset.x, t - shape.y * t * t * t, offset.y);
}

// How far up the blade a bend reaches at height t (0..1): the PC's
// bend_factor (grass_shader_sources.cpp).
float grass_bend_profile(float t) {
    return t * (0.65 + 0.35 * t);
}

// A tuft blade's height, of the tuft's, from its random r in [0, 1): the
// tuft's height on average (a PC blade is its anchor's height), +-15%.
float grass_blade_top(float r) {
    return 0.85 + 0.3 * r;
}

// The PC's evaluate_wind (grass_shader_sources.cpp) for a tuft at `base`
// with its phase: a 0.55 lean along the wind plus the wave and the gust, a
// lateral turbulence, the phase scaled by the randomness, at most the
// maximum bend. Colliders add to it in the tuft pass.
vec2 grass_wind_bend(vec3 base, float phase) {
    vec2 direction = grass_wind.xy;
    vec2 lateral = vec2(-direction.y, direction.x);
    float spatial = dot(base.xz, direction) * 0.01 * grass_wind_detail.y;
    float randomPhase = phase * clamp(grass_brightness.z, 0.0, 1.0);
    float primary = sin(grass_wind.w * grass_wind_detail.x + spatial + randomPhase);
    float gust = 0.5 + 0.5 * sin(grass_wind.w * grass_wind_detail.w * 6.2831853 + spatial * 0.21 + randomPhase * 0.37);
    float turbulence = sin(grass_wind.w * (grass_wind_detail.x * 1.71 + 0.31) - base.x * 0.017 + base.z * 0.013 +
                           randomPhase * 2.13);
    float along = grass_wind.z * (0.55 + primary * 0.30 + gust * grass_wind_detail.z * 0.45);
    float across = grass_wind.z * grass_tip_color.w * turbulence * 0.35;
    vec2 bend = direction * along + lateral * across;
    float bendLength = length(bend);
    if (bendLength > grass_shape.z && bendLength > 1.0e-6) bend *= grass_shape.z / bendLength;
    return bend;
}

// grass_blade_shape's twist: how far the blade's width axis turns at height t.
float grass_near_twist(float t, float signedRandom) {
    return GRASS_NEAR_SHAPE.w * signedRandom * t * t;
}

// A single blade's point at height t and side: test.9's blade (root corners
// at -+1.32 half widths along its facing, the tip leaning by `lean` and lower
// by 0.3 |lean|^2) bent along the PC curve, its width narrowing as the PC's
// (1 - 0.75 t, none at the tip) and turning by the twist.
vec3 grass_near_blade_point(vec3 root, vec2 facing, float halfWidth, float height, vec2 lean,
                            float phase, float signedRandom, float t, float side) {
    vec3 position = root + grass_near_blade_shape(t, phase, signedRandom, facing) * height;
    position.xz += lean * (height * grass_bend_profile(t));
    position.y -= dot(lean, lean) * height * 0.3 * t * t;
    float twist = grass_near_twist(t, signedRandom);
    float c = cos(twist);
    float s = sin(twist);
    vec2 across = vec2(c * facing.x - s * facing.y, s * facing.x + c * facing.y);
    float widthFactor = t >= 1.0 ? 0.0 : 1.0 - 0.75 * t;
    position.xz += across * (1.32 * halfWidth * widthFactor * side);
    return position;
}

// A tuft's blade on its card at height t (0..1) and side: the root at
// grass_lod_blade's triangle corners (centre -+ half base), then the PC blade
// seen on the card: the centre line to the tip bent sideways by `curve` t^2
// (card widths), the height drooping (t - droop t^3, of tipY), the width
// narrowing as the PC's (1 - 0.75 t, none at the tip). Card x and height y.
vec2 grass_tuft_xy(float centre, float halfBase, float tip, float tipY, float curve, float t, float side) {
    float width = t >= 1.0 ? 0.0 : 1.0 - 0.75 * t;
    float x = mix(centre, tip, t) + curve * t * t + side * halfBase * width;
    return vec2(x, tipY * (t - GRASS_NEAR_SHAPE.y * t * t * t));
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
