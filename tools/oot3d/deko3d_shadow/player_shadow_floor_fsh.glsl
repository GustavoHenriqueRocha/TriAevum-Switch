#version 460
// deko3d simple shadow, method "texture" (gfx_deko3d_player_shadow.cpp): Link's
// silhouette under the floor quad, softened by four bilinear taps (about three
// texels across), in black at the shadow's opacity. Nothing is drawn where the
// silhouette is empty; no depth write.
layout(std140, binding = 0) uniform PlayerShadowFloor {
    vec4 positions[4];
    vec4 texcoords[4]; // xy
    vec4 shade;        // x: opacity, yz: one texel of the silhouette
};
layout(binding = 0) uniform sampler2D silhouette;

layout(location = 0) in vec2 silhouette_uv;
layout(location = 0) out vec4 shadow_out_color;

void main() {
    vec2 d = shade.yz * 0.75;
    float cover = texture(silhouette, silhouette_uv + vec2(-d.x, -d.y)).r +
                  texture(silhouette, silhouette_uv + vec2(d.x, -d.y)).r +
                  texture(silhouette, silhouette_uv + vec2(-d.x, d.y)).r +
                  texture(silhouette, silhouette_uv + vec2(d.x, d.y)).r;
    if (cover <= 0.0) discard;
    shadow_out_color = vec4(0.0, 0.0, 0.0, shade.x * cover * 0.25);
}
