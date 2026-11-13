#version 460
// deko3d grass, config/grass_lod on (SWITCH_PORT.md "Grass on deko3d: cost
// per tuft"): the per-tuft work of grass_vsh.glsl, once per tuft instead of
// once per vertex (15 times). One point per tuft, the rasterizer off; it
// writes the tuft's GrassTuft (grass_lod_common.glsl) for grass_lod_vsh.glsl.
// Every frame (in-between frames included), with this frame's camera, wind
// and colliders: the cull worker's instances are drawn for several frames.
//
// Same instance stream and attributes as grass_vsh.glsl, read per vertex.
layout(location = 0) in vec4 grass_position_height; // world base, tuft height
layout(location = 1) in uint grass_axis;            // snorm16 x2 width axis (x, z)
layout(location = 2) in uint grass_normal;          // octahedral snorm16 x2
layout(location = 3) in uint grass_color;           // RGBA8 ground colour
layout(location = 4) in uint grass_width_phase_fade;

#include "grass_lod_common.glsl"

layout(std430, binding = 0) writeonly buffer GrassTufts {
    GrassTuft grass_tufts[];
};

void main() {
    // Base and size as grass_vsh.glsl; the wind is the PC's (grass_wind_bend).
    vec3 base = grass_position_height.xyz;
    float fade = float(grass_width_phase_fade >> 24u) / 255.0;
    float height = grass_position_height.w * smoothstep(0.0, 1.0, fade);
    float halfWidth = unpackHalf2x16(grass_width_phase_fade).x;
    float phase = float((grass_width_phase_fade >> 16u) & 0xffu) / 255.0 * 6.2831853;
    float bladeCount = max(grass_shape.x, 1.0);
    float cardHalf = halfWidth * bladeCount * grass_shape.y;
    float distanceToEye = length(grass_eye.xyz - base);

    vec2 bend = grass_wind_bend(base, phase);
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

    // The tuft's camera-facing card and its light (the tuft normal of grass_vsh.glsl).
    vec3 toEye = grass_eye.xyz - base;
    vec2 flatToEye = length(toEye.xz) > 1.0e-3 ? normalize(toEye.xz) : vec2(0.0, 1.0);
    vec2 sideHalf = vec2(-flatToEye.y, flatToEye.x) * cardHalf;
    vec3 normal = normalize(vec3(flatToEye.x, 0.0, flatToEye.y) * 0.5 + vec3(0.0, 1.0, 0.0));
    vec3 light = grass_light(normalize(normal + grass_decode_normal(grass_normal)));

    // grass_vsh.glsl's colour is linear in the height y (0 root, 1 tip):
    // root and tip, lit, and the vertices interpolate them.
    vec4 ground = unpackUnorm4x8(grass_color);
    vec3 root = mix(grass_root_color.rgb, ground.rgb * grass_brightness.x, grass_root_color.w) * light;
    vec3 tip = mix(grass_tip_color.rgb, ground.rgb * grass_brightness.y, grass_root_color.w) * light;

    GrassTuft tuft;
    tuft.base_height = vec4(base, height);
    tuft.side_bend = vec4(sideHalf, bend);
    tuft.root_seed = vec4(root, uintBitsToFloat(grass_tuft_seed(base, grass_width_phase_fade)));
    tuft.tip_level = vec4(tip, grass_lod_coordinate(distanceToEye, halfWidth));
    grass_tufts[gl_VertexID] = tuft;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
