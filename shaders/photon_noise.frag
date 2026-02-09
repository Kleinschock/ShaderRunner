#version 330 core
// ============================================================================
//  Photon (Shot) Noise — Modular Effect
// ============================================================================
//  Applies Poisson-distributed shot noise per channel.
//  Expects noise_utils.glsl to be prepended.
// ============================================================================

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;
uniform float     u_photonScale;
uniform int       u_frameNumber;
uniform vec2      u_resolution;

void main()
{
    vec2 fragCoord = v_texCoord * u_resolution;
    vec3 color = max(texture(u_inputTexture, v_texCoord).rgb, vec3(0.0));

    uint state = rng_seed_temporal(fragCoord, u_frameNumber);

    vec3 noisy;
    noisy.r = float(sample_poisson(color.r * u_photonScale, state)) / u_photonScale;
    noisy.g = float(sample_poisson(color.g * u_photonScale, state)) / u_photonScale;
    noisy.b = float(sample_poisson(color.b * u_photonScale, state)) / u_photonScale;

    fragColor = vec4(clamp(noisy, 0.0, 1.0), 1.0);
}
