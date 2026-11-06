#version 460
// deko3d grass (Switch, SWITCH_PORT.md "Grass on deko3d"): one instance per
// tuft. Tufts far from the camera are a 4-vertex card facing the camera
// (triangle strip) that the fragment shader cuts into its blades; tufts
// within grass_distance.z (GrassFrame) are drawn as those blades, one triangle each
// (3 vertices per blade). Light (the source draw's PICA vertex lights) and
// fog (its PICA fog LUT) are evaluated here, per vertex.
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
    vec4 grass_distance;          // draw distance, fade start, single-blade distance, blades (0/1)
    vec4 grass_root_color;        // rgb, ground colour influence
    vec4 grass_tip_color;         // rgb, unused
    vec4 grass_brightness;        // root, tip, unused, collider count
    vec4 grass_collider[4];       // capsule centre xyz, radius
    vec4 grass_collider_extent[4];// half height, unused
};

layout(location = 0) out vec4 grass_shade;       // lit colour, alpha 1
layout(location = 1) out vec2 grass_card;        // across (0-1), up (0-1)
layout(location = 2) out float grass_fog_factor; // 1 = no fog
layout(location = 3) flat out vec4 grass_tuft;   // blades, unused, blade mode, unused
// The card's blades for the fragment cut, per tuft (not per fragment):
// x centre across the card, y 1 / top height, z lean per unit of height.
layout(location = 4) flat out vec4 grass_blades[5];

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
    bool blades = grass_distance.w > 0.5;

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
    vec3 position;
    vec3 normal;
    vec2 card;
    float seed = fract(phase * 0.1591549 + base.x * 0.0137 + base.z * 0.0071);
    if (!blades) {
        // Cylindrical billboard: strip 0 (-1, 0), 1 (1, 0), 2 (-1, 1), 3 (1, 1).
        uint corner = uint(gl_VertexID) & 3u;
        float x = (corner & 1u) != 0u ? 1.0 : -1.0;
        float y = (corner & 2u) != 0u ? 1.0 : 0.0;
        vec3 side = vec3(-flatToEye.y, 0.0, flatToEye.x);
        position = base + side * (x * cardHalf) + vec3(0.0, height * y, 0.0);
        position.xz += bend * (height * y * y);
        position.y -= dot(bend, bend) * height * 0.3 * y * y;
        normal = normalize(vec3(flatToEye.x, 0.0, flatToEye.y) * 0.5 + vec3(0.0, 1.0, 0.0));
        card = vec2(x * 0.5 + 0.5, y);
    } else {
        // One segment per blade: base left, base right, tip.
        uint blade = uint(gl_VertexID) / 3u;
        uint corner = uint(gl_VertexID) % 3u;
        float b = float(blade);
        float r0 = grass_hash(seed * 17.0 + b);
        float r1 = grass_hash(seed * 31.0 + b * 1.7);
        float r2 = grass_hash(seed * 53.0 + b * 2.3);
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
        if (corner == 2u) {
            position = root + vec3(lean.x * bladeHeight, bladeHeight * (1.0 - 0.3 * dot(lean, lean)), lean.y * bladeHeight);
        } else {
            position = root + vec3(facing.x, 0.0, facing.y) * (corner == 0u ? -halfWidth : halfWidth);
        }
        normal = normalize(vec3(-facing.y, 0.6, facing.x));
        card = vec2(corner == 1u ? 1.0 : 0.0, corner == 2u ? 1.0 : 0.0);
    }

    vec4 ground = unpackUnorm4x8(grass_color);
    float y = card.y;
    vec3 authored = mix(grass_root_color.rgb, grass_tip_color.rgb, y);
    vec3 fromGround = ground.rgb * mix(grass_brightness.x, grass_brightness.y, y);
    vec3 colour = mix(authored, fromGround, grass_root_color.w);
    vec3 groundNormal = grass_decode_normal(grass_normal);
    grass_shade = vec4(colour * grass_light(normalize(normal + groundNormal)), 1.0);
    grass_card = card;
    grass_tuft = vec4(bladeCount, 0.0, blades ? 1.0 : 0.0, 0.0);
    for (int i = 0; i < 5; ++i) {
        float b = float(i);
        float r0 = grass_hash(seed * 17.0 + b);
        float r1 = grass_hash(seed * 31.0 + b * 1.7);
        float top = 0.7 + 0.3 * grass_hash(seed * 53.0 + b * 2.3);
        grass_blades[i] = vec4((b + 0.5 + (r0 - 0.5) * 0.5) / bladeCount, 1.0 / top, (r1 - 0.5) * 0.6 / bladeCount, 0.0);
    }

    vec4 clip = grass_position_to_clip * vec4(position, 1.0);
    grass_fog_factor = grass_fog(grass_pica_depth(clip));
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
