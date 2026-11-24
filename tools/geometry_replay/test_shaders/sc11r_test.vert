#version 450
// SC-11R test vertex shader.
//
// Exercises the four inputs the phase requires: one vertex attribute, one uniform buffer, one
// push-constant field, and a sampled texture passed to the fragment stage. It is a real shader
// compiled by glslc, not a fabricated SPIR-V header.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;

layout(set = 0, binding = 0) uniform TransformBlock {
    mat4 mvp;
    vec4 tint;
} transform;

layout(push_constant) uniform PushBlock {
    vec4 offset;
} push;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_tint;

void main() {
    vec4 position = transform.mvp * vec4(in_position, 1.0);
    position.xy += push.offset.xy;
    gl_Position = position;
    v_uv = in_uv;
    v_tint = transform.tint;
}
