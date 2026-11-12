#version 430
layout(location=0) in vec4 pica_primary_color;
layout(location=1) in vec2 pica_texcoord0;
layout(location=2) in vec2 pica_texcoord1;
layout(location=3) in vec2 pica_texcoord2;
layout(location=4) in float pica_texcoord0_w;
layout(location=5) in vec4 pica_normquat;
layout(location=6) in vec3 pica_view;
layout(binding=1) uniform sampler2D pica_texture0;
layout(binding=2) uniform sampler2D pica_texture1;
layout(binding=3) uniform sampler2D pica_texture2;
layout(binding=4, std140) uniform PicaFragmentUniforms {
    vec4 tev_constants[6];
    vec4 combiner_buffer_color;
    int alpha_reference;
    float depth_scale;
    float depth_offset;
    int w_buffering;
    vec4 fog_color;
    vec4 fog_lut[64];
    vec4 texture_lod_bias;
    vec4 lighting_specular0[8];
    vec4 lighting_specular1[8];
    vec4 lighting_diffuse[8];
    vec4 lighting_ambient[8];
    vec4 lighting_position[8];
    vec4 lighting_spot_direction[8];
    vec4 lighting_attenuation[8];
    vec4 lighting_global_ambient;
    int shadow_texture_bias;
    int shadow_orthographic;
    float shadow_bias_constant;
    float shadow_bias_linear;
} fragment_uniforms;
layout(location=0) out vec4 pica_color;
vec3 byteround3(vec3 value) { precise vec3 scaled=value*255.0; precise vec3 biased=scaled+0.5; return floor(biased)/255.0; }
float byteround1(float value) { precise float scaled=value*255.0; precise float biased=scaled+0.5; return floor(biased)/255.0; }
float pica_texture_lod(vec2 coord, vec2 size) {
    vec2 scaled = coord * size;
    vec2 delta = max(abs(dFdx(scaled)), abs(dFdy(scaled)));
    return log2(max(delta.x, delta.y));
}
vec4 pica_sample_texture0(vec2 coord) {
    float lod = pica_texture_lod(coord, vec2(textureSize(pica_texture0, 0)));
    return textureLod(pica_texture0, coord, lod + fragment_uniforms.texture_lod_bias[0]);
}
vec4 pica_sample_texture1(vec2 coord) {
    float lod = pica_texture_lod(coord, vec2(textureSize(pica_texture1, 0)));
    return textureLod(pica_texture1, coord, lod + fragment_uniforms.texture_lod_bias[1]);
}
vec4 pica_sample_texture2(vec2 coord) {
    float lod = pica_texture_lod(coord, vec2(textureSize(pica_texture2, 0)));
    return textureLod(pica_texture2, coord, lod + fragment_uniforms.texture_lod_bias[2]);
}

struct ToonSurfaceParameters {
    vec4 control;
    vec4 shadow;
    vec4 rim;
    vec4 flags;
    vec4 levels[2];
    vec4 thresholds[2];
};

float oot3d_toon_transition(float edge, float softness, float guide) {
    // GLSL smoothstep has undefined results for equal edges (hard bands).
    return softness > 0.0 ? smoothstep(edge - softness, edge + softness, guide)
                          : step(edge, guide);
}

float oot3d_toon_band_luminance(float guide, ToonSurfaceParameters p) {
    if (p.control.w < 0.5) {
        float scaled = guide * p.control.x;
        return (floor(scaled) + oot3d_toon_transition(0.5, p.control.y, fract(scaled))) / p.control.x;
    }
    int count = int(p.control.x);
    float bandLuminance = p.levels[0][0];
    for (int i = 0; i < count; ++i) {
        float edge = p.thresholds[i / 4][i % 4];
        float spacing = 1.0;
        if (count > 1) {
            int previous = max(i - 1, 0), next = min(i + 1, count - 1);
            float before = edge - p.thresholds[previous / 4][previous % 4];
            float after = p.thresholds[next / 4][next % 4] - edge;
            spacing = i == 0 ? after : (i == count - 1 ? before : min(before, after));
        }
        float delta = p.levels[(i + 1) / 4][(i + 1) % 4] - p.levels[i / 4][i % 4];
        bandLuminance += delta * oot3d_toon_transition(edge, p.control.y * max(spacing, 0.001), guide);
    }
    return clamp(bandLuminance, 0.0, 1.0);
}

vec3 oot3d_toon_banded_color(vec3 sourceColor, vec3 lighting,
                            float hasLighting, bool shadowTint, ToonSurfaceParameters p) {
    const vec3 lumaWeights = vec3(0.2126, 0.7152, 0.0722);
    float guide = clamp(dot(lighting, lumaWeights), 0.0, 1.0);
    float bandLuminance = oot3d_toon_band_luminance(guide, p);
    float lightingScale = mix(1.0, clamp(bandLuminance / max(guide, 0.05), 0.0, 2.0), hasLighting);
    vec3 banded = sourceColor * lightingScale;
    banded = mix(vec3(dot(banded, lumaWeights)), banded, p.control.z);
    if (shadowTint) {
        float shadowMask = (1.0 - smoothstep(0.0, 0.55, bandLuminance)) * hasLighting;
        banded = mix(banded, banded * p.shadow.rgb, shadowMask * p.shadow.w);
    }
    return banded;
}

vec3 oot3d_toon_rim(vec3 normal, vec3 viewDirection, ToonSurfaceParameters p) {
    normal = dot(normal, normal) > 0.000001 ? normalize(normal) : vec3(0.0, 0.0, 1.0);
    viewDirection = dot(viewDirection, viewDirection) > 0.000001
        ? normalize(viewDirection) : vec3(0.0, 0.0, 1.0);
    float rim = pow(clamp(1.0 - abs(dot(normal, viewDirection)), 0.0, 1.0), p.flags.z);
    return p.rim.rgb * (rim * p.rim.w);
}

vec3 oot3d_toon_surface_response(vec3 sourceColor, vec3 lighting,
                                vec3 normal, vec3 viewDirection, ToonSurfaceParameters p) {
    if (p.flags.x < 0.5) return sourceColor;
    return clamp(oot3d_toon_banded_color(sourceColor, lighting, p.flags.y, true, p)
        + oot3d_toon_rim(normal, viewDirection, p), 0.0, 1.0);
}

// Terrain-cover extensions inherit diffuse bands, not an independent rim.
// The response intentionally has no normal or viewing-direction argument.
vec3 oot3d_toon_diffuse_response(vec3 sourceColor, vec3 lighting, ToonSurfaceParameters p) {
    if (p.flags.x < 0.5) return sourceColor;
    return clamp(oot3d_toon_banded_color(sourceColor, lighting, p.flags.y, true, p), 0.0, 1.0);
}

ToonSurfaceParameters oot3d_toon_parameters() {
    return ToonSurfaceParameters(vec4(3.000000, 0.228000, 1.090000, 0.000000), vec4(0.480000, 0.560000, 0.720000, 1.000000), vec4(1.000000, 0.930000, 0.720000, 0.340000), vec4(1.000000, 1.000000, 3.170000, 0.000000), vec4[2](vec4(0.000000, 0.333333, 0.666667, 1.000000), vec4(1.000000, 1.000000, 0.000000, 0.000000)), vec4[2](vec4(0.166667, 0.500000, 0.833333, 1.000000), vec4(1.000000, 0.000000, 0.000000, 0.000000)));
}

vec3 oot3d_toon_rotate_z(vec4 q) {
    float qlen = dot(q, q);
    if (qlen < 0.000001) return vec3(0.0, 0.0, 1.0);
    q *= inversesqrt(qlen);
    return vec3(2.0 * (q.x * q.z + q.w * q.y),
                2.0 * (q.y * q.z - q.w * q.x),
                1.0 - 2.0 * (q.x * q.x + q.y * q.y));
}
vec3 oot3d_apply_toon(vec3 sourceColor) {
    return oot3d_toon_surface_response(sourceColor, pica_primary_color.rgb,
        oot3d_toon_rotate_z(pica_normquat), pica_view, oot3d_toon_parameters());
}
vec3 oot3d_material_bands(vec3 lighting, bool shadow_tint) {
    return clamp(oot3d_toon_banded_color(lighting, lighting, 1.0,
        shadow_tint, oot3d_toon_parameters()), 0.0, 1.0);
}
vec3 oot3d_material_rim(vec3 materialNormal) {
    return oot3d_toon_rim(materialNormal, pica_view, oot3d_toon_parameters());
}
void main() {
    precise vec4 rounded_primary_color = vec4(byteround3(pica_primary_color.rgb), byteround1(pica_primary_color.a));
    vec4 primary_fragment_color = vec4(0.0);
    vec4 secondary_fragment_color = vec4(0.0);
    vec4 combiner_buffer = vec4(0.0);
    vec4 next_combiner_buffer = fragment_uniforms.combiner_buffer_color;
    vec4 combiner_output = vec4(0.0);
    // OOT3D_PICA_LIGHTING_EXTENSION_POINT
    precise vec3 color_output_0 = byteround3(clamp(rounded_primary_color.rgb * pica_sample_texture0(vec2(pica_texcoord0.x, 1.0 - pica_texcoord0.y)).rgb, vec3(0.0), vec3(1.0)));
    precise float alpha_output_0 = byteround1(clamp(rounded_primary_color.a * pica_sample_texture0(vec2(pica_texcoord0.x, 1.0 - pica_texcoord0.y)).a, 0.0, 1.0));
    combiner_output = vec4(clamp(color_output_0 * 2.0, vec3(0.0), vec3(1.0)), clamp(alpha_output_0 * 1.0, 0.0, 1.0));
    combiner_buffer = next_combiner_buffer;
    precise vec3 color_output_1 = byteround3(clamp(combiner_output.rgb, vec3(0.0), vec3(1.0)));
    precise float alpha_output_1 = byteround1(clamp(combiner_output.a * fragment_uniforms.tev_constants[1].a, 0.0, 1.0));
    combiner_output = vec4(clamp(color_output_1 * 1.0, vec3(0.0), vec3(1.0)), clamp(alpha_output_1 * 1.0, 0.0, 1.0));
    combiner_buffer = next_combiner_buffer;
    precise vec3 color_output_2 = byteround3(clamp(combiner_output.rgb, vec3(0.0), vec3(1.0)));
    precise float alpha_output_2 = byteround1(clamp(combiner_output.a, 0.0, 1.0));
    combiner_output = vec4(clamp(color_output_2 * 1.0, vec3(0.0), vec3(1.0)), clamp(alpha_output_2 * 1.0, 0.0, 1.0));
    combiner_buffer = next_combiner_buffer;
    precise vec3 color_output_3 = byteround3(clamp(combiner_output.rgb, vec3(0.0), vec3(1.0)));
    precise float alpha_output_3 = byteround1(clamp(combiner_output.a, 0.0, 1.0));
    combiner_output = vec4(clamp(color_output_3 * 1.0, vec3(0.0), vec3(1.0)), clamp(alpha_output_3 * 1.0, 0.0, 1.0));
    combiner_buffer = next_combiner_buffer;
    precise vec3 color_output_4 = byteround3(clamp(combiner_output.rgb, vec3(0.0), vec3(1.0)));
    precise float alpha_output_4 = byteround1(clamp(combiner_output.a, 0.0, 1.0));
    combiner_output = vec4(clamp(color_output_4 * 1.0, vec3(0.0), vec3(1.0)), clamp(alpha_output_4 * 1.0, 0.0, 1.0));
    combiner_buffer = next_combiner_buffer;
    precise vec3 color_output_5 = byteround3(clamp(combiner_output.rgb, vec3(0.0), vec3(1.0)));
    precise float alpha_output_5 = byteround1(clamp(combiner_output.a, 0.0, 1.0));
    combiner_output = vec4(clamp(color_output_5 * 1.0, vec3(0.0), vec3(1.0)), clamp(alpha_output_5 * 1.0, 0.0, 1.0));
    combiner_buffer = next_combiner_buffer;
    combiner_output.rgb = oot3d_apply_toon(combiner_output.rgb);
    float pica_z_over_w = -gl_FragCoord.z;
    float pica_depth = pica_z_over_w * fragment_uniforms.depth_scale + fragment_uniforms.depth_offset;
    if (fragment_uniforms.w_buffering != 0) pica_depth /= gl_FragCoord.w;
    float fog_index = pica_depth * 128.0;
    float fog_i = clamp(floor(fog_index), 0.0, 127.0);
    int fog_entry_index = int(fog_i);
    vec4 fog_pair = fragment_uniforms.fog_lut[fog_entry_index >> 1];
    vec2 fog_entry = (fog_entry_index & 1) == 0 ? fog_pair.xy : fog_pair.zw;
    float fog_factor = clamp(fog_entry.x + fog_entry.y * (fog_index - fog_i), 0.0, 1.0);
    combiner_output.rgb = mix(fragment_uniforms.fog_color.rgb, combiner_output.rgb, fog_factor);
    gl_FragDepth = pica_depth;
    pica_color = vec4(byteround3(combiner_output.rgb), byteround1(combiner_output.a));
}
