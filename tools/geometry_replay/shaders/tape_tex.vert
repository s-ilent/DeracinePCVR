#version 450
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec2 v_uv;
layout(push_constant) uniform Push { mat4 mvp; } pc;
void main() {
    v_uv = in_uv;
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
}
