#version 330 core
// ============================================================================
//  Dark Noise — Dark Current + DSNU + Hot Pixels — Modular Effect
// ============================================================================
//  Adds dark current (Poisson-sampled temporal noise) plus fixed-pattern
//  dark-signal non-uniformity and hot pixel defects.
//  Expects noise_utils.glsl to be prepended.
// ============================================================================

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;
uniform float     u_darkCurrent;         // mean dark current (normalised)
uniform float     u_dsnuStrength;        // DSNU sigma
uniform float     u_hotPixelProbability; // fraction of hot pixels
uniform float     u_hotPixelStrength;    // hot pixel dark current multiplier
uniform int       u_frameNumber;
uniform vec2      u_resolution;

void main()
{
    vec2 fragCoord = v_texCoord * u_resolution;
    vec3 color = texture(u_inputTexture, v_texCoord).rgb;

    // ── Fixed-pattern: DSNU offset + hot pixel (spatial seed, static) ───
    uint spatialState = rng_seed_spatial(fragCoord);

    // DSNU: half-normal offset (always additive)
    float dsnuOffset = abs(u_dsnuStrength * rand_normal(spatialState));

    // Hot pixel determination
    bool isHotPixel = (rand_float(spatialState) < u_hotPixelProbability);

    // Total dark current for this pixel
    float darkContrib = u_darkCurrent + dsnuOffset;
    if (isHotPixel)
        darkContrib += u_hotPixelStrength * u_darkCurrent;

    // ── Temporal: Poisson-sample the dark current ───────────────────────
    // Dark current generates electrons randomly each frame
    uint temporalState = rng_seed_temporal(fragCoord, u_frameNumber + 7919);

    // Add dark current noise to each channel identically
    // (dark current is not wavelength-dependent to first order)
    float darkNoise = float(sample_poisson(darkContrib * 1000.0, temporalState)) / 1000.0;

    vec3 noisy = color + vec3(darkNoise);

    fragColor = vec4(clamp(noisy, 0.0, 1.0), 1.0);
}
