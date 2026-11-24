#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_inst;
layout(location = 0) out vec4 out_color;
layout(binding = 0) uniform sampler2D u_tex;
void main() {
    // Diagnostic instance visualisation. The captured draw uses two instances of the same geometry
    // with no per-instance attribute, so both rasterise to identical pixels and a mutation of
    // NumInstances or FirstInstance cannot be seen in the sampled colour. Tinting by the instance
    // index makes the executed instance range visible without changing the draw.
    vec4 texel = texture(u_tex, v_uv);
    float t = v_inst * 0.37;
    vec3 tint = vec3(0.5 + 0.5 * cos(6.28318 * (t + 0.0)),
                     0.5 + 0.5 * cos(6.28318 * (t + 0.33)),
                     0.5 + 0.5 * cos(6.28318 * (t + 0.67)));
    out_color = vec4(texel.rgb * tint, 1.0);
}
