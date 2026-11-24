#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
void main() {
    // The UVs themselves, so the texture's contribution can be told apart from the coordinates.
    out_color = vec4(fract(v_uv), 0.0, 1.0);
}
