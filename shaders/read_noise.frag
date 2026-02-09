#version 330 core
// ============================================================================
//  Read Noise — Gaussian Readout Noise — Modular Effect
// ============================================================================
//  Additive, signal-independent Gaussian noise from the sensor's ADC
//  and readout electronics.
//  Expects noise_utils.glsl to be prepended.
// ============================================================================

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;
uniform float     u_readNoise;       // read noise sigma (normalised)
uniform int       u_frameNumber;
uniform vec2      u_resolution;

void main()
{
    vec2 fragCoord = v_texCoord * u_resolution;
    vec3 color = texture(u_inputTexture, v_texCoord).rgb;

    // Use a different frame offset to decorrelate from other temporal noise
    uint state = rng_seed_temporal(fragCoord, u_frameNumber + 15731);

    // Independent Gaussian noise per channel
    vec3 noise;
    noise.r = u_readNoise * rand_normal(state);
    noise.g = u_readNoise * rand_normal(state);
    noise.b = u_readNoise * rand_normal(state);

    fragColor = vec4(clamp(color + noise, 0.0, 1.0), 1.0);
}
