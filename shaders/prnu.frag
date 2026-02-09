#version 330 core
// ============================================================================
//  PRNU — Photo-Response Non-Uniformity — Modular Effect
// ============================================================================
//  Each pixel has a slightly different quantum efficiency (gain).
//  This is a multiplicative, fixed-pattern effect.
//  Expects noise_utils.glsl to be prepended.
// ============================================================================

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;
uniform float     u_prnuStrength;    // PRNU sigma (fraction, e.g. 0.01 = 1%)
uniform vec2      u_resolution;

void main()
{
    vec2 fragCoord = v_texCoord * u_resolution;
    vec3 color = texture(u_inputTexture, v_texCoord).rgb;

    // Spatial-only seed — gain map is fixed across frames
    uint spatialState = rng_seed_spatial(fragCoord);

    // Per-pixel gain: ~ N(1, sigma)
    float gain = 1.0 + u_prnuStrength * rand_normal(spatialState);

    // Apply multiplicative gain (same gain for all channels on a given pixel,
    // since PRNU is primarily a per-photosite effect)
    vec3 modulated = color * gain;

    fragColor = vec4(clamp(modulated, 0.0, 1.0), 1.0);
}
