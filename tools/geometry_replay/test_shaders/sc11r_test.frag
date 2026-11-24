#version 450
// SC-11R test fragment shader.
//
// Samples a texture with a sampler, which is the descriptor pair the phase requires. The sampled
// value is modulated by the uniform tint so a wrong uniform buffer or a wrong texture is visible in
// the output rather than silently ignored.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_tint;

layout(set = 0, binding = 1) uniform sampler2D in_texture;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = texture(in_texture, v_uv) * v_tint;
}
