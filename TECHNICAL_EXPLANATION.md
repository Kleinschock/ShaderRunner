# Sensor Noise Simulation — Technical Deep Dive

This document explains every design decision, every line of shader logic, every alternative
that was considered, and the physics behind this real-time sensor noise simulator. After reading
this, you should be able to understand the "why" and "how" deeply enough to recreate the entire
system from scratch.

---

## Table of Contents

1. [The Physics: What Is Sensor Noise?](#1-the-physics-what-is-sensor-noise)
2. [The Core Problem: Random Numbers on a GPU](#2-the-core-problem-random-numbers-on-a-gpu)
3. [noise_utils.glsl — The Shared Foundation](#3-noise_utilsglsl--the-shared-foundation)
4. [photon_noise.frag — Shot Noise](#4-photon_noisefrag--shot-noise)
5. [prnu.frag — Photo-Response Non-Uniformity](#5-prnufrag--photo-response-non-uniformity)
6. [dark_noise.frag — Dark Current, DSNU, and Hot Pixels](#6-dark_noisefrag--dark-current-dsnu-and-hot-pixels)
7. [read_noise.frag — Readout Noise](#7-read_noisefrag--readout-noise)
8. [Pipeline Order: Why This Sequence Matters](#8-pipeline-order-why-this-sequence-matters)
9. [The Modular Architecture](#9-the-modular-architecture)
10. [Performance Analysis](#10-performance-analysis)
11. [Alternatives That Were Considered](#11-alternatives-that-were-considered)

---

## 1. The Physics: What Is Sensor Noise?

Every digital camera sensor — from your phone to the Hubble Space Telescope — suffers from
multiple noise sources. These noise sources are well-characterised by semiconductor physics
and can be categorised into two fundamental types:

### Temporal Noise (changes every frame)
- **Photon (shot) noise**: The quantum nature of light means photons arrive randomly.
  If a pixel expects 100 photons, it might actually receive 93 or 108. This randomness
  follows a Poisson distribution where the variance equals the mean.
- **Dark current shot noise**: Thermal energy generates electrons even without light.
  These also arrive randomly (Poisson).
- **Read noise**: The electronics that convert accumulated charge to a digital number
  introduce Gaussian noise from amplifiers and ADCs.

### Fixed-Pattern Noise (same every frame, different per pixel)
- **PRNU (Photo-Response Non-Uniformity)**: Manufacturing imperfections cause each pixel
  to have a slightly different sensitivity (quantum efficiency). One pixel might convert
  photons to electrons at 99.2% efficiency, its neighbour at 100.8%. This is a
  multiplicative effect.
- **DSNU (Dark-Signal Non-Uniformity)**: Each pixel generates a slightly different amount
  of dark current. Some pixels run "hotter" than others.
- **Hot pixels**: Extreme outlier pixels with dramatically elevated dark current, caused
  by crystal lattice defects in the silicon. These show up as bright dots, especially in
  long-exposure or high-ISO images.

### Key Insight: Signal-Dependent vs Signal-Independent

This distinction is critical for implementation:

- **Shot noise** is signal-dependent: more photons → more noise (but better SNR).
  SNR = sqrt(N) for N photons. A pixel receiving 10,000 photons has SNR = 100,
  while a pixel receiving 100 photons has SNR = 10.
- **Read noise** is signal-independent: the electronics add the same noise whether
  the pixel is bright or dark.
- **PRNU** is signal-dependent (multiplicative): a 1% PRNU means ±0.01 × signal.
- **DSNU** is signal-independent (additive): a fixed offset regardless of brightness.

---

## 2. The Core Problem: Random Numbers on a GPU

GPUs are deterministic parallel machines. Every fragment shader invocation runs
independently, with no shared state. This means we cannot use a traditional sequential
PRNG (like Mersenne Twister) because:

1. There is no way to maintain a global state counter across millions of parallel threads.
2. Each pixel needs its own independent random stream.
3. We need the randomness to be different each frame (for temporal noise) but identical
   across frames for the same pixel (for fixed-pattern noise).

### The Solution: Hash-Based RNG

Instead of a sequential PRNG, we use a **hash function** as our RNG. The idea:

    random_number = hash(pixel_x, pixel_y, frame_number)

This gives us:
- **Parallelism**: Each pixel computes its own random number independently.
- **Determinism**: Same inputs always produce the same output (important for fixed-pattern noise).
- **No shared state**: Nothing to synchronise between threads.

### Why PCG?

I chose the PCG (Permuted Congruential Generator) hash for several reasons:

**What PCG does**: It takes a 32-bit integer and produces a new 32-bit integer that
appears random. The specific variant used is:

```glsl
uint pcg_hash(uint input_state)
{
    uint state = input_state * 747796405u + 2891336453u;
    uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
```

Line by line:
- `state = input_state * 747796405u + 2891336453u` — This is a Linear Congruential
  Generator (LCG) step. The constants are carefully chosen to give a full period of 2^32.
  747796405 and 2891336453 are specifically the multiplier and increment from PCG's
  reference implementation.
- `((state >> ((state >> 28u) + 4u)) ^ state)` — This is the "permutation" step. It
  takes the top 4 bits of `state` to decide how far to shift, then XORs the shifted
  value with the original. This creates a data-dependent rotation that breaks linear
  patterns.
- `* 277803737u` — Another mixing multiplication. This constant was selected by the PCG
  author (Melissa O'Neill) for good avalanche properties.
- `(word >> 22u) ^ word` — A final XOR-shift to mix the high bits back down.

**Alternatives I considered:**

1. **xxHash**: Slightly better statistical quality, but requires more operations (4-5
   multiply-XOR rounds). Since we call the hash function many times per pixel, the speed
   difference matters. PCG gives us good-enough quality at lower cost.

2. **Wang hash**: Very fast (2 operations), but has known statistical weaknesses. Adjacent
   inputs can produce correlated outputs, which would create visible patterns in the noise.

3. **Blue noise / precomputed noise textures**: Would give the best visual quality (noise
   is evenly distributed, no clumping). But requires a texture lookup per sample, burns
   a texture unit, and the tile size limits the pattern period. For photon noise
   specifically, blue noise would be physically wrong — real photon noise IS clumpy.

4. **GPU-side Mersenne Twister**: Statistically excellent, but requires ~2.5KB of state
   per thread. With millions of pixels, that's gigabytes of state memory. Completely
   impractical.

**The verdict**: PCG gives us the best balance of speed, statistical quality, and
simplicity for a GPU shader. It passes TestU01's SmallCrush battery and is fast enough
for real-time use.

---

## 3. noise_utils.glsl — The Shared Foundation

This file contains all the random number generation and sampling utilities. It is NOT
a standalone shader — the C++ code prepends it to each fragment shader at load time via
string concatenation. This avoids the problem that GLSL has no native `#include` mechanism.

### Two Seeding Strategies

This is one of the most important design decisions in the entire system.

**Temporal seed** — `rng_seed_temporal(fragCoord, frame)`:
```glsl
return pcg_hash(x + pcg_hash(y + pcg_hash(f)));
```
Uses nested hashing of (frame → y → x) to combine all three values into one seed.
The nesting order matters: by hashing `f` first, then mixing `y`, then `x`, we ensure
that changing any one of the three inputs completely changes the output. Simple addition
(`hash(x + y + f)`) would cause collisions: pixel (3,2) on frame 0 would produce the
same seed as pixel (0,2) on frame 3.

**Spatial seed** — `rng_seed_spatial(fragCoord)`:
```glsl
return pcg_hash(x * 1664525u + pcg_hash(y * 1013904223u + 374761393u));
```
No frame number. Uses different constants (1664525 and 1013904223, which are the
classic Numerical Recipes LCG constants) to decorrelate from the temporal seed. The
offset 374761393 is a large prime that ensures pixel (0,0) doesn't start at a trivial
seed value.

**Why two seeds?** Fixed-pattern noise (PRNU, DSNU, hot pixels) must produce the same
pattern every frame — that's what "fixed-pattern" means. A sensor's hot pixel is always
hot, in every frame. If we used the temporal seed, the hot pixel would jump around the
image, which is physically wrong. The spatial seed guarantees the pattern is static.

### Converting Hash Output to Useful Distributions

**Uniform float [0, 1)**:
```glsl
float rand_float(inout uint state)
{
    state = pcg_hash(state);
    return float(state) / 4294967296.0;  // 2^32
}
```
Simple: hash the state to advance it, then divide by 2^32 to normalise to [0,1).
The `inout` keyword is crucial — it allows us to chain multiple calls from the same
pixel, each advancing the state so we get different values.

**Normal distribution N(0,1)** via Box-Muller transform:
```glsl
float rand_normal(inout uint state)
{
    float u1 = max(rand_float(state), 1e-10);  // avoid log(0)
    float u2 = rand_float(state);
    return sqrt(-2.0 * log(u1)) * cos(6.28318530718 * u2);
}
```

The Box-Muller transform converts two independent uniform samples into a normally
distributed sample. The formula is:

    Z = sqrt(-2 ln(U1)) * cos(2π U2)

where U1 and U2 are uniform random numbers in (0,1).

**Why Box-Muller instead of alternatives?**

1. **Ziggurat method**: Faster on CPU (uses table lookups), but the table would need
   to be stored as a constant array in the shader, and the branching pattern is GPU-
   unfriendly (divergent warps). Not worth the complexity.

2. **Central Limit Theorem (sum 12 uniform samples)**: Very fast, no transcendental
   functions, but only approximates a normal distribution. The tails are truncated at
   ±√3 ≈ ±1.73, which is terrible for simulating rare hot pixels or extreme noise events.

3. **Inverse CDF (erfinv)**: Mathematically clean but GLSL has no built-in erfinv, and
   polynomial approximations of it are slower than Box-Muller anyway.

Box-Muller wins because it is exact (not an approximation), compact (3 lines), and the
transcendental functions (log, sqrt, cos) map well to GPU hardware.

The `max(u1, 1e-10)` guard prevents `log(0)` which would produce infinity. The value
1e-10 is small enough that it has negligible impact on the distribution — log(1e-10) ≈
-23, giving sqrt(-2 * -23) ≈ 6.8, which is already at the extreme tail.

### Poisson Sampling — The Heart of Shot Noise

This is the most technically interesting part. Poisson noise is the defining characteristic
of photon noise, and sampling from a Poisson distribution on a GPU is non-trivial.

**Knuth's algorithm (for small λ):**
```glsl
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
```

This is Knuth's 1969 algorithm. The idea: generate uniform random numbers U1, U2, U3, ...
and multiply them together. Count how many you need before the product drops below
exp(-λ). That count is your Poisson sample.

Why it works: The number of uniform random numbers needed before their product drops below
exp(-λ) follows exactly a Poisson(λ) distribution. This is because -log(U) is
exponentially distributed, and the sum of exponential random variables crossing λ is
a Poisson process.

The `for` loop cap of 200 is a safety valve for real-time. In theory, for very large λ,
this loop could run thousands of iterations. In practice:
- The expected number of iterations is λ + 1.
- For λ = 30, that is about 31 iterations on average.
- We only use this method for λ < 30, so the cap of 200 is extremely conservative.

**Normal approximation (for large λ):**
```glsl
int poisson_large(float lambda, inout uint state)
{
    float result = lambda + sqrt(lambda) * rand_normal(state);
    return max(0, int(round(result)));
}
```

For large λ (≥ 30), the Poisson distribution is very well approximated by a Normal
distribution N(λ, √λ). This is a consequence of the Central Limit Theorem, since a
Poisson(λ) random variable can be thought of as the sum of λ independent Poisson(1)
random variables.

The approximation error is of order 1/√λ, so at λ = 30 the error is about 18%, but at
λ = 100 it is 10%, and at λ = 1000 it is 3%. For visual noise simulation, this is
imperceptible.

The `max(0, ...)` clamp prevents negative values, which are physically impossible (you
cannot receive negative photons). The `round()` discretises to integers, matching the
discrete nature of photon counts.

**The unified sampler:**
```glsl
int sample_poisson(float lambda, inout uint state)
{
    if (lambda < 0.001)
        return 0;
    else if (lambda < 30.0)
        return poisson_small(lambda, state);
    else
        return poisson_large(lambda, state);
}
```

Three branches:
1. **λ < 0.001**: Return 0. At this level, the probability of receiving even 1 photon
   is < 0.1%. Not worth the computation.
2. **λ < 30**: Use Knuth's exact algorithm. Accurate but O(λ) cost.
3. **λ ≥ 30**: Use Normal approximation. O(1) cost, accurate enough.

**Why 30 as the threshold?** This is a standard threshold in computational statistics.
At λ = 30, the Normal approximation matches the Poisson distribution within ~2% at the
peak and ~5% at the tails. Visually, you cannot tell the difference. Below 30, the
Poisson distribution becomes noticeably asymmetric (skewed right), and the Normal
approximation starts to look wrong — it allows negative values and is too symmetric.

**Alternatives considered for Poisson sampling:**

1. **Inverse CDF table lookup**: Pre-compute the Poisson CDF for a range of λ values and
   do a binary search. Fast for fixed λ, but in our case λ varies per pixel and per
   channel, so we would need a massive table or per-pixel computation anyway.

2. **Rejection sampling (Ahrens-Dieter)**: Faster than Knuth for medium λ (30-100), but
   we use the Normal approximation in that range, which is even faster.

3. **Transformed rejection with squeeze (Hörmann)**: The gold standard for CPU Poisson
   sampling, but the branching and auxiliary computations make it GPU-unfriendly.

4. **Direct look-up from a Poisson noise texture**: Pre-generate a large texture of
   Poisson-distributed values and sample it. Eliminates all per-pixel computation but
   has a fixed period (tile size), cannot adapt to varying λ, and uses texture memory.
   Would be appropriate for a fixed-noise-level application but not for real-time
   adjustable parameters.

---

## 4. photon_noise.frag — Shot Noise

This is the physically most important noise source. In well-lit conditions, shot noise
is typically the dominant noise source.

```glsl
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
```

### The photonScale Trick

The key insight is how we convert between the normalised [0,1] colour space and a
physically meaningful photon count:

1. **Scale up**: `expected_photons = pixel_value × photonScale`
   A pixel with value 0.5 and photonScale 100 expects 50 photons.

2. **Sample**: `actual_photons = Poisson(expected_photons)`
   Maybe we get 47 or 53.

3. **Scale back**: `output = actual_photons / photonScale`
   53 / 100 = 0.53. The pixel is slightly brighter than expected.

photonScale acts as a "virtual exposure" parameter:
- **High photonScale (1000+)**: The image looks clean. Each pixel expects thousands of
  photons, giving excellent SNR.
- **Low photonScale (10-50)**: Heavy grain. Dark areas become extremely noisy because
  a pixel with value 0.05 only expects 0.5 to 2.5 photons.

### Why Per-Channel?

We sample noise independently for R, G, and B. This is physically correct because:
- Each colour channel on a Bayer filter sensor has its own photosite.
- Photon arrivals in the red channel are independent of the green and blue channels.
- This independence produces the characteristic colour speckle of real camera noise.

If we applied the same noise value to all three channels, we would get monochromatic
(grey) noise, which is what you see in film grain but NOT in digital sensors.

### Why Temporal Seed?

Shot noise is genuinely random in the real world. Each frame of a video camera shows
different noise. We use `rng_seed_temporal` so the noise pattern changes every frame,
producing the characteristic "dancing" grain of real sensor noise.

---

## 5. prnu.frag — Photo-Response Non-Uniformity

```glsl
void main()
{
    vec2 fragCoord = v_texCoord * u_resolution;
    vec3 color = texture(u_inputTexture, v_texCoord).rgb;

    uint spatialState = rng_seed_spatial(fragCoord);
    float gain = 1.0 + u_prnuStrength * rand_normal(spatialState);
    vec3 modulated = color * gain;

    fragColor = vec4(clamp(modulated, 0.0, 1.0), 1.0);
}
```

### What PRNU Models

In a real sensor, each photosite (pixel) has a slightly different quantum efficiency —
the probability that an incoming photon generates an electron. Manufacturing processes
cannot make every single one of millions of photosites perfectly identical, so there are
small variations.

This manifests as a multiplicative gain per pixel: some pixels are very slightly more
sensitive, others slightly less. The gain distribution is typically Gaussian around 1.0:

    gain_i ~ N(1.0, σ_prnu)

For high-quality consumer cameras, σ is typically 0.5-2%. For scientific CCDs, it can be
as low as 0.1%.

### Why Spatial Seed?

PRNU is a fixed physical property of each pixel. The gain map does not change between
frames. When you take a flat-field calibration image in astronomy, you are measuring
exactly this PRNU pattern. We use `rng_seed_spatial` (no frame number) so the gain map
is identical in every frame.

This means if you pause the video and look at the PRNU pattern, then resume, the pattern
stays the same. This is physically correct and matches what real calibration workflows
observe.

### Same Gain for All Channels

I apply the same gain value to R, G, and B for a given pixel. This is because PRNU is
primarily a property of the photosite bulk material, not the colour filter. In reality,
there is a small colour-dependent PRNU component (the colour filter dyes have their own
non-uniformity), but the dominant effect is the per-site gain. For simplicity and
performance, I use a single gain per pixel.

**Alternative considered**: Per-channel PRNU with three independent gains. More accurate
but 3× the computation, and the visual difference is subtle. Could be added as an option
for scientific applications.

### Why Gaussian Distribution?

PRNU is well-modelled by a Gaussian because it arises from many small, independent
manufacturing imperfections. By the Central Limit Theorem, their combined effect
converges to a normal distribution. This is well-established in the sensor
characterisation literature (EMVA Standard 1288).

---

## 6. dark_noise.frag — Dark Current, DSNU, and Hot Pixels

This is the most complex shader because it combines three related but distinct phenomena.

```glsl
void main()
{
    vec2 fragCoord = v_texCoord * u_resolution;
    vec3 color = texture(u_inputTexture, v_texCoord).rgb;

    // Fixed-pattern component
    uint spatialState = rng_seed_spatial(fragCoord);
    float dsnuOffset = abs(u_dsnuStrength * rand_normal(spatialState));
    bool isHotPixel = (rand_float(spatialState) < u_hotPixelProbability);

    float darkContrib = u_darkCurrent + dsnuOffset;
    if (isHotPixel)
        darkContrib += u_hotPixelStrength * u_darkCurrent;

    // Temporal component
    uint temporalState = rng_seed_temporal(fragCoord, u_frameNumber + 7919);
    float darkNoise = float(sample_poisson(darkContrib * 1000.0, temporalState)) / 1000.0;

    vec3 noisy = color + vec3(darkNoise);
    fragColor = vec4(clamp(noisy, 0.0, 1.0), 1.0);
}
```

### Dark Current — The Baseline

Even with the lens cap on, a sensor generates electrons due to thermal energy in the
silicon. This is called dark current. It is:
- Proportional to temperature (doubles roughly every 6-8°C).
- Proportional to exposure time.
- Independent of incident light.

We model the mean dark current as `u_darkCurrent`, a normalised value in the same
[0,1] scale as the image. A typical value of 0.005 means dark current adds about 0.5%
signal level per frame.

### DSNU — Why abs()?

```glsl
float dsnuOffset = abs(u_dsnuStrength * rand_normal(spatialState));
```

DSNU is the per-pixel variation in dark current. I use a half-normal distribution
(`abs(Gaussian)`) rather than a plain Gaussian because dark current offsets are always
positive. A pixel cannot generate negative electrons. The half-normal distribution:
- Has support [0, ∞).
- Has mean = σ√(2/π) ≈ 0.798σ.
- Has the right skewness: most pixels are near the mean, a few have much higher offset.

**Alternative considered**: Log-normal distribution, which better models the heavy tail
of extreme pixels. But log-normal requires an extra exp() call and the visual difference
from half-normal is negligible at typical DSNU levels.

### Hot Pixels — Rare Outliers

```glsl
bool isHotPixel = (rand_float(spatialState) < u_hotPixelProbability);
```

Hot pixels are caused by crystal lattice defects that create extra charge generation
points. They show up as persistently bright dots that do not change position between
frames. Typical prevalence:
- New sensors: 0.01-0.05% of pixels (hotPixelProbability ≈ 0.0001 to 0.0005)
- Aged sensors: Can reach 0.1-0.5%
- Space environment: Much higher due to radiation damage

The hot pixel contributes `hotPixelStrength × darkCurrent` additional dark current.
A hotPixelStrength of 50 means the hot pixel generates 50× more dark current than a
normal pixel, making it noticeably bright even in a single frame.

**Why spatial seed?** Hot pixels are fixed defects. A hot pixel at position (342, 197)
is always hot, in every frame. Using the spatial seed guarantees this persistence.

### Temporal Dark Current Noise

```glsl
float darkNoise = float(sample_poisson(darkContrib * 1000.0, temporalState)) / 1000.0;
```

The actual dark electrons generated each frame follow a Poisson distribution, just like
photon arrivals. The mean is `darkContrib` (the base dark current plus DSNU offset plus
any hot pixel contribution), and the variance equals the mean.

The `* 1000.0 / 1000.0` scaling is necessary because dark current values are very small
in normalised units. Without scaling, `darkContrib` might be 0.005, which would make
Poisson sampling degenerate (λ < 0.001 → returns 0). By multiplying by 1000, we get
λ = 5, which produces meaningful Poisson variation.

### The Frame Offset "+7919"

```glsl
uint temporalState = rng_seed_temporal(fragCoord, u_frameNumber + 7919);
```

7919 is used to decorrelate the dark noise RNG stream from the photon noise RNG stream.
Without this offset, both shaders would use the same seed for the same pixel on the same
frame, producing correlated noise. 7919 is an arbitrary large prime — any large prime
works. The key is that `frame + 0` (photon noise) and `frame + 7919` (dark noise)
produce completely different hash chains.

### Why Same Dark Noise for All Channels?

```glsl
vec3 noisy = color + vec3(darkNoise);
```

We add the same dark noise value to all three channels. This is because dark current
is a property of the silicon bulk, not the colour filter. The thermal electron generation
rate is the same regardless of what colour filter sits above the photosite. In a real
Bayer sensor, dark current does not know or care about wavelength.

This is in contrast to shot noise, which IS per-channel (because each colour channel
receives different numbers of photons).

---

## 7. read_noise.frag — Readout Noise

```glsl
void main()
{
    vec2 fragCoord = v_texCoord * u_resolution;
    vec3 color = texture(u_inputTexture, v_texCoord).rgb;

    uint state = rng_seed_temporal(fragCoord, u_frameNumber + 15731);

    vec3 noise;
    noise.r = u_readNoise * rand_normal(state);
    noise.g = u_readNoise * rand_normal(state);
    noise.b = u_readNoise * rand_normal(state);

    fragColor = vec4(clamp(color + noise, 0.0, 1.0), 1.0);
}
```

### The Simplest Noise Source

Read noise is the simplest to implement because it is:
- **Gaussian**: The Central Limit Theorem guarantees this, since readout noise arises
  from many independent small noise sources in the amplifier chain.
- **Additive**: It adds to the signal, regardless of signal level.
- **Signal-independent**: A completely dark pixel gets the same amount of read noise
  as a fully saturated one.
- **Temporal**: Different every frame (new readout = new noise).

### Per-Channel Independence

Unlike dark noise, read noise IS independent per channel. In a real sensor, each colour
channel's amplifier has its own noise sources. So we call `rand_normal(state)` three
times, getting three independent Gaussian samples. The `inout uint state` ensures each
call advances to a new random value.

### Frame Offset 15731

Another large prime, chosen to decorrelate from both photon noise (offset 0) and dark
noise (offset 7919). This ensures that even if two effects happen to produce the same
seed for some pixel, their noise patterns are uncorrelated.

### Why Not Poisson?

Read noise is NOT Poisson-distributed. It comes from electronic amplifier noise, not
from discrete particle counting. The noise is continuous (analogue) rather than discrete,
and it is well-characterised as Gaussian in the literature. Using Poisson sampling
for read noise would be physically incorrect.

---

## 8. Pipeline Order: Why This Sequence Matters

The shaders are chained in this order:

    Scene → PRNU → Dark Noise → Photon Noise → Read Noise → Screen

This matches the physical signal chain in a real sensor:

1. **PRNU first**: Light hits the photosite. The quantum efficiency (gain) of this
   specific pixel determines how many photons are converted to electrons. This is a
   multiplicative effect on the incoming signal, so it must be applied before any
   noise sampling.

2. **Dark noise second**: Dark current electrons are generated thermally and added to
   the signal electrons. Now the pixel holds signal_electrons + dark_electrons. Both
   are in the same charge well and are indistinguishable.

3. **Photon noise third**: The total electron count (signal + dark current) is subject
   to shot noise. In reality, the shot noise happened during acquisition (it is
   inherent to the counting process), but mathematically we can model it as sampling:
   "if we expected N electrons total, we actually got Poisson(N)." This must happen
   AFTER dark current is added because the dark electrons also undergo shot noise.

4. **Read noise last**: The accumulated charge is read out by the ADC. During this
   readout process, the amplifier adds Gaussian noise. This happens AFTER all charge
   accumulation is complete, so it is the last effect.

**What would go wrong if we swapped the order?**

If we applied photon noise BEFORE dark current, the dark current would be added as a
clean signal without shot noise, which is physically wrong. Dark electrons are just as
"noisy" as photon electrons.

If we applied read noise BEFORE photon noise, the read noise would get Poisson-sampled,
amplifying it incorrectly. Read noise is independent of the Poisson process.

---

## 9. The Modular Architecture

### Why Separate Shaders Instead of One Big Shader?

I considered three architectures:

**Option A: Single monolithic shader (original implementation)**
- Pros: Single fullscreen pass, maximum performance.
- Cons: All noise types are entangled. Cannot toggle individual effects. Hard to
  test or debug individual noise sources. Adding a new noise type requires modifying
  the existing shader.

**Option B: Preprocessor defines in one shader**
```glsl
#ifdef ENABLE_PRNU
    // PRNU code
#endif
```
- Pros: Single pass, conditional compilation.
- Cons: Still one file, requires shader recompilation to toggle effects, limited
  modularity (cannot add new effects without modifying this file).

**Option C: Separate shaders, chained via multi-pass (chosen approach)**
- Pros: True modularity. Each effect is a standalone file. Adding a new noise type
  means writing one new .frag file and one new C++ header. Effects can be toggled,
  reordered, or used independently.
- Cons: Multiple fullscreen passes (one per effect). Each pass reads and writes a
  fullscreen texture.

I chose Option C because the user explicitly requested modularity, and the performance
cost of 3-4 extra fullscreen passes at 1280×720 is negligible on any modern GPU
(see performance analysis below).

### The INoiseEffect Interface

```cpp
class INoiseEffect {
    virtual std::string getFragmentSource() const = 0;
    virtual void setupUniforms(osg::StateSet* ss) = 0;
    virtual osg::ref_ptr<osg::NodeCallback> createUpdateCallback();
    virtual std::string getName() const = 0;
};
```

Every noise module implements this interface. The PostProcessChain asks each effect for:
1. Its GLSL source (the chain prepends noise_utils.glsl automatically).
2. Its uniforms (attached to the fullscreen quad's state set).
3. An optional update callback (for frame-counter uniforms).

This design means the chain does not need to know anything about the specifics of each
noise type. It just chains generic shader passes.

### The PostProcessChain — Texture Ping-Pong

The chain works like this:

```
Scene → RTT Camera → Texture0
Texture0 → PRNU shader  → Texture1
Texture1 → Dark shader  → Texture2
Texture2 → Shot shader  → Texture3
Texture3 → Read shader  → Screen (final pass, no RTT)
```

Each intermediate pass renders a fullscreen quad into an FBO-backed texture. The
next pass reads that texture as its input. The final pass renders directly to the
screen (POST_RENDER camera, no FBO).

The shader composition is done at load time:

```cpp
fragSource = versionLine + "\n" + m_utilsSource + "\n" + body;
```

This string concatenation means noise_utils.glsl functions are available in every
shader without any #include magic. The #version line from the effect shader is
preserved and placed at the top (GLSL requires #version to be the first line).

---

## 10. Performance Analysis

### Cost Per Fragment

| Operation | Approx. GPU Cycles |
|-----------|-------------------|
| PCG hash  | ~4 cycles (3 multiplies, 2 XORs, 1 shift) |
| rand_float | ~4 cycles (one PCG hash + division) |
| rand_normal (Box-Muller) | ~20 cycles (log, sqrt, cos + 2 rand_float) |
| sample_poisson (λ<30, avg) | ~4λ cycles (avg λ iterations × 4 cycles each) |
| sample_poisson (λ≥30) | ~24 cycles (one rand_normal + sqrt + round) |
| Texture sample | ~4-8 cycles |

### Per-Pass Cost (at 1280×720 = 921,600 pixels)

| Shader | Est. Cycles/Pixel | Notes |
|--------|------------------|-------|
| PRNU | ~30 | 1 spatial seed + 1 rand_normal + 1 multiply |
| Dark Noise | ~50-100 | spatial seed + rand_normal + rand_float + Poisson |
| Photon Noise | ~60-200 | temporal seed + 3× Poisson (depends on λ) |
| Read Noise | ~70 | temporal seed + 3× rand_normal |

### Total Overhead

At 1280×720, the 4 additional fullscreen passes cost approximately:
- 4 texture reads + 4 texture writes = 8 fullscreen memory operations
- Total compute: ~250-400 cycles per pixel
- At 1 GHz shader clock with 2048 ALUs: processes ~5 million pixels/ms
- Our 0.9M pixels: ~0.2 ms total

**Verdict**: On any GPU from the last 10 years, the 4-pass noise pipeline adds well
under 1 ms of frame time. At 60 FPS, your frame budget is 16.6 ms, so this is less
than 2% overhead.

---

## 11. Alternatives That Were Considered

### Compute Shader Approach
Instead of fragment shader passes, use a single compute shader that reads the scene
texture and writes the final noisy result. This eliminates the multi-pass overhead.
However, compute shaders require OpenGL 4.3, which would exclude some older hardware.
The fragment shader approach works on OpenGL 3.3, which is near-universally supported.

### Noise Texture Pre-Generation
Pre-generate large noise textures (e.g., 4096×4096 Poisson noise) on the CPU and sample
them in the shader. This eliminates all per-pixel RNG computation. However:
- The noise would repeat at the texture boundary (visible tiling).
- Cannot adapt to varying photon scale without multiple textures.
- Uses significant VRAM for the noise textures.
- The fixed-pattern noise approach already does this implicitly via deterministic hashing.

### Temporal Accumulation
Instead of applying noise per-frame, accumulate clean frames and add noise to the
accumulated result. Physically more accurate for long-exposure simulation but adds one
frame of latency and requires a persistent frame buffer. Not suitable for real-time
interactive use.

### Bayer Pattern Simulation
Real sensors have a Bayer colour filter array, meaning each pixel only measures one
colour. Demosaicing algorithms interpolate the missing colours, which affects the noise
spatial correlation structure. We skip this because:
1. It would require a very different pipeline (work in raw Bayer space).
2. The visual effect is subtle (slight colour noise correlation).
3. It would tie the implementation to a specific Bayer pattern.

### Gain and Offset Correction Simulation
Real cameras apply gain and offset corrections to compensate for PRNU and DSNU. We could
add a "calibration" step that partially removes the fixed-pattern noise. This would be
interesting for simulating calibrated vs uncalibrated sensor output, but adds complexity
without core benefit for the noise simulation use case.

---

## Summary

The system simulates all five major noise sources in a digital imaging sensor:

| Source | Type | Distribution | Seed | Shader |
|--------|------|-------------|------|--------|
| PRNU | Fixed-pattern, multiplicative | Gaussian(1, σ) | Spatial | prnu.frag |
| Dark current | Temporal baseline, additive | Poisson(DC) | Temporal | dark_noise.frag |
| DSNU | Fixed-pattern, additive | Half-Normal(σ) | Spatial | dark_noise.frag |
| Hot pixels | Fixed-pattern, additive | Bernoulli + constant | Spatial | dark_noise.frag |
| Shot noise | Temporal, signal-dependent | Poisson(signal) | Temporal | photon_noise.frag |
| Read noise | Temporal, signal-independent | Gaussian(0, σ) | Temporal | read_noise.frag |

The implementation prioritises:
1. **Physical accuracy**: Correct distributions, correct pipeline order, correct
   temporal/spatial behaviour.
2. **Real-time performance**: PCG hash (fast), adaptive Poisson sampling (O(1) for
   large λ), minimal passes.
3. **Modularity**: Each noise source is independent, toggleable, and reusable.
