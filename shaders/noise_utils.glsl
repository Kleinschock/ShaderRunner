// ============================================================================
//  noise_utils.glsl — Shared RNG & Sampling Utilities
// ============================================================================
//  Prepended to each noise shader at load time by C++.
//  Provides: PCG hash, uniform/normal random, Poisson sampling,
//            spatial (fixed-pattern) and temporal seed generators.
// ============================================================================

// ── PCG Hash ────────────────────────────────────────────────────────────────
uint pcg_hash(uint input_state)
{
    uint state = input_state * 747796405u + 2891336453u;
    uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// ── Seeding ─────────────────────────────────────────────────────────────────
// Temporal seed: varies per pixel AND per frame (for shot/read noise)
uint rng_seed_temporal(vec2 fragCoord, int frame)
{
    uint x = uint(fragCoord.x);
    uint y = uint(fragCoord.y);
    uint f = uint(frame);
    return pcg_hash(x + pcg_hash(y + pcg_hash(f)));
}

// Spatial seed: varies per pixel ONLY (for fixed-pattern noise: PRNU, DSNU)
uint rng_seed_spatial(vec2 fragCoord)
{
    uint x = uint(fragCoord.x);
    uint y = uint(fragCoord.y);
    return pcg_hash(x * 1664525u + pcg_hash(y * 1013904223u + 374761393u));
}

// ── Random number generators ────────────────────────────────────────────────
float rand_float(inout uint state)
{
    state = pcg_hash(state);
    return float(state) / 4294967296.0;
}

float rand_normal(inout uint state)
{
    float u1 = max(rand_float(state), 1e-10);
    float u2 = rand_float(state);
    return sqrt(-2.0 * log(u1)) * cos(6.28318530718 * u2);
}

// ── Poisson Sampling ────────────────────────────────────────────────────────
int poisson_small(float lambda, inout uint state)
{
    float L = exp(-lambda);
    float p = 1.0;
    int   k = 0;
    for (int i = 0; i < 200; ++i)
    {
        p *= rand_float(state);
        if (p <= L) break;
        k++;
    }
    return k;
}

int poisson_large(float lambda, inout uint state)
{
    float result = lambda + sqrt(lambda) * rand_normal(state);
    return max(0, int(round(result)));
}

int sample_poisson(float lambda, inout uint state)
{
    if (lambda < 0.001)
        return 0;
    else if (lambda < 30.0)
        return poisson_small(lambda, state);
    else
        return poisson_large(lambda, state);
}

// ── End noise_utils.glsl ────────────────────────────────────────────────────
