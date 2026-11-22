#version 450
layout(location = 0) in vec3 in_pos;
layout(location = 0) out vec3 v_color;
layout(push_constant) uniform Push { mat4 mvp; } pc;
void main() {
    // Diagnostic shading: colour by object-space position so the three axes are distinguishable.
    v_color = clamp(in_pos * 0.5 + 0.5, 0.0, 1.0);
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
}
