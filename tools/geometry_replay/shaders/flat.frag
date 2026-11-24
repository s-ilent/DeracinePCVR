#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
void main() {
    // A flat diagnostic colour, so the textured render can be shown to differ from it.
    out_color = vec4(0.55, 0.55, 0.60, 1.0);
}
