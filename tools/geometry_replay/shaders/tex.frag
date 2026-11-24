#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(binding = 0) uniform sampler2D u_tex;
void main() {
    // The diagnostic shows the sampled colour with alpha forced opaque: the captured texture is a
    // mask whose alpha is mostly zero, and a transparent diagnostic would show nothing.
    vec4 texel = texture(u_tex, v_uv);
    out_color = vec4(texel.rgb, 1.0);
}
