// deko3d per-pixel lighting declarations as shipped in 0.1.2-test.7 (gfx_deko3d_lighting.cpp at 9880e0dc):
// the reference of tests/switch/test_perpixel_lighting.py (same output for valid inputs).
// OOT3D_DEKO3D_PER_PIXEL declarations (gfx_deko3d_lighting.cpp)
layout(location=9) in vec3 pica_surface_normal;
layout(location=10) in vec4 pica_surface_tint;
struct PicaSurfaceLight {
    vec4 direction_enabled;
    vec4 diffuse;
    vec4 position_radius;
};
struct PicaScenePointLight {
    vec4 position_radius;
    vec4 color;
};
struct PicaPlacedLight {
    vec4 position_reach;
    vec4 color;
    vec4 axis_kind;
    vec4 tangent_shape;
};
layout(binding=5, std140) uniform PicaSurfaceLighting {
    vec4 material_diffuse;
    vec4 material_ambient;
    vec4 resolved_ambient;
    uvec4 control;
    vec4 view_to_world_linear[3];
    PicaSurfaceLight lights[3];
    vec4 environment_parameters;
    vec4 environment_ambient;
    vec4 environment_directions[2];
    vec4 environment_colors[2];
    vec4 scene_directions[2];
    vec4 scene_directional_colors[2];
    PicaScenePointLight scene_points[32];
    vec4 gi_sources;
    vec4 point_shadow;
    vec4 point_shadow_atlas;
    PicaPlacedLight placed_lights[16];
    vec4 water_view_translation;
    vec4 water_footprints[4];
    vec4 water_heights;
    vec4 water_optics;
    vec4 water_ripples;
    vec4 water_flow;
    vec4 water_rings[16];
    vec4 rim;
    vec4 rim_color;
    vec4 rim_direction;
    vec4 baked_reference_up;
    vec4 baked_reference_key;
} pica_surface;
vec3 pica_surface_unit(vec3 value) {
    float squared = dot(value, value);
    return squared > 1.0e-20 ? value * inversesqrt(squared) : vec3(0.0);
}
// Rim light (AssignPicaSurfaceRim; zero when off): rim.x intensity, .y
// sharpness, .z source (1 independent, 2 scene lights), .w coverage.
float pica_surface_rim_fresnel(vec3 n, vec3 v) {
    if (dot(n, n) < 0.5 || dot(v, v) < 0.5) return 0.0;
    return pow(clamp(1.0 - abs(dot(n, v)), 0.0, 1.0), pica_surface.rim.y);
}
float pica_surface_rim_weight(vec3 n, vec3 v, vec3 l) {
    if (pica_surface.rim.x <= 0.0 || pica_surface.rim.z != 2.0) return 0.0;
    l = pica_surface_unit(l);
    if (dot(l, l) < 0.5) return 0.0;
    float back = smoothstep(-0.25, 0.5, dot(l, -v));
    float wrap = clamp((dot(n, l) + 0.5) / 1.5, 0.0, 1.0);
    return pica_surface.rim.x * pica_surface_rim_fresnel(n, v) * back * wrap;
}
vec3 pica_surface_rim_independent(vec3 n, vec3 v) {
    if (pica_surface.rim.x <= 0.0 || pica_surface.rim.z != 1.0) return vec3(0.0);
    float projected = dot(n.xy, n.xy);
    float side = projected > 1.0e-8
        ? smoothstep(-0.25, 0.75, dot(n.xy * inversesqrt(projected), pica_surface.rim_direction.xy)) : 0.0;
    float coverage = mix(side, 1.0, pica_surface.rim.w);
    return pica_surface.rim_color.rgb * pica_surface.rim.x * pica_surface_rim_fresnel(n, v) * coverage;
}
float pica_surface_diffuse_rim(vec3 n, vec3 l) {
    float diffuse = max(dot(n, l), 0.0);
    if (pica_surface.rim.x > 0.0) diffuse += pica_surface_rim_weight(n, pica_surface_unit(pica_view), l);
    return diffuse;
}
vec3 pica_surface_to_world(vec3 view_vector) {
    return vec3(dot(pica_surface.view_to_world_linear[0].xyz, view_vector),
                dot(pica_surface.view_to_world_linear[1].xyz, view_vector),
                dot(pica_surface.view_to_world_linear[2].xyz, view_vector));
}
void pica_surface_light(int slot, out vec3 direction, out vec3 radiance) {
    PicaSurfaceLight light = pica_surface.lights[slot];
    direction = light.direction_enabled.xyz;
    radiance = light.diffuse.rgb;
    if (light.position_radius.w > 0.0) {
        vec3 delta = light.position_radius.xyz + pica_view;
        vec3 world_delta = pica_surface_to_world(delta);
        radiance *= max(0.0, 1.0 - dot(world_delta, world_delta) /
                                  (light.position_radius.w * light.position_radius.w));
        direction = pica_surface_unit(delta);
    }
}
vec3 pica_surface_scene_point(uint slot, vec3 normal_view) {
    PicaScenePointLight light = pica_surface.scene_points[slot];
    vec3 delta = light.position_radius.xyz + pica_view;
    vec3 world_delta = pica_surface_to_world(delta);
    float attenuation = max(0.0, 1.0 - dot(world_delta, world_delta) /
                            (light.position_radius.w * light.position_radius.w));
    return pica_surface_diffuse_rim(normal_view, pica_surface_unit(delta)) * light.color.rgb * attenuation;
}
// Points 3 and 4 (a room with Navi, a fairy and torches) behind their own
// uniform branch; same sums in the same order as one loop.
vec3 pica_surface_scene_point_primary(vec3 normal_view) {
    vec3 color = vec3(0.0);
    uint count = min(pica_surface.control.y, 4u);
    for (uint slot = 0u; slot < min(count, 2u); ++slot)
        color += pica_surface_scene_point(slot, normal_view);
    if (count > 2u) {
        for (uint slot = 2u; slot < count; ++slot)
            color += pica_surface_scene_point(slot, normal_view);
    }
    return color;
}
vec3 pica_surface_scene_directional_primary(vec3 normal_view) {
    vec3 color = vec3(0.0);
    uint count = min(pica_surface.control.z, 2u);
    for (uint slot = 0u; slot < count; ++slot)
        color += pica_surface_diffuse_rim(normal_view, pica_surface_unit(pica_surface.scene_directions[slot].xyz)) *
            pica_surface.scene_directional_colors[slot].rgb;
    return color;
}
vec3 pica_surface_native_primary(vec3 normal_view) {
    vec3 direct = vec3(0.0);
    bool native_direct = pica_surface.environment_parameters.w == 0.0 ||
        pica_surface.environment_parameters.x != 0.0;
    for (int slot = 0; pica_surface.control.x != 0u && native_direct && slot < 3; ++slot) {
        if (pica_surface.lights[slot].direction_enabled.w == 0.0) continue;
        vec3 direction, radiance;
        pica_surface_light(slot, direction, radiance);
        direct += pica_surface_diffuse_rim(normal_view, direction) * radiance;
    }
    vec3 color = direct * pica_surface.material_diffuse.rgb;
    if (pica_surface.environment_parameters.w != 0.0) color *= pica_surface.environment_parameters.x;
    color += pica_surface.resolved_ambient.rgb;
    color += pica_surface_rim_independent(normal_view, pica_surface_unit(pica_view));
    if (pica_surface.environment_parameters.w != 0.0) {
        for (int slot = 0; slot < 2; ++slot) {
            if (pica_surface.environment_directions[slot].w <= 0.0) continue;
            color += pica_surface_diffuse_rim(normal_view, pica_surface_unit(pica_surface.environment_directions[slot].xyz)) *
                pica_surface.environment_colors[slot].rgb * pica_surface.environment_directions[slot].w;
        }
    }
    color += pica_surface_scene_point_primary(normal_view);
    color += pica_surface_scene_directional_primary(normal_view);
    return min(abs(color * pica_surface_tint.rgb), vec3(1.0));
}
// Baked outdoor colour is a lit reference: ambient and the directionals
// redistribute it; a horizontal face keeps its native colour.
float pica_surface_baked_diffuse(vec3 normal_view, vec3 up, vec3 direction) {
    float diffuse = min(max(dot(normal_view, direction), 0.0), max(dot(up, direction), 0.0));
    // Every rim evaluation sits behind the rim's uniform branch.
    if (pica_surface.rim.x > 0.0)
        diffuse += pica_surface_rim_weight(normal_view, pica_surface_unit(pica_view), direction);
    return diffuse;
}
vec3 pica_surface_baked_response(vec3 normal_view, out vec3 reference) {
    vec3 up = pica_surface_unit(pica_surface.baked_reference_up.xyz);
    vec3 response = pica_surface.control.x != 0u ? pica_surface.resolved_ambient.rgb
        : vec3(pica_surface.environment_parameters.y) + pica_surface.environment_ambient.rgb;
    reference = response;
    if (pica_surface.environment_parameters.w != 0.0) {
        for (int slot = 0; slot < 2; ++slot) {
            vec3 direction = pica_surface_unit(pica_surface.environment_directions[slot].xyz);
            vec3 color = pica_surface.environment_colors[slot].rgb;
            float gain = pica_surface.environment_directions[slot].w;
            response += color * gain * pica_surface_baked_diffuse(normal_view, up, direction);
            reference += (slot == 0 ? pica_surface.baked_reference_key.rgb : color * gain) *
                max(dot(up, direction), 0.0);
        }
    }
    uint count = min(pica_surface.control.z, 2u);
    for (uint slot = 0u; slot < count; ++slot) {
        vec3 direction = pica_surface_unit(pica_surface.scene_directions[slot].xyz);
        vec3 color = pica_surface.scene_directional_colors[slot].rgb;
        response += color * pica_surface_baked_diffuse(normal_view, up, direction);
        reference += color * max(dot(up, direction), 0.0);
    }
    return response;
}
vec3 pica_surface_primary(vec3 native_color, vec3 normal_view) {
    if (pica_surface.baked_reference_up.w != 0.0) {
        vec3 reference;
        vec3 response = pica_surface_baked_response(normal_view, reference);
        vec3 color = native_color * response / max(reference, vec3(1.0e-6));
        color += (pica_surface_scene_point_primary(normal_view) +
                  pica_surface_rim_independent(normal_view, pica_surface_unit(pica_view))) * pica_surface_tint.rgb;
        return clamp(color, 0.0, 1.0);
    }
    if (pica_surface.control.x != 0u ||
        (pica_surface.environment_parameters.w != 0.0 && (pica_surface.control.w & 1u) == 0u))
        return pica_surface_native_primary(normal_view);
    if (pica_surface.environment_parameters.w != 0.0)
        return min(native_color + pica_surface_native_primary(normal_view), vec3(1.0));
    return min(native_color + (pica_surface_scene_point_primary(normal_view) +
               pica_surface_scene_directional_primary(normal_view)) * pica_surface_tint.rgb, vec3(1.0));
}
