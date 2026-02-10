# Sensor Simulation — GLSL Shader Reference

**Tiered implementations from fast approximation to full physical accuracy.**

Based on the research in `SENSOR_PHYSICS_RESEARCH.md`. All shaders are GLSL 330 core, expect `noise_utils.glsl` prepended.

---

## Shared Utilities — `noise_utils.glsl`

All tiers share this foundation. Our existing implementation is solid:

```glsl
// ── PCG Hash (statistically excellent, GPU-friendly) ──
uint pcg_hash(uint s) {
    uint state = s * 747796405u + 2891336453u;
    uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// ── Seeding ──
uint rng_seed_temporal(vec2 fc, int frame) {
    return pcg_hash(uint(fc.x) + pcg_hash(uint(fc.y) + pcg_hash(uint(frame))));
}

uint rng_seed_spatial(vec2 fc) {
    return pcg_hash(uint(fc.x)*1664525u + pcg_hash(uint(fc.y)*1013904223u + 374761393u));
}

// ── Samplers ──
float rand_float(inout uint s) { s = pcg_hash(s); return float(s) / 4294967296.0; }

float rand_normal(inout uint s) {
    float u1 = max(rand_float(s), 1e-10);
    float u2 = rand_float(s);
    return sqrt(-2.0 * log(u1)) * cos(6.28318530718 * u2);
}

int poisson_small(float lam, inout uint s) {
    float L = exp(-lam); float p = 1.0; int k = 0;
    for (int i = 0; i < 200; ++i) { p *= rand_float(s); if (p <= L) break; k++; }
    return k;
}

int poisson_large(float lam, inout uint s) {
    return max(0, int(round(lam + sqrt(lam) * rand_normal(s))));
}

int sample_poisson(float lam, inout uint s) {
    if (lam < 0.001) return 0;
    return (lam < 30.0) ? poisson_small(lam, s) : poisson_large(lam, s);
}
```

---

## Tier Overview

| Tier | Name | What It Simulates | Passes | Use Case |
|------|------|-------------------|--------|----------|
| **T1** | Quick & Dirty | Shot + read + dark (RGB domain) | 1 | Game engine post-FX, previews |
| **T2** | Modular RGB | All 7 core noise sources (RGB domain) | 4 separate | Existing pipeline, good visual fidelity |
| **T3** | Physical Bayer | Bayer mosaic → noise → demosaic + ISP | 5-7 | Scientific simulation, MATLAB validation |

---

# Tier 1 — Single-Pass Combined Noise

**Philosophy**: Maximum performance. One shader pass does everything. Works in normalised [0,1] RGB space. Sacrifices modularity and Bayer accuracy for speed.

**Cost**: ~15 hash operations per pixel. Negligible GPU load.

```glsl
#version 330 core
// ════════════════════════════════════════════════════════════════════════════
//  TIER 1 — Single-Pass Combined Sensor Noise
// ════════════════════════════════════════════════════════════════════════════
//  Combines shot noise, dark current + DSNU + hot pixels, PRNU, read noise,
//  column FPN, quantisation, and soft saturation in one pass.
//  Fast. Approximate. Visually convincing.
//  Expects noise_utils.glsl prepended.
// ════════════════════════════════════════════════════════════════════════════

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;
uniform vec2      u_resolution;
uniform int       u_frameNumber;

// ── Noise parameters (all normalised to [0,1] signal range) ──
uniform float u_photonScale;          // effective well capacity for shot noise
uniform float u_darkCurrent;          // mean dark current (normalised)
uniform float u_dsnuSigma;            // DSNU sigma
uniform float u_hotPixelProb;         // fraction of hot pixels (e.g. 0.001)
uniform float u_hotPixelStrength;     // hot pixel multiplier
uniform float u_prnuSigma;            // PRNU sigma (e.g. 0.01 = 1%)
uniform float u_readNoiseSigma;       // read noise sigma (normalised)
uniform float u_colFPN_offset;        // column FPN offset sigma
uniform float u_colFPN_gain;          // column FPN gain sigma
uniform int   u_adcBits;              // ADC resolution (0 = skip quantisation)

void main()
{
    vec2 fc = v_texCoord * u_resolution;
    vec3 color = max(texture(u_inputTexture, v_texCoord).rgb, vec3(0.0));

    // ── 1. PRNU (fixed-pattern multiplicative gain) ──────────────────────
    uint spState = rng_seed_spatial(fc);
    float prnuGain = 1.0 + u_prnuSigma * rand_normal(spState);
    color *= prnuGain;

    // ── 2. Dark current: base + DSNU + hot pixels ────────────────────────
    float dsnuOffset = abs(u_dsnuSigma * rand_normal(spState));
    bool  isHot      = (rand_float(spState) < u_hotPixelProb);
    float darkTotal  = u_darkCurrent + dsnuOffset;
    if (isHot) darkTotal += u_hotPixelStrength * u_darkCurrent;

    // ── 3. Shot noise (Poisson on signal + dark current) ─────────────────
    uint tState = rng_seed_temporal(fc, u_frameNumber);
    vec3 noisy;
    noisy.r = float(sample_poisson((color.r + darkTotal) * u_photonScale, tState))
              / u_photonScale;
    noisy.g = float(sample_poisson((color.g + darkTotal) * u_photonScale, tState))
              / u_photonScale;
    noisy.b = float(sample_poisson((color.b + darkTotal) * u_photonScale, tState))
              / u_photonScale;

    // ── 4. Read noise (Gaussian per channel) ─────────────────────────────
    uint rState = rng_seed_temporal(fc, u_frameNumber + 15731);
    noisy.r += u_readNoiseSigma * rand_normal(rState);
    noisy.g += u_readNoiseSigma * rand_normal(rState);
    noisy.b += u_readNoiseSigma * rand_normal(rState);

    // ── 5. Column FPN (offset + gain per column) ─────────────────────────
    if (u_colFPN_offset > 0.0 || u_colFPN_gain > 0.0) {
        uint colSeed = pcg_hash(uint(fc.x) * 2654435761u + 12345u);
        float cOff  = u_colFPN_offset * rand_normal(colSeed);
        float cGain = 1.0 + u_colFPN_gain
                      * rand_normal(pcg_hash(colSeed + 99991u));
        noisy = noisy * cGain + vec3(cOff);
    }

    // ── 6. Soft saturation (non-linear response near FWC) ────────────────
    noisy = max(noisy, vec3(0.0));
    float knee = 0.85;
    noisy = mix(noisy,
                vec3(knee) + (1.0 - knee) * (1.0 - exp(-(noisy - knee)/(1.0-knee))),
                step(knee, noisy));

    // ── 7. Quantisation (ADC bit depth) ──────────────────────────────────
    if (u_adcBits > 0) {
        float levels = exp2(float(u_adcBits));
        noisy = floor(noisy * levels + 0.5) / levels;
    }

    fragColor = vec4(clamp(noisy, 0.0, 1.0), 1.0);
}
```

**What's physically correct**: Shot noise (Poisson), PRNU (multiplicative), dark current + DSNU + hot pixels, read noise (Gaussian), column FPN, soft saturation, quantisation.

**What's missing**: Bayer/demosaicing, dead pixels, 1/f noise, crosstalk, temperature model, per-channel PRNU. Also combines dark current shot noise with signal shot noise (physically correct — both are Poisson on the combined signal).

---

# Tier 2 — Modular RGB Pipeline (Current Architecture + Extensions)

**Philosophy**: Separate shader passes for each noise source. Easy to enable/disable individually. Still works in RGB domain (no Bayer). This is an evolution of our existing 4-shader pipeline with added effects.

## Pass 1: Vignetting + Distortion

```glsl
#version 330 core
// ════════════════════════════════════════════════════════════════════════════
//  T2 Pass 1 — Optical Defects: Vignetting + Radial Distortion
// ════════════════════════════════════════════════════════════════════════════

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;
uniform vec2      u_resolution;
uniform float     u_vignetteStrength;   // 0 = off, 1 = full cos^4
uniform float     u_distortionK1;       // barrel/pincushion coefficient
uniform float     u_distortionK2;       // higher-order distortion

void main()
{
    vec2 uv = v_texCoord;

    // ── Radial distortion (Brown-Conrady model) ──
    if (u_distortionK1 != 0.0 || u_distortionK2 != 0.0) {
        vec2 centered = uv - 0.5;
        float r2 = dot(centered, centered);
        float r4 = r2 * r2;
        float scale = 1.0 + u_distortionK1 * r2 + u_distortionK2 * r4;
        uv = centered * scale + 0.5;
    }

    vec3 color = texture(u_inputTexture, uv).rgb;

    // ── Vignetting (cos^4 theta approximation) ──
    if (u_vignetteStrength > 0.0) {
        vec2 offset = (uv - 0.5) * 2.0;
        float r2 = dot(offset, offset);
        // cos^4(theta) ≈ (1 - r²/4)² for small angles; use exact for larger
        float cos_theta = 1.0 / sqrt(1.0 + r2);
        float vignette = pow(cos_theta, 4.0);
        color *= mix(1.0, vignette, u_vignetteStrength);
    }

    fragColor = vec4(color, 1.0);
}
```

## Pass 2: PRNU (Per-Channel, Wavelength-Dependent)

```glsl
#version 330 core
// ════════════════════════════════════════════════════════════════════════════
//  T2 Pass 2 — PRNU (Per-Channel Photo-Response Non-Uniformity)
// ════════════════════════════════════════════════════════════════════════════
//  Multiplicative fixed-pattern. Per-channel for wavelength dependence.

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;
uniform vec2      u_resolution;
uniform float     u_prnuSigma;       // overall PRNU sigma
uniform bool      u_perChannel;      // true = different gain per R,G,B

void main()
{
    vec2 fc = v_texCoord * u_resolution;
    vec3 color = texture(u_inputTexture, v_texCoord).rgb;

    uint sp = rng_seed_spatial(fc);

    if (u_perChannel) {
        // Each channel gets its own gain (wavelength-dependent PRNU)
        // Use different spatial seeds offset by large primes
        float gR = 1.0 + u_prnuSigma * rand_normal(sp);
        uint sp2 = pcg_hash(sp + 104729u);
        float gG = 1.0 + u_prnuSigma * rand_normal(sp2);
        uint sp3 = pcg_hash(sp + 224737u);
        float gB = 1.0 + u_prnuSigma * rand_normal(sp3);
        color *= vec3(gR, gG, gB);
    } else {
        // Single gain per pixel (original behaviour)
        float gain = 1.0 + u_prnuSigma * rand_normal(sp);
        color *= gain;
    }

    fragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
```

## Pass 3: Dark Current + DSNU + Hot/Dead Pixels

```glsl
#version 330 core
// ════════════════════════════════════════════════════════════════════════════
//  T2 Pass 3 — Dark Signal: Current + DSNU + Hot Pixels + Dead Pixels
// ════════════════════════════════════════════════════════════════════════════
//  Adds temperature-dependent dark current with Arrhenius model.

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;
uniform vec2      u_resolution;
uniform int       u_frameNumber;

uniform float u_darkCurrentRef;     // dark current at reference temp (normalised)
uniform float u_dsnuSigma;          // DSNU sigma
uniform float u_hotPixelProb;       // hot pixel probability
uniform float u_hotPixelStrength;   // hot pixel multiplier
uniform float u_deadPixelProb;      // dead pixel probability

// ── Temperature model (Arrhenius) ──
uniform float u_sensorTemp;         // current temperature (Kelvin), 0 = disabled
uniform float u_refTemp;            // reference temperature (Kelvin, e.g. 298.15)
uniform float u_activationEnergy;   // eV (e.g. 0.56 for depletion SRH)

const float k_B = 8.617e-5;  // Boltzmann constant in eV/K

void main()
{
    vec2 fc = v_texCoord * u_resolution;
    vec3 color = texture(u_inputTexture, v_texCoord).rgb;

    // ── Temperature-dependent dark current ──
    float darkCurrent = u_darkCurrentRef;
    if (u_sensorTemp > 0.0 && u_refTemp > 0.0) {
        darkCurrent *= exp(u_activationEnergy / k_B
                          * (1.0 / u_refTemp - 1.0 / u_sensorTemp));
    }

    // ── Fixed-pattern (spatial seed) ──
    uint sp = rng_seed_spatial(fc);

    // DSNU: half-normal (always positive offset)
    float dsnu = abs(u_dsnuSigma * rand_normal(sp));

    // Hot pixel
    bool isHot = (rand_float(sp) < u_hotPixelProb);

    // Dead pixel (stuck at 0 or stuck at a fixed value)
    bool isDead = (rand_float(sp) < u_deadPixelProb);
    if (isDead) {
        // Determine stuck value: 80% stuck-low, 20% stuck-high
        float stuckVal = (rand_float(sp) < 0.8) ? 0.0 : 0.95;
        fragColor = vec4(vec3(stuckVal), 1.0);
        return;
    }

    float darkTotal = darkCurrent + dsnu;
    if (isHot) darkTotal += u_hotPixelStrength * darkCurrent;

    // ── Temporal dark current noise (Poisson) ──
    uint ts = rng_seed_temporal(fc, u_frameNumber + 7919);
    float darkNoise = float(sample_poisson(darkTotal * 1000.0, ts)) / 1000.0;

    fragColor = vec4(clamp(color + vec3(darkNoise), 0.0, 1.0), 1.0);
}
```

## Pass 4: Photon Shot Noise (unchanged from current)

```glsl
#version 330 core
// ════════════════════════════════════════════════════════════════════════════
//  T2 Pass 4 — Photon Shot Noise (Poisson)
// ════════════════════════════════════════════════════════════════════════════

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;
uniform float     u_photonScale;   // ≈ well capacity in normalised units
uniform int       u_frameNumber;
uniform vec2      u_resolution;

void main()
{
    vec2 fc = v_texCoord * u_resolution;
    vec3 c = max(texture(u_inputTexture, v_texCoord).rgb, vec3(0.0));

    uint st = rng_seed_temporal(fc, u_frameNumber);

    vec3 noisy;
    noisy.r = float(sample_poisson(c.r * u_photonScale, st)) / u_photonScale;
    noisy.g = float(sample_poisson(c.g * u_photonScale, st)) / u_photonScale;
    noisy.b = float(sample_poisson(c.b * u_photonScale, st)) / u_photonScale;

    fragColor = vec4(clamp(noisy, 0.0, 1.0), 1.0);
}
```

## Pass 5: Read Noise + Column FPN

```glsl
#version 330 core
// ════════════════════════════════════════════════════════════════════════════
//  T2 Pass 5 — Read Noise (Gaussian) + Column FPN
// ════════════════════════════════════════════════════════════════════════════

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;
uniform vec2      u_resolution;
uniform int       u_frameNumber;

uniform float u_readNoiseSigma;     // read noise sigma (normalised)
uniform float u_colFPN_offset;      // column FPN offset sigma
uniform float u_colFPN_gain;        // column FPN gain sigma

void main()
{
    vec2 fc = v_texCoord * u_resolution;
    vec3 color = texture(u_inputTexture, v_texCoord).rgb;

    // ── Column FPN (spatial, per-column) ──
    if (u_colFPN_offset > 0.0 || u_colFPN_gain > 0.0) {
        uint colSeed  = pcg_hash(uint(fc.x) * 2654435761u + 12345u);
        float cOffset = u_colFPN_offset * rand_normal(colSeed);
        uint colSeed2 = pcg_hash(colSeed + 99991u);
        float cGain   = 1.0 + u_colFPN_gain * rand_normal(colSeed2);
        color = color * cGain + vec3(cOffset);
    }

    // ── Read noise (Gaussian, temporal, per-channel) ──
    uint rs = rng_seed_temporal(fc, u_frameNumber + 15731);
    color.r += u_readNoiseSigma * rand_normal(rs);
    color.g += u_readNoiseSigma * rand_normal(rs);
    color.b += u_readNoiseSigma * rand_normal(rs);

    fragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
```

## Pass 6: ADC + Soft Saturation

```glsl
#version 330 core
// ════════════════════════════════════════════════════════════════════════════
//  T2 Pass 6 — ADC Quantisation + Soft Saturation
// ════════════════════════════════════════════════════════════════════════════

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;
uniform int   u_adcBits;        // ADC resolution (8, 10, 12, 14)
uniform float u_satKnee;        // where soft-clip begins (0.85 typical)

void main()
{
    vec3 c = texture(u_inputTexture, v_texCoord).rgb;

    // ── Soft saturation ──
    if (u_satKnee > 0.0 && u_satKnee < 1.0) {
        vec3 excess = max(c - u_satKnee, 0.0);
        float range = 1.0 - u_satKnee;
        c = min(c, vec3(u_satKnee))
          + range * (1.0 - exp(-excess / range));
    }
    c = clamp(c, 0.0, 1.0);

    // ── ADC quantisation ──
    if (u_adcBits > 0) {
        float levels = exp2(float(u_adcBits));
        c = floor(c * levels + 0.5) / levels;
    }

    fragColor = vec4(c, 1.0);
}
```

**Tier 2 Pipeline Order**: Vignetting → PRNU → Dark → Shot → Read+ColFPN → ADC

---

# Tier 3 — Physical Bayer Domain Pipeline

**Philosophy**: Full physical accuracy. Noise applied in Bayer (single-channel) domain, then demosaiced with MHC. Working in electron domain. ISP included.

## Pass 1: RGB → Bayer Mosaic + Electron Conversion

```glsl
#version 330 core
// ════════════════════════════════════════════════════════════════════════════
//  T3 Pass 1 — Bayer Mosaic (RGB → single-channel mosaic in electrons)
// ════════════════════════════════════════════════════════════════════════════
//  Converts rendered RGB image to a single-channel Bayer mosaic image.
//  Output is in electron counts stored in the red channel.
//  Pattern: RGGB (configurable via uniform).

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;
uniform vec2      u_resolution;
uniform float     u_fullWellCapacity;    // FWC in electrons (e.g. 20000)
uniform int       u_bayerPattern;        // 0=RGGB, 1=GRBG, 2=GBRG, 3=BGGR

// ── Quantum efficiency per channel (wavelength-dependent) ──
uniform float u_qe_red;    // e.g. 0.45
uniform float u_qe_green;  // e.g. 0.60
uniform float u_qe_blue;   // e.g. 0.35

void main()
{
    vec2 fc = v_texCoord * u_resolution;
    vec3 rgb = max(texture(u_inputTexture, v_texCoord).rgb, vec3(0.0));

    // Determine Bayer position
    int x = int(fc.x) % 2;
    int y = int(fc.y) % 2;

    // Determine which colour this pixel sees
    // RGGB pattern:  (0,0)=R  (1,0)=G  (0,1)=G  (1,1)=B
    float signal;
    float qe;

    // Pattern selection
    int idx = y * 2 + x;               // 0..3
    idx = (idx + u_bayerPattern) % 4;   // rotate pattern

    if      (idx == 0) { signal = rgb.r; qe = u_qe_red;   }
    else if (idx == 3) { signal = rgb.b; qe = u_qe_blue;  }
    else               { signal = rgb.g; qe = u_qe_green; }

    // Convert normalised signal to electron count (applying QE)
    float electrons = signal * u_fullWellCapacity * qe;

    // Store electron count in red channel; encode Bayer index in green
    fragColor = vec4(electrons, float(idx), 0.0, 1.0);
}
```

## Pass 2: Sensor Noise in Electron Domain

```glsl
#version 330 core
// ════════════════════════════════════════════════════════════════════════════
//  T3 Pass 2 — All Sensor Noise (Electron Domain, Bayer Mosaic)
// ════════════════════════════════════════════════════════════════════════════
//  Applied per-photosite to scalar electron values. This is the physically
//  correct domain for all noise sources.

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;      // .r = electrons, .g = bayer index
uniform vec2      u_resolution;
uniform int       u_frameNumber;
uniform float     u_fullWellCapacity;

// ── Noise parameters (in electron domain) ──
uniform float u_darkCurrent_e;      // dark current in e⁻ (already scaled by t_exp)
uniform float u_dsnuSigma_e;        // DSNU sigma in electrons
uniform float u_hotPixelProb;
uniform float u_hotPixelStrength;
uniform float u_deadPixelProb;
uniform float u_prnuSigma;          // PRNU sigma (fractional, e.g. 0.01)
uniform float u_readNoise_e;        // read noise in electrons RMS
uniform float u_colFPN_e;           // column FPN offset in electrons
uniform float u_satKnee;            // non-linearity knee (fraction of FWC)

void main()
{
    vec2 fc = v_texCoord * u_resolution;
    vec4 data = texture(u_inputTexture, v_texCoord);
    float electrons = data.r;       // signal in electrons
    float bayerIdx  = data.g;       // colour channel index

    uint sp = rng_seed_spatial(fc);

    // ── Dead pixel check ──
    if (rand_float(sp) < u_deadPixelProb) {
        float stuckVal = (rand_float(sp) < 0.8) ? 0.0 : u_fullWellCapacity * 0.95;
        fragColor = vec4(stuckVal, bayerIdx, 0.0, 1.0);
        return;
    }

    // ── PRNU (multiplicative gain per photosite) ──
    float prnuGain = 1.0 + u_prnuSigma * rand_normal(sp);
    electrons *= prnuGain;

    // ── Dark current: base + DSNU + hot pixel ──
    float dsnu = abs(u_dsnuSigma_e * rand_normal(sp));
    bool isHot = (rand_float(sp) < u_hotPixelProb);
    float darkTotal = u_darkCurrent_e + dsnu;
    if (isHot) darkTotal += u_hotPixelStrength * u_darkCurrent_e;
    electrons += darkTotal;

    // ── Shot noise (Poisson on total signal = photon + dark) ──
    //    This is correct: Poisson(signal + dark) gives proper
    //    combined shot noise including dark current shot noise.
    uint ts = rng_seed_temporal(fc, u_frameNumber);
    electrons = float(sample_poisson(max(electrons, 0.0), ts));

    // ── Column FPN (additive offset per column) ──
    if (u_colFPN_e > 0.0) {
        uint cSeed = pcg_hash(uint(fc.x) * 2654435761u + 12345u);
        electrons += u_colFPN_e * rand_normal(cSeed);
    }

    // ── Read noise (Gaussian) ──
    uint rs = rng_seed_temporal(fc, u_frameNumber + 15731);
    electrons += u_readNoise_e * rand_normal(rs);

    // ── Soft saturation (non-linear near FWC) ──
    electrons = max(electrons, 0.0);
    float kneeE = u_satKnee * u_fullWellCapacity;
    if (electrons > kneeE) {
        float excess = electrons - kneeE;
        float range  = u_fullWellCapacity - kneeE;
        electrons = kneeE + range * (1.0 - exp(-excess / range));
    }
    electrons = min(electrons, u_fullWellCapacity);

    fragColor = vec4(electrons, bayerIdx, 0.0, 1.0);
}
```

## Pass 3: ADC (Electron → Digital Number)

```glsl
#version 330 core
// ════════════════════════════════════════════════════════════════════════════
//  T3 Pass 3 — ADC: Electron Count → Digital Number
// ════════════════════════════════════════════════════════════════════════════

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;
uniform float     u_fullWellCapacity;
uniform int       u_adcBits;          // 10, 12, 14
uniform float     u_blackLevel;       // pedestal in DN (e.g. 64 for 12-bit)

void main()
{
    vec4 data = texture(u_inputTexture, v_texCoord);
    float electrons  = data.r;
    float bayerIdx   = data.g;

    // Convert electrons to digital numbers
    float maxDN   = exp2(float(u_adcBits)) - 1.0;
    float gain    = maxDN / u_fullWellCapacity;   // DN per electron
    float dn      = electrons * gain + u_blackLevel;

    // Quantise (floor to integer DN)
    dn = clamp(floor(dn + 0.5), 0.0, maxDN);

    fragColor = vec4(dn, bayerIdx, 0.0, 1.0);
}
```

## Pass 4: Demosaicing (Malvar-He-Cutler)

```glsl
#version 330 core
// ════════════════════════════════════════════════════════════════════════════
//  T3 Pass 4 — Malvar-He-Cutler Demosaicing
// ════════════════════════════════════════════════════════════════════════════
//  Reconstructs RGB from noisy Bayer mosaic using optimised 5×5 filters.
//  Input: .r = DN value, .g = bayer index (0=R, 1/2=G, 3=B)
//  Output: RGB in DN space
//
//  Coefficients from Malvar, He, Cutler (2004), scaled by 8.
//  Division by 8 performed via multiply by 0.125.
// ════════════════════════════════════════════════════════════════════════════

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;
uniform vec2      u_resolution;

// Fetch raw DN value from mosaic at pixel offset (dx, dy)
float fetch(vec2 base, int dx, int dy) {
    vec2 uv = base + vec2(float(dx), float(dy)) / u_resolution;
    return texture(u_inputTexture, uv).r;
}

void main()
{
    vec2 fc   = v_texCoord * u_resolution;
    vec2 base = v_texCoord;

    int x = int(fc.x) % 2;
    int y = int(fc.y) % 2;

    float C  = fetch(base, 0, 0);   // centre pixel

    // 5×5 neighbourhood (only non-zero coefficient positions)
    float N  = fetch(base,  0, -1);  // north
    float S  = fetch(base,  0,  1);  // south
    float E  = fetch(base,  1,  0);  // east
    float W  = fetch(base, -1,  0);  // west
    float NE = fetch(base,  1, -1);
    float NW = fetch(base, -1, -1);
    float SE = fetch(base,  1,  1);
    float SW = fetch(base, -1,  1);
    float N2 = fetch(base,  0, -2);
    float S2 = fetch(base,  0,  2);
    float E2 = fetch(base,  2,  0);
    float W2 = fetch(base, -2,  0);

    float R, G, B;

    if (x == 0 && y == 0) {
        // ── Red pixel: R is known ──
        R = C;
        // Green: MHC filter for G at R location
        G = (4.0*N + 4.0*S + 4.0*E + 4.0*W
             + 2.0*C - N2 - S2 - E2 - W2) * 0.125;
        // Blue: MHC filter for B at R location
        B = (4.0*NE + 4.0*NW + 4.0*SE + 4.0*SW
             + 6.0*C - 1.5*(N2+S2+E2+W2)) * 0.125;
    }
    else if (x == 1 && y == 1) {
        // ── Blue pixel: B is known ──
        B = C;
        G = (4.0*N + 4.0*S + 4.0*E + 4.0*W
             + 2.0*C - N2 - S2 - E2 - W2) * 0.125;
        R = (4.0*NE + 4.0*NW + 4.0*SE + 4.0*SW
             + 6.0*C - 1.5*(N2+S2+E2+W2)) * 0.125;
    }
    else if (x == 1 && y == 0) {
        // ── Green pixel on red row ──
        G = C;
        R = (5.0*C + 4.0*(W+E) - (N2+S2-E2-W2+NE+NW+SE+SW)*0.5
             - N - S) * 0.125;
        B = (5.0*C + 4.0*(N+S) - (E2+W2-N2-S2+NE+NW+SE+SW)*0.5
             - E - W) * 0.125;
    }
    else {
        // ── Green pixel on blue row ──
        G = C;
        B = (5.0*C + 4.0*(W+E) - (N2+S2-E2-W2+NE+NW+SE+SW)*0.5
             - N - S) * 0.125;
        R = (5.0*C + 4.0*(N+S) - (E2+W2-N2-S2+NE+NW+SE+SW)*0.5
             - E - W) * 0.125;
    }

    fragColor = vec4(max(R,0.0), max(G,0.0), max(B,0.0), 1.0);
}
```

## Pass 5: ISP (White Balance + CCM + Gamma)

```glsl
#version 330 core
// ════════════════════════════════════════════════════════════════════════════
//  T3 Pass 5 — Image Signal Processor
// ════════════════════════════════════════════════════════════════════════════
//  Black level subtraction → white balance → CCM → gamma

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;

// ── ISP parameters ──
uniform float u_blackLevel;       // same as ADC pass, in DN
uniform float u_maxDN;            // 2^bits - 1
uniform vec3  u_whiteBalance;     // per-channel multipliers (e.g. 1.8, 1.0, 1.4)
uniform mat3  u_ccm;              // 3×3 colour correction matrix (sensor→sRGB)
uniform float u_gamma;            // output gamma (2.2 for sRGB)

void main()
{
    vec3 rgb = texture(u_inputTexture, v_texCoord).rgb;

    // ── Black level subtraction + normalise to [0,1] ──
    rgb = max(rgb - u_blackLevel, 0.0) / (u_maxDN - u_blackLevel);

    // ── White balance ──
    rgb *= u_whiteBalance;

    // ── Colour correction matrix ──
    rgb = u_ccm * rgb;

    // ── Gamma (linear → sRGB perceptual) ──
    rgb = clamp(rgb, 0.0, 1.0);
    rgb = pow(rgb, vec3(1.0 / u_gamma));

    fragColor = vec4(rgb, 1.0);
}
```

---

# Tier Comparison

| Feature | T1 (Single-Pass) | T2 (Modular RGB) | T3 (Physical Bayer) |
|---------|:-:|:-:|:-:|
| Shot noise (Poisson) | ✅ | ✅ | ✅ |
| Dark current + DSNU | ✅ | ✅ | ✅ (electrons) |
| Hot pixels | ✅ | ✅ | ✅ |
| Dead pixels | ❌ | ✅ | ✅ |
| PRNU (single gain) | ✅ | ✅ | ✅ |
| PRNU (per-channel) | ❌ | ✅ | ✅ (per-photosite) |
| Read noise | ✅ | ✅ | ✅ (electrons) |
| Column FPN | ✅ | ✅ | ✅ (electrons) |
| Vignetting | ❌ | ✅ | via T2 pre-pass |
| Lens distortion | ❌ | ✅ | via T2 pre-pass |
| Temperature model | ❌ | ✅ (Arrhenius) | ✅ (Arrhenius) |
| Bayer mosaic | ❌ | ❌ | ✅ |
| Demosaicing (MHC) | ❌ | ❌ | ✅ |
| Soft saturation | ✅ | ✅ | ✅ |
| Quantisation (ADC) | ✅ | ✅ | ✅ (electron→DN) |
| Black level | ❌ | ❌ | ✅ |
| White balance | ❌ | ❌ | ✅ |
| CCM (sRGB) | ❌ | ❌ | ✅ |
| Gamma | ❌ | ❌ | ✅ |
| **Noise domain** | Normalised RGB | Normalised RGB | **Electrons (Bayer)** |
| **Passes** | 1 | 4-6 | 5 |
| **Accuracy** | ★★☆ | ★★★☆ | ★★★★★ |
| **Performance** | ★★★★★ | ★★★★ | ★★★ |

---

# Chromatic Aberration — Standalone Effect (Any Tier)

```glsl
#version 330 core
// ════════════════════════════════════════════════════════════════════════════
//  Chromatic Aberration (lateral)
// ════════════════════════════════════════════════════════════════════════════
//  Samples R, G, B at slightly different UV positions.

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;
uniform float     u_caStrength;   // pixel offset at corners (e.g. 2.0)
uniform vec2      u_resolution;

void main()
{
    vec2 center = vec2(0.5);
    vec2 dir = v_texCoord - center;
    float r2 = dot(dir, dir);

    vec2 offset = dir * r2 * u_caStrength / u_resolution;

    float R = texture(u_inputTexture, v_texCoord + offset).r;
    float G = texture(u_inputTexture, v_texCoord).g;
    float B = texture(u_inputTexture, v_texCoord - offset).b;

    fragColor = vec4(R, G, B, 1.0);
}
```

# Pixel Crosstalk — Standalone Effect (Tier 2/3)

```glsl
#version 330 core
// ════════════════════════════════════════════════════════════════════════════
//  Pixel Crosstalk (optical + electrical approximation)
// ════════════════════════════════════════════════════════════════════════════
//  Simple nearest-neighbour coupling model.
//  Apply BEFORE noise in the pipeline for physical accuracy.

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_inputTexture;
uniform vec2      u_resolution;
uniform float     u_crosstalkFrac;   // fraction per neighbour (e.g. 0.02)

void main()
{
    vec2 px = 1.0 / u_resolution;

    vec3 C = texture(u_inputTexture, v_texCoord).rgb;
    vec3 N = texture(u_inputTexture, v_texCoord + vec2(0, -px.y)).rgb;
    vec3 S = texture(u_inputTexture, v_texCoord + vec2(0,  px.y)).rgb;
    vec3 E = texture(u_inputTexture, v_texCoord + vec2( px.x, 0)).rgb;
    vec3 W = texture(u_inputTexture, v_texCoord + vec2(-px.x, 0)).rgb;

    float a = u_crosstalkFrac;
    vec3 result = C * (1.0 - 4.0*a) + a * (N + S + E + W);

    fragColor = vec4(result, 1.0);
}
```

---

# Recommended Uniform Defaults

```
// ── Typical consumer CMOS at 25°C ──
u_fullWellCapacity  = 20000.0     // electrons
u_photonScale       = 500.0       // normalised-domain equivalent
u_darkCurrent_e     = 5.0         // e⁻ (for 33ms exposure)
u_darkCurrentRef    = 0.00025     // normalised equivalent
u_dsnuSigma_e       = 2.0         // electrons
u_dsnuSigma         = 0.0001      // normalised
u_hotPixelProb      = 0.001       // 0.1% of pixels
u_hotPixelStrength  = 50.0        // 50× base dark current
u_deadPixelProb     = 0.0001      // 0.01% of pixels
u_prnuSigma         = 0.01        // 1%
u_readNoise_e       = 3.5         // electrons RMS
u_readNoiseSigma    = 0.000175    // normalised (3.5/20000)
u_colFPN_offset     = 0.0005      // normalised
u_colFPN_gain       = 0.001       // fractional
u_colFPN_e          = 1.0         // electrons
u_adcBits           = 12
u_blackLevel        = 64.0        // DN
u_satKnee           = 0.85
u_gamma             = 2.2
u_whiteBalance      = vec3(1.8, 1.0, 1.4)  // daylight approx
u_sensorTemp        = 298.15      // 25°C
u_refTemp           = 298.15
u_activationEnergy  = 0.56        // eV (depletion SRH)
u_qe_red            = 0.45
u_qe_green          = 0.60
u_qe_blue           = 0.35

// ── Optical defaults ──
u_vignetteStrength  = 0.3
u_distortionK1      = 0.0
u_caStrength        = 1.5
u_crosstalkFrac     = 0.02
```
