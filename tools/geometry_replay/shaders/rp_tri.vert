#version 450
layout(location = 0) out vec3 v_color;
void main() {
    // A full-screen triangle. The colour is derived from the vertex index so the output is
    // deterministic and every pixel is covered.
    vec2 pos[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec3 col[3] = vec3[](vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0));
    v_color = col[gl_VertexIndex];
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
}
