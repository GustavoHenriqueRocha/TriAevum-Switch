#version 460
layout(location = 0) in vec4 ui_shade;
layout(location = 0) out vec4 ui_out;
void main() {
    ui_out = ui_shade;
}
