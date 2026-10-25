#version 460
layout(binding = 0) uniform sampler2D ui_texture;
layout(location = 0) in vec4 ui_shade;
layout(location = 1) in vec2 ui_texcoord;
layout(location = 0) out vec4 ui_out;
void main() {
    ui_out = texture(ui_texture, ui_texcoord) * ui_shade;
}
