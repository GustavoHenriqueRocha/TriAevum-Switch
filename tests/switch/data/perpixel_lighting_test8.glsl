// deko3d per-pixel lighting declarations as shipped in 0.1.2-test.8 (gfx_deko3d_lighting.cpp at 644ba5b2):
// the exact reference of tests/switch/test_perpixel_lighting.py (the same floats for valid inputs).
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
// The fragment's interpolants, read once at the top of the per-pixel body
// (before any branch) and passed down: no input is interpolated inside a
// loop or a lane-dependent branch (SWITCH_PORT.md "Per-pixel dots"). A plain
// copy would be propagated back to every use (one interpolation per use, deep
// in the light loops): the clamp to the finite range keeps a computed value,
// and is the identity on every finite input.
struct PicaSurfaceFragment {
    vec3 normal;    // unit view-space normal, or 0
    vec3 view;      // pica_view
    vec3 view_unit; // unit pica_view, or 0
    vec3 tint;
};
PicaSurfaceFragment pica_surface_fragment() {
    const vec3 finite = vec3(3.4028235e38);
    PicaSurfaceFragment fragment;
    fragment.normal = pica_surface_unit(pica_surface_normal);
    fragment.view = clamp(pica_view, -finite, finite);
    fragment.view_unit = pica_surface_unit(fragment.view);
    fragment.tint = clamp(pica_surface_tint.rgb, -finite, finite);
    return fragment;
}
// Rim light (AssignPicaSurfaceRim; zero when off): rim.x intensity, .y
// sharpness, .z source (1 independent, 2 scene lights), .w coverage. The
// per-fragment early returns are selects (same value, no divergent branch).
float pica_surface_rim_fresnel(vec3 n, vec3 v) {
    float fresnel = pow(clamp(1.0 - abs(dot(n, v)), 0.0, 1.0), pica_surface.rim.y);
    return any(lessThan(vec2(dot(n, n), dot(v, v)), vec2(0.5))) ? 0.0 : fresnel;
}
float pica_surface_rim_weight(vec3 n, vec3 v, vec3 l) {
    if (pica_surface.rim.x <= 0.0 || pica_surface.rim.z != 2.0) return 0.0;
    l = pica_surface_unit(l);
    float back = smoothstep(-0.25, 0.5, dot(l, -v));
    float wrap = clamp((dot(n, l) + 0.5) / 1.5, 0.0, 1.0);
    float weight = pica_surface.rim.x * pica_surface_rim_fresnel(n, v) * back * wrap;
    return dot(l, l) < 0.5 ? 0.0 : weight;
}
vec3 pica_surface_rim_independent(vec3 n, vec3 v) {
    if (pica_surface.rim.x <= 0.0 || pica_surface.rim.z != 1.0) return vec3(0.0);
    float projected = dot(n.xy, n.xy);
    float side = smoothstep(-0.25, 0.75, dot(n.xy * inversesqrt(max(projected, 1.0e-8)), pica_surface.rim_direction.xy));
    side = projected > 1.0e-8 ? side : 0.0;
    float coverage = mix(side, 1.0, pica_surface.rim.w);
    return pica_surface.rim_color.rgb * pica_surface.rim.x * pica_surface_rim_fresnel(n, v) * coverage;
}
float pica_surface_diffuse_rim(PicaSurfaceFragment fragment, vec3 l) {
    float diffuse = max(dot(fragment.normal, l), 0.0);
    if (pica_surface.rim.x > 0.0) diffuse += pica_surface_rim_weight(fragment.normal, fragment.view_unit, l);
    return diffuse;
}
vec3 pica_surface_to_world(vec3 view_vector) {
    return vec3(dot(pica_surface.view_to_world_linear[0].xyz, view_vector),
                dot(pica_surface.view_to_world_linear[1].xyz, view_vector),
                dot(pica_surface.view_to_world_linear[2].xyz, view_vector));
}
void pica_surface_light(int slot, vec3 view, out vec3 direction, out vec3 radiance) {
    PicaSurfaceLight light = pica_surface.lights[slot];
    direction = light.direction_enabled.xyz;
    radiance = light.diffuse.rgb;
    if (light.position_radius.w > 0.0) {
        vec3 delta = light.position_radius.xyz + view;
        vec3 world_delta = pica_surface_to_world(delta);
        radiance *= max(0.0, 1.0 - dot(world_delta, world_delta) /
                                  (light.position_radius.w * light.position_radius.w));
        direction = pica_surface_unit(delta);
    }
}
vec3 pica_surface_scene_point(uint slot, PicaSurfaceFragment fragment) {
    PicaScenePointLight light = pica_surface.scene_points[slot];
    vec3 delta = light.position_radius.xyz + fragment.view;
    vec3 world_delta = pica_surface_to_world(delta);
    float attenuation = max(0.0, 1.0 - dot(world_delta, world_delta) /
                            (light.position_radius.w * light.position_radius.w));
    return pica_surface_diffuse_rim(fragment, pica_surface_unit(delta)) * light.color.rgb * attenuation;
}
// Points 3 and 4 (a room with Navi, a fairy and torches) behind their own
// uniform branch; same sums in the same order as one loop.
vec3 pica_surface_scene_point_primary(PicaSurfaceFragment fragment) {
    vec3 color = vec3(0.0);
    uint count = min(pica_surface.control.y, 4u);
    for (uint slot = 0u; slot < min(count, 2u); ++slot)
        color += pica_surface_scene_point(slot, fragment);
    if (count > 2u) {
        for (uint slot = 2u; slot < count; ++slot)
            color += pica_surface_scene_point(slot, fragment);
    }
    return color;
}
vec3 pica_surface_scene_directional_primary(PicaSurfaceFragment fragment) {
    vec3 color = vec3(0.0);
    uint count = min(pica_surface.control.z, 2u);
    for (uint slot = 0u; slot < count; ++slot)
        color += pica_surface_diffuse_rim(fragment, pica_surface_unit(pica_surface.scene_directions[slot].xyz)) *
            pica_surface.scene_directional_colors[slot].rgb;
    return color;
}
vec3 pica_surface_native_primary(PicaSurfaceFragment fragment) {
    vec3 direct = vec3(0.0);
    bool native_direct = pica_surface.environment_parameters.w == 0.0 ||
        pica_surface.environment_parameters.x != 0.0;
    for (int slot = 0; pica_surface.control.x != 0u && native_direct && slot < 3; ++slot) {
        if (pica_surface.lights[slot].direction_enabled.w == 0.0) continue;
        vec3 direction, radiance;
        pica_surface_light(slot, fragment.view, direction, radiance);
        direct += pica_surface_diffuse_rim(fragment, direction) * radiance;
    }
    vec3 color = direct * pica_surface.material_diffuse.rgb;
    if (pica_surface.environment_parameters.w != 0.0) color *= pica_surface.environment_parameters.x;
    color += pica_surface.resolved_ambient.rgb;
    color += pica_surface_rim_independent(fragment.normal, fragment.view_unit);
    if (pica_surface.environment_parameters.w != 0.0) {
        for (int slot = 0; slot < 2; ++slot) {
            if (pica_surface.environment_directions[slot].w <= 0.0) continue;
            color += pica_surface_diffuse_rim(fragment, pica_surface_unit(pica_surface.environment_directions[slot].xyz)) *
                pica_surface.environment_colors[slot].rgb * pica_surface.environment_directions[slot].w;
        }
    }
    color += pica_surface_scene_point_primary(fragment);
    color += pica_surface_scene_directional_primary(fragment);
    return min(abs(color * fragment.tint), vec3(1.0));
}
// Baked outdoor colour is a lit reference: ambient and the directionals
// redistribute it; a horizontal face keeps its native colour.
float pica_surface_baked_diffuse(PicaSurfaceFragment fragment, vec3 up, vec3 direction) {
    float diffuse = min(max(dot(fragment.normal, direction), 0.0), max(dot(up, direction), 0.0));
    // Every rim evaluation sits behind the rim's uniform branch.
    if (pica_surface.rim.x > 0.0)
        diffuse += pica_surface_rim_weight(fragment.normal, fragment.view_unit, direction);
    return diffuse;
}
vec3 pica_surface_baked_response(PicaSurfaceFragment fragment, out vec3 reference) {
    vec3 up = pica_surface_unit(pica_surface.baked_reference_up.xyz);
    vec3 response = pica_surface.control.x != 0u ? pica_surface.resolved_ambient.rgb
        : vec3(pica_surface.environment_parameters.y) + pica_surface.environment_ambient.rgb;
    reference = response;
    if (pica_surface.environment_parameters.w != 0.0) {
        for (int slot = 0; slot < 2; ++slot) {
            vec3 direction = pica_surface_unit(pica_surface.environment_directions[slot].xyz);
            vec3 color = pica_surface.environment_colors[slot].rgb;
            float gain = pica_surface.environment_directions[slot].w;
            response += color * gain * pica_surface_baked_diffuse(fragment, up, direction);
            reference += (slot == 0 ? pica_surface.baked_reference_key.rgb : color * gain) *
                max(dot(up, direction), 0.0);
        }
    }
    uint count = min(pica_surface.control.z, 2u);
    for (uint slot = 0u; slot < count; ++slot) {
        vec3 direction = pica_surface_unit(pica_surface.scene_directions[slot].xyz);
        vec3 color = pica_surface.scene_directional_colors[slot].rgb;
        response += color * pica_surface_baked_diffuse(fragment, up, direction);
        reference += color * max(dot(up, direction), 0.0);
    }
    return response;
}
vec3 pica_surface_primary(vec3 native_color, PicaSurfaceFragment fragment) {
    if (pica_surface.baked_reference_up.w != 0.0) {
        vec3 reference;
        vec3 response = pica_surface_baked_response(fragment, reference);
        vec3 color = native_color * response / max(reference, vec3(1.0e-6));
        color += (pica_surface_scene_point_primary(fragment) +
                  pica_surface_rim_independent(fragment.normal, fragment.view_unit)) * fragment.tint;
        return clamp(color, 0.0, 1.0);
    }
    if (pica_surface.control.x != 0u ||
        (pica_surface.environment_parameters.w != 0.0 && (pica_surface.control.w & 1u) == 0u))
        return pica_surface_native_primary(fragment);
    if (pica_surface.environment_parameters.w != 0.0)
        return min(native_color + pica_surface_native_primary(fragment), vec3(1.0));
    return min(native_color + (pica_surface_scene_point_primary(fragment) +
               pica_surface_scene_directional_primary(fragment)) * fragment.tint, vec3(1.0));
}
