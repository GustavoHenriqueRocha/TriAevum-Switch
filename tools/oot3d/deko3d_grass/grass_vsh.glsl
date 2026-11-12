#version 460
// deko3d grass (Switch, SWITCH_PORT.md "Grass on deko3d"): one instance per
// tuft, 3 vertices per blade (a triangle list, BladesPerTuft blades).
// - Tufts: the blades stand side by side in one plane facing the camera,
//   each narrowing to its tip. They used to be a 4-vertex card the fragment
//   program cut into blades with discard: ~2/3 of its pixels were shaded then
//   thrown away, and the discard with depth writes turned early depth testing
//   off, so tufts hidden behind tufts were shaded too. Triangles draw only
//   blade pixels, with early depth testing.
// - Blades (within grass_distance.z of the camera): each blade with its own
//   facing and lean, one segment. From grass_distance.z to grass_distance.w
//   every vertex moves from its blade to its tuft place (no tuft changes at
//   once as the camera moves); the stream's two kinds only split the draws.
// Light (the source draw's PICA vertex lights) and fog (its PICA fog LUT)
// are evaluated here, per vertex; the fragment program only writes the colour.
//
// Instance stream: GrassStreamInstance (grass_instance_stream.h), 32 bytes.
layout(location = 0) in vec4 grass_position_height; // world base, tuft height
layout(location = 1) in uint grass_axis;            // snorm16 x2 width axis (x, z)
layout(location = 2) in uint grass_normal;          // octahedral snorm16 x2
layout(location = 3) in uint grass_color;           // RGBA8 ground colour
layout(location = 4) in uint grass_width_phase_fade;

// Per placement (the source draw): GrassPlacementUniforms in gfx_deko3d_grass.cpp.
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

// Per frame and kind: GrassFrameUniforms in gfx_deko3d_grass.cpp.
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

layout(location = 0) out vec4 grass_shade; // lit and fogged colour, alpha 1

vec3 grass_decode_normal(uint bits) {
    vec2 oct = unpackSnorm2x16(bits);
    vec3 n = vec3(oct, 1.0 - abs(oct.x) - abs(oct.y));
    float fold = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -fold : fold;
    n.y += n.y >= 0.0 ? -fold : fold;
    return normalize(n);
}

float grass_hash(float value) {
    return fract(sin(value * 12.9898 + 78.233) * 43758.5453);
}

// The PICA depth and fog of the native fragment program (pica_depth and
// fog_index in oot3d_native_pica_fragment_shader_gen.cpp), per vertex.
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

// EvaluateGrassLighting (grass_shading_environment.cpp): ambient plus diffuse
// with |N.L| (the blades are two-sided), clamped.
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

void main() {
    vec3 base = grass_position_height.xyz;
    float fade = float(grass_width_phase_fade >> 24u) / 255.0;
    float height = grass_position_height.w * smoothstep(0.0, 1.0, fade);
    float halfWidth = unpackHalf2x16(grass_width_phase_fade).x;
    float phase = float((grass_width_phase_fade >> 16u) & 0xffu) / 255.0 * 6.2831853;
    float bladeCount = max(grass_shape.x, 1.0);
    float cardHalf = halfWidth * bladeCount * grass_shape.y;
    float distanceToEye = length(grass_eye.xyz - base);
    // 0: single blades, 1: tuft (smoothstep over grass_distance.z..w).
    float tuftShare = smoothstep(grass_distance.z, grass_distance.w, distanceToEye);

    // Wind: a travelling wave plus a slow gust, as a fraction of the height.
    float along = dot(base.xz, grass_wind.xy) * grass_wind_detail.y * 0.01;
    float wave = sin(grass_wind.w * grass_wind_detail.x + along + phase);
    float gust = 0.5 + 0.5 * sin(grass_wind.w * grass_wind_detail.w * 6.2831853 + along * 0.3);
    vec2 bend = grass_wind.xy * grass_wind.z * (0.6 * wave + grass_wind_detail.z * gust);
    // Link and Epona: tufts inside a capsule's reach lean away from its axis.
    int colliders = int(grass_brightness.w);
    for (int i = 0; i < 4; ++i) {
        if (i >= colliders) break;
        vec3 centre = grass_collider[i].xyz;
        float halfHeight = grass_collider_extent[i].x;
        if (base.y > centre.y + halfHeight || base.y + height < centre.y - halfHeight) continue;
        vec2 away = base.xz - centre.xz;
        float dist = length(away);
        float reach = grass_collider[i].w + cardHalf + height * 0.25;
        if (dist >= reach) continue;
        float weight = 1.0 - dist / reach;
        weight = weight * weight * (3.0 - 2.0 * weight);
        bend += (dist > 1.0e-3 ? away / dist : vec2(1.0, 0.0)) * weight * 1.5;
    }
    float bendLength = length(bend);
    if (bendLength > grass_shape.z && bendLength > 0.0) bend *= grass_shape.z / bendLength;

    vec3 toEye = grass_eye.xyz - base;
    vec2 flatToEye = length(toEye.xz) > 1.0e-3 ? normalize(toEye.xz) : vec2(0.0, 1.0);
    vec3 position = base;
    vec3 normal = vec3(0.0, 1.0, 0.0);
    float y = 0.0; // 0 at the root, up to 1 at the tallest tip (the colour ramp)
    float seed = fract(phase * 0.1591549 + base.x * 0.0137 + base.z * 0.0071);
    // Base left, base right, tip of blade `blade`.
    uint blade = uint(gl_VertexID) / 3u;
    uint corner = uint(gl_VertexID) % 3u;
    float b = float(blade);
    float r0 = grass_hash(seed * 17.0 + b);
    float r1 = grass_hash(seed * 31.0 + b * 1.7);
    float r2 = grass_hash(seed * 53.0 + b * 2.3);
    if (tuftShare > 0.0) {
        // The tuft's plane faces the camera (cylindrical billboard), the
        // blades side by side across it: x 0-1 across, y 0-1 up. Blade b
        // stands near the middle of its slot, its base 0.84 slot wide, its
        // tip at 0.7-1 of the height and leaning by up to 0.3 slot.
        float centre = (b + 0.5 + (r0 - 0.5) * 0.5) / bladeCount;
        float halfBase = 0.42 / bladeCount;
        float top = 0.7 + 0.3 * r2;
        float x = corner == 2u ? centre + (r1 - 0.5) * 0.6 / bladeCount
                               : centre + (corner == 0u ? -halfBase : halfBase);
        y = corner == 2u ? top : 0.0;
        vec3 side = vec3(-flatToEye.y, 0.0, flatToEye.x);
        position = base + side * ((x * 2.0 - 1.0) * cardHalf) + vec3(0.0, height * y, 0.0);
        position.xz += bend * (height * y * y);
        position.y -= dot(bend, bend) * height * 0.3 * y * y;
        normal = normalize(vec3(flatToEye.x, 0.0, flatToEye.y) * 0.5 + vec3(0.0, 1.0, 0.0));
    }
    if (tuftShare < 1.0) {
        // One segment per blade, each with its own facing and lean. A blade
        // facing anywhere shows 2/pi of its width on average: 1.32 x the half
        // width (0.84 x pi / 2) keeps a tuft's average screen width.
        vec2 axis = unpackSnorm2x16(grass_axis);
        if (dot(axis, axis) < 1.0e-6) axis = vec2(1.0, 0.0);
        axis = normalize(axis);
        vec2 across = vec2(-axis.y, axis.x);
        vec2 offset = axis * (((b + 0.5) / bladeCount - 0.5) * 2.0 * cardHalf) +
                      across * ((r0 - 0.5) * cardHalf * 0.6);
        float angle = b * 1.3 + r1 * 3.14159;
        vec2 facing = vec2(cos(angle), sin(angle));
        float bladeHeight = height * (0.7 + 0.3 * r2);
        vec2 lean = (vec2(r1, r2) - 0.5) * 0.35 + bend;
        vec3 root = base + vec3(offset.x, 0.0, offset.y);
        vec3 bladePosition;
        if (corner == 2u) {
            bladePosition = root + vec3(lean.x * bladeHeight, bladeHeight * (1.0 - 0.3 * dot(lean, lean)), lean.y * bladeHeight);
        } else {
            bladePosition = root + vec3(facing.x, 0.0, facing.y) * (corner == 0u ? -1.32 * halfWidth : 1.32 * halfWidth);
        }
        vec3 bladeNormal = normalize(vec3(-facing.y, 0.6, facing.x));
        float bladeY = corner == 2u ? 1.0 : 0.0;
        position = mix(bladePosition, position, tuftShare);
        normal = normalize(mix(bladeNormal, normal, tuftShare));
        y = mix(bladeY, y, tuftShare);
    }

    vec4 ground = unpackUnorm4x8(grass_color);
    vec3 authored = mix(grass_root_color.rgb, grass_tip_color.rgb, y);
    vec3 fromGround = ground.rgb * mix(grass_brightness.x, grass_brightness.y, y);
    vec3 colour = mix(authored, fromGround, grass_root_color.w);
    vec3 groundNormal = grass_decode_normal(grass_normal);
    colour *= grass_light(normalize(normal + groundNormal));

    vec4 clip = grass_position_to_clip * vec4(position, 1.0);
    // Fog per vertex (the factor was interpolated per vertex before too).
    if (grass_fog_color.w > 0.5) colour = mix(grass_fog_color.rgb, colour, grass_fog(grass_pica_depth(clip)));
    grass_shade = vec4(colour, 1.0);
    // Window depth = the PICA depth of the source draw. Without w-buffering
    // that mapping (z/w * scale + offset) is linear in z/w, so it goes into
    // clip z (z_gl = (2 d - 1) w) and the rasterizer interpolates it exactly
    // as the native fragment program computes it: no gl_FragDepth, early
    // depth testing stays on. With the identity mapping (scale -1, offset 0)
    // this is the dialect's epilogue, z_gl = -2 z - w. W-buffered draws use
    // the fragment variant that writes gl_FragDepth.
    float zGl = grass_depth.z > 0.5 ? -2.0 * clip.z - clip.w
                                     : 2.0 * (clip.z * grass_depth.x + grass_depth.y * clip.w) - clip.w;
    gl_Position = vec4(clip.x, clip.y, zGl, clip.w);
}
