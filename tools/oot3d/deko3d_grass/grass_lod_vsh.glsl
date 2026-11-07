#version 460
// deko3d grass, config/grass_lod on (SWITCH_PORT.md "Grass on deko3d: cost
// per tuft"): the blades of the tufts grass_tuft_vsh.glsl prepared this
// frame. No instancing: vertex v is corner v % 3 of blade (v / 3) % blades
// of tuft v / (3 x blades) (vertices of several tufts share a warp), read
// from the tuft buffer bound at the draw's first tuft.
// - Far draws (default): 5, 3 or 1 blades per tuft (1 / grass_lod_draw.x),
//   turning into grass_lod_draw.y blades as the tuft's level coordinate
//   passes grass_lod_draw.w (grass_lod_blade). Every tuft is a whole tuft
//   (the cull worker leaves single blades and their turn into tufts to the
//   near draw). The tuft's lit root and tip colours, fog per vertex.
// - GRASS_LOD_NEAR: 5 blades, single blades within grass_distance.z turning
//   into the tuft from there to grass_distance.w as grass_vsh.glsl does,
//   their per-blade light per vertex (the instance read for its width
//   axis, ground normal and colour).
// The fragment programs are grass_fsh.glsl's.

#include "grass_lod_common.glsl"

// `coherent`: read past the L1 (written by the tuft pass of this frame).
layout(std430, binding = 0) coherent readonly buffer GrassTufts {
    GrassTuft grass_tufts[];
};
#ifdef GRASS_LOD_NEAR
// The 32-byte GrassStreamInstance of each tuft: two uvec4.
layout(std430, binding = 1) coherent readonly buffer GrassInstances {
    uvec4 grass_instances[];
};
#endif

layout(location = 0) out vec4 grass_shade; // lit and fogged colour, alpha 1

void main() {
    // v / (3 x blades) in float: exact for every v of a draw (at most 65536
    // tufts x 15 vertices; test_grass_lod.py checks each one).
    uint vertex = uint(gl_VertexID);
    uint index = uint((float(vertex) + 0.5) * uintBitsToFloat(grass_lod_index.x));
    uint local = vertex - index * grass_lod_index.y;
    uint blade = local / 3u;
    uint corner = local - blade * 3u;
    GrassTuft tuft = grass_tufts[index];
    vec3 base = tuft.base_height.xyz;
    float height = tuft.base_height.w;
    vec2 sideHalf = tuft.side_bend.xy;
    vec2 bend = tuft.side_bend.zw;
    vec3 random = grass_blade_random(floatBitsToUint(tuft.root_seed.w), blade);
    float b = float(blade);

    // The tuft (grass_vsh.glsl's tuft branch, its layout from grass_lod_blade).
    float morph = clamp(tuft.tip_level.w - grass_lod_draw.w, 0.0, 1.0);
    float centre, halfBase, tipX;
    grass_lod_blade(b, grass_lod_draw.x, grass_lod_draw.y, grass_lod_draw.z, morph, random, centre, halfBase, tipX);
    float x = corner == 2u ? tipX : centre + (corner == 0u ? -halfBase : halfBase);
    float y = corner == 2u ? 0.7 + 0.3 * random.z : 0.0;
    vec3 position = base + vec3(sideHalf.x, 0.0, sideHalf.y) * (x * 2.0 - 1.0) + vec3(0.0, height * y, 0.0);
    position.xz += bend * (height * y * y);
    position.y -= dot(bend, bend) * height * 0.3 * y * y;
    vec3 colour = mix(tuft.root_seed.rgb, tuft.tip_level.rgb, y);

#ifdef GRASS_LOD_NEAR
    float tuftShare = smoothstep(grass_distance.z, grass_distance.w, length(grass_eye.xyz - base));
    if (tuftShare < 1.0) {
        // grass_vsh.glsl's single blade, mixed into the tuft by tuftShare.
        uvec4 instance = grass_instances[2u * index + 1u]; // axis, normal, colour, width/phase/fade
        float halfWidth = unpackHalf2x16(instance.w).x;
        float bladeCount = max(grass_shape.x, 1.0);
        float cardHalf = halfWidth * bladeCount * grass_shape.y;
        vec2 axis = unpackSnorm2x16(instance.x);
        if (dot(axis, axis) < 1.0e-6) axis = vec2(1.0, 0.0);
        axis = normalize(axis);
        vec2 across = vec2(-axis.y, axis.x);
        vec2 offset = axis * (((b + 0.5) / bladeCount - 0.5) * 2.0 * cardHalf) +
                      across * ((random.x - 0.5) * cardHalf * 0.6);
        float angle = b * 1.3 + random.y * 3.14159;
        vec2 facing = vec2(cos(angle), sin(angle));
        float bladeHeight = height * (0.7 + 0.3 * random.z);
        vec2 lean = (random.yz - 0.5) * 0.35 + bend;
        vec3 root = base + vec3(offset.x, 0.0, offset.y);
        vec3 bladePosition;
        if (corner == 2u) {
            bladePosition = root + vec3(lean.x * bladeHeight, bladeHeight * (1.0 - 0.3 * dot(lean, lean)), lean.y * bladeHeight);
        } else {
            bladePosition = root + vec3(facing.x, 0.0, facing.y) * (corner == 0u ? -1.32 * halfWidth : 1.32 * halfWidth);
        }
        vec3 bladeNormal = normalize(vec3(-facing.y, 0.6, facing.x));
        float bladeY = corner == 2u ? 1.0 : 0.0;
        // The tuft's normal: its card faces the camera (sideHalf is the
        // camera direction turned a quarter, times the card's half width).
        vec2 flatToEye = dot(sideHalf, sideHalf) > 0.0 ? normalize(vec2(sideHalf.y, -sideHalf.x)) : vec2(0.0, 1.0);
        vec3 tuftNormal = normalize(vec3(flatToEye.x, 0.0, flatToEye.y) * 0.5 + vec3(0.0, 1.0, 0.0));
        position = mix(bladePosition, position, tuftShare);
        vec3 normal = normalize(mix(bladeNormal, tuftNormal, tuftShare));
        y = mix(bladeY, y, tuftShare);
        vec4 ground = unpackUnorm4x8(instance.z);
        vec3 authored = mix(grass_root_color.rgb, grass_tip_color.rgb, y);
        vec3 fromGround = ground.rgb * mix(grass_brightness.x, grass_brightness.y, y);
        colour = mix(authored, fromGround, grass_root_color.w) *
                 grass_light(normalize(normal + grass_decode_normal(instance.y)));
    }
#endif
    grass_emit(position, colour, grass_shade);
}
