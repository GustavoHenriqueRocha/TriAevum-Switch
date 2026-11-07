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
void main() {
    precise vec4 rounded_primary_color = vec4(byteround3(pica_primary_color.rgb), byteround1(pica_primary_color.a));
    vec4 primary_fragment_color = vec4(0.0);
    vec4 secondary_fragment_color = vec4(0.0);
    vec4 combiner_buffer = vec4(0.0);
    vec4 next_combiner_buffer = fragment_uniforms.combiner_buffer_color;
    vec4 combiner_output = vec4(0.0);
    // OOT3D_PICA_LIGHTING_EXTENSION_POINT
    precise vec3 color_output_0 = byteround3(clamp(rounded_primary_color.rgb * pica_sample_texture0(vec2(pica_texcoord0.x, 1.0 - pica_texcoord0.y)).rgb, vec3(0.0), vec3(1.0)));
    precise float alpha_output_0 = byteround1(clamp(fragment_uniforms.tev_constants[0].a * pica_sample_texture1(vec2(pica_texcoord1.x, 1.0 - pica_texcoord1.y)).a, 0.0, 1.0));
    combiner_output = vec4(clamp(color_output_0 * 2.0, vec3(0.0), vec3(1.0)), clamp(alpha_output_0 * 1.0, 0.0, 1.0));
    combiner_buffer = next_combiner_buffer;
    precise vec3 color_output_1 = byteround3(clamp(combiner_output.rgb, vec3(0.0), vec3(1.0)));
    precise float alpha_output_1 = byteround1(clamp(combiner_output.a, 0.0, 1.0));
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
    int alpha_byte = int(combiner_output.a * 255.0);
    if (alpha_byte <= fragment_uniforms.alpha_reference) discard;
    float pica_z_over_w = -gl_FragCoord.z;
    float pica_depth = gl_FragCoord.z;
    float fog_index = pica_depth * 128.0;
    float fog_i = clamp(floor(fog_index), 0.0, 127.0);
    int fog_entry_index = int(fog_i);
    vec4 fog_pair = fragment_uniforms.fog_lut[fog_entry_index >> 1];
    vec2 fog_entry = (fog_entry_index & 1) == 0 ? fog_pair.xy : fog_pair.zw;
    float fog_factor = clamp(fog_entry.x + fog_entry.y * (fog_index - fog_i), 0.0, 1.0);
    combiner_output.rgb = mix(fragment_uniforms.fog_color.rgb, combiner_output.rgb, fog_factor);
    pica_color = vec4(byteround3(combiner_output.rgb), byteround1(combiner_output.a));
}
