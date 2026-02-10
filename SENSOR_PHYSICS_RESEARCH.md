# Sensor Physics Research Documentary

**Iterative Research Document — Physical Fidelity of Sensor Simulation**

This document is built through 4 iterations of reflection, online research, and self-correction. Each iteration refines, challenges, and strengthens the reasoning. The goal: understand every physical phenomenon a digital image sensor encounters, assess our current shader implementation against true physical reality, and document what would be required for a fully faithful simulation.

---

## Revision Log

| Iteration | Date | Focus |
|-----------|------|-------|
| 1 | 2026-02-10 | Initial reflected thoughts from first principles |
| 2 | 2026-02-10 | EMVA 1288 corrections, Arrhenius model, PRNU wavelength, 1/f noise + CDS |
| 3 | 2026-02-10 | Cosmic ray rates, pixel crosstalk, RTS noise, MHC details, PTC method, dead pixels |
| 4 | 2026-02-10 | Pipeline order confirmation, blooming physics, real sensor data, final synthesis |

---

# Part 1: RGB to Bayer — The Fundamental Misunderstanding

## 1.1 What a Sensor Actually Sees

A digital image sensor does **not** see colour. Each photosite is a monochrome light bucket — it counts photons via the photoelectric effect and reads out a single scalar value: a charge count.

To capture colour, a **Colour Filter Array (CFA)** is placed over the sensor. The most common is the **Bayer pattern** (Bryce Bayer, Kodak, 1976):

```
Row 0:  R  G  R  G  R  G  ...
Row 1:  G  B  G  B  G  B  ...
Row 2:  R  G  R  G  R  G  ...
Row 3:  G  B  G  B  G  B  ...
```

Each 2×2 superpixel: 1R, 2G, 1B. Green is doubled because human vision peaks at ~555 nm.

**Key realisation**: At the raw sensor level, each pixel has only ONE value. A red-filtered pixel receives white light but only measures the red component.

## 1.2 RGB to Bayer: The Reverse Operation

In shader simulation, we start with RGB (3 values/pixel) and convert to Bayer (1 value/pixel):

```glsl
int x = int(fragCoord.x) % 2;
int y = int(fragCoord.y) % 2;

float bayerValue;
if (x == 0 && y == 0)      bayerValue = color.r;  // Red
else if (x == 1 && y == 1)  bayerValue = color.b;  // Blue
else                         bayerValue = color.g;  // Green
```

Implications:
1. **Information loss**: 3 channels → 1. Two-thirds of colour data discarded (physically correct).
2. **Mosaic image**: Single-channel, finely patterned grey image with subtle colour cast.
3. **Demosaicing required**: Must reconstruct missing channels via interpolation.

## 1.3 Why Bayer Matters for Noise Simulation

Our current shaders operate in RGB space. What we miss:

| Gap | Description | Visual Impact |
|-----|-------------|---------------|
| Noise spatial correlation | Demosaicing spreads single-pixel noise → correlated colour blobs, not speckle | **High** |
| Green channel advantage | 2G per quad → better green SNR; our RGB treats all channels equally | Medium |
| Aliasing / moiré | Bayer undersamples ~2× per channel; fine details can false-colour | Medium |
| Colour noise character | Real noise is "splotchy" from demosaic; ours is random per-channel speckle | **High** |

## 1.4 The Correct Pipeline Order

**Iteration 4 — Confirmed by research**: Noise MUST be applied in the **Bayer domain** (before demosaicing) for physical accuracy. This is the standard approach in all sensor simulation literature (Stanford ISETCam, CVPR papers, IISW proceedings).

```
Rendered RGB Scene
    ↓
[1. Bayer Mosaic Shader] — select one channel per pixel based on position
    ↓
[2. Apply ALL noise in mosaic domain] — shot, dark current, DSNU, PRNU, read noise
    ↓
[3. Quantisation (ADC)] — convert to integer domain
    ↓
[4. Demosaicing Shader (MHC)] — reconstruct RGB
    ↓
[5. ISP] — white balance, CCM, gamma, NR, sharpening
    ↓
Display
```

**Why this order matters**: When noise is applied per-photosite (scalar value) and then demosaiced, the interpolation creates spatial correlations in the RGB output that perfectly match real camera noise character. Applying noise in RGB space after demosaicing gives the wrong correlation structure.

**For our current shaders**: We can continue using the RGB-domain approach as a "good enough" approximation, but the document should make clear this is a known simplification. A physically correct implementation would restructure the pipeline as above.

## 1.5 Malvar-He-Cutler — The GPU Demosaicing Algorithm

**Confirmed across all iterations**: MHC is the definitive choice for GPU demosaicing.

**Algorithm**: 8 distinct 5×5 linear filters for each combination of interpolated channel + pixel position. Each is bilinear interpolation + gradient corrections (Laplacians from neighbouring channels).

**Performance (Morgan McGuire, 2008)**:
- **40 simultaneous HD 1080p streams at 30 fps** (2728 Mpix/s)
- 2-3× faster than unoptimized GLSL
- Integer arithmetic with /16 via bitshifting
- Only **13 texture samples** per pixel (sparse 5×5 kernel)

**Implementation sketch**:
```glsl
// Optimized MHC: branch-free via swizzle selection
ivec2 pos = ivec2(fragCoord) % 2;  // Bayer position
// 4 possible (x,y) combinations select different coefficient sets
// Apply 5x5 convolution using 13 texture samples
// Result >> 4 (divide by 16)
```

## 1.6 Bayer Pattern Variations

| Pattern | Size | Used By | Notes |
|---------|------|---------|-------|
| RGGB/GRBG/GBRG/BGGR | 2×2 | 90%+ of sensors | Rotated Bayer variants |
| RGBW | 2×2 | Sony, Aptina | Clear pixel for low-light |
| X-Trans | 6×6 | Fujifilm | Reduced moiré without OLPF |
| Foveon | 3 layers | Sigma | Full RGB per site |
| Quad Bayer | 4×4 | Samsung, Sony | 2×2 binning in low light |

## 1.7 CFA Spectral Imperfection

Real Bayer filters don't have ideal bandpass responses — the red filter transmits some green and NIR. Our `bayerValue = color.r` assumes perfect separation. This matters for:
- NIR contamination (vegetation red edge at ~750 nm)
- Line spectra (LEDs, sodium lamps)
- Spectral metamerism

For general simulation: acceptable. For spectral accuracy: need multispectral rendering.

---

# Part 2: Dark Noise Taxonomy — Precise Definitions

## 2.1 The Complete Hierarchy

```
Dark Signal (total in darkness)
├── Dark Current (deterministic mean, f(T, t_exp))
│   ├── Dark Current Shot Noise (Poisson temporal variation)
│   └── Dark Current Drift (long-term evolution)
├── DSNU (spatial per-pixel variation, fixed-pattern)
│   ├── Hot Pixels (extreme outliers)
│   └── Dead Pixels (zero response)
├── Dark Signal Offset (electronic pedestal)
└── 1/f Component (slow temporal baseline fluctuation)
```

## 2.2 Dark Current (Signal, Not Noise)

Steady thermal electron generation, independent of light.

| Property | Value |
|----------|-------|
| Cause | Thermally generated e⁻-h⁺ pairs (depletion region + Si-SiO₂ interface) |
| T dependence | Arrhenius: doubles every ~5-10°C |
| Unit | e⁻/pixel/s |

**Iteration 4 — Typical Values for Simulation Targets**:

| Sensor Category | Dark Current (25°C) | Read Noise | Notes |
|----------------|---------------------|------------|-------|
| Scientific cooled CCD (-40°C) | 0.001 e⁻/px/s | 2-5 e⁻ | Best case, thermoelectrically cooled |
| Scientific sCMOS (air-cooled) | 0.55 e⁻/px/s | 1-3 e⁻ | e.g., Hamamatsu ORCA |
| Consumer DSLR/mirrorless | 0.5-5 e⁻/px/s | 2-8 e⁻ | Canon, Sony, Nikon at room temp |
| DJI Drone (1/1.3" CMOS) | ~1 e⁻/px/s | 2-5 e⁻ | Estimated from sensor class |
| Smartphone (high T, small pixel) | 5-50 e⁻/px/s | 3-10 e⁻ | Higher due to small px + warm |
| Automotive (40-85°C operating) | 10-100+ e⁻/px/s | 5-15 e⁻ | High temp → high dark current |
| Camera module (generic) | <1 e⁻/px/s at 25°C | - | OmniVision, Sony IMX spec |

**Key takeaway**: Our shader parameter `u_darkCurrentBase` should default to ~1-5 e⁻/px/s equivalent for typical consumer sensors. Automotive simulation would use 10-100× higher values.

## 2.3 Dark Current Shot Noise (Temporal)

Poisson(λ_dark). σ = √λ_dark. Changes every frame. Depends on T and t_exp, not scene.

## 2.4 DSNU (Fixed Pattern, Not "Noise")

Spatial variation of dark current across pixels. EMVA 1288 deliberately uses **"non-uniformity"** — DSNU is deterministic, repeatable, calibratable. Our spatial-seed approach correctly models this.

## 2.5 "Dark Noise" — EMVA 1288 Definition

**σ_d (temporal dark noise)** = total temporal noise in darkness = σ²_dark_shot + σ²_read + σ²_quant + σ²_1/f + ...

At short exposure: σ_d ≈ σ_read. At long exposure: σ_d ≈ √(σ²_read + λ_dark).

**Our shader separation** (dark_noise.frag + read_noise.frag) is more granular than EMVA 1288's single σ_d — this is a strength for simulation modularity.

## 2.6 Current Shader Assessment

| Aspect | Status | Notes |
|--------|--------|-------|
| DSNU (half-normal) | ✅ | Physically reasonable |
| Hot pixels (Bernoulli + spatial seed) | ✅ | Correct |
| Temporal dark current Poisson | ✅ | Correct |
| Dark current baseline | ✅ | Correct |
| `*1000.0/1000.0` scaling | ⚠️ | Should use electron domain |
| Temperature dependence | ❌ | Arrhenius needed |
| Exposure time dependence | ❌ | Linear accumulation |
| Dead pixels | ❌ | Always-zero pixels |

## 2.7 Arrhenius Dark Current Model

```
I_dark(T) = I_0 × exp(-ΔE / kT)
```

| Regime | Mechanism | ΔE | Doubling Temp |
|--------|-----------|-----|---------------|
| Low T (< ~20°C) | SRH depletion generation | Eg/2 ≈ **0.56 eV** | ~5.8°C |
| High T (> ~40°C) | Diffusion current | Eg ≈ **1.12 eV** | ~10°C |
| Interface states | Surface generation | **0.40 eV** | ~4°C |

**Meyer-Neldel Rule**: I_0 and ΔE are correlated across pixels → an "isokinetic temperature" exists where all pixels have equal dark current. Consequence: DSNU distribution shape changes with temperature.

**Shader formula**:
```glsl
float dc_at_T = u_dcRef * exp(u_Ea / k_B * (1.0/T_ref - 1.0/T));
```

## 2.8 Dead Pixels

Missing from our simulation. Types:
- **Stuck low**: Always dark (damaged photodiode, blocked lens)
- **Stuck high**: Always at fixed bright value (adjacent to but distinct from hot pixels)

Prevalence: <0.001% (new sensor) → 0.1%+ (aged/irradiated).

```glsl
if (rand_float(spatialState) < u_deadPixelRate)
    noisy = vec3(isStuckHigh ? u_stuckValue : 0.0);
```

---

# Part 3: Complete Physical Sensor Simulation

## 3.1 The Photon-to-Digital-Number Chain

```
Scene Spectral Radiance
    ↓ Atmospheric transmission (Beer-Lambert, Rayleigh, Mie)
    ↓ Optics (focal length, aperture, aberrations, vignetting, OLPF)
    ↓ IR cut filter
    ↓ Microlens array (fill factor enhancement)
    ↓ Colour filter array (Bayer mosaic)
    ↓ Photoelectric conversion (QE × spectral response)
    ↓ Charge accumulation (+ dark current + crosstalk)
    ↓ PRNU (multiplicative per-pixel gain)
    ↓ Shot noise (Poisson on total signal)
    ↓ Charge overflow / blooming (if near FWC)
    ↓ Non-linear response (soft saturation near FWC)
    ↓ Pixel readout (source follower + CDS)
    ↓ Column amplifier (column FPN)
    ↓ Read noise + 1/f noise + RTS noise
    ↓ ADC (quantisation + DNL)
    ↓ ISP (black level, linearise, demosaic, WB, CCM, gamma, NR, sharp)
    ↓ Output Image
```

## 3.2 Environmental Prerequisites

### Light Sources
- **Solar SPD**: ~5778K blackbody with Fraunhofer lines
- **Atmospheric path**: Rayleigh (∝λ⁻⁴), Mie, H₂O/CO₂/O₃ absorption
- **Time-of-day spectral shift**: Longer path → redder spectrum

### Scene
- **BRDFs**: Angle-dependent reflectance
- **Spectral reflectance**: Not just RGB — full spectral curves (vegetation red edge at 750 nm)
- **Self-emission**, fluorescence

### Atmosphere
- Beer-Lambert exponential extinction
- Wavelength-dependent scattering
- Aerosol (haze, fog, dust)

## 3.3 Optics

| Element | Effect | Difficulty |
|---------|--------|-----------|
| Lens transmission | ~10% loss (10-element) | Easy (uniform multiply) |
| Lateral CA | Colour fringing at edges | Medium (per-channel UV offset) |
| Longitudinal CA | Colour fringing in defocus | Hard |
| Barrel/pincushion distortion | Geometric deformation | Medium (UV remap) |
| Natural vignetting | cos⁴(θ) falloff | Easy |
| Mechanical vignetting | Physical barrel clipping | Medium |
| Diffraction (Airy) | Resolution loss at small f/# | Hard (frequency domain) |
| Lens flare/ghosts | Internal reflections | Hard |
| IR-cut filter | Blocks >700 nm | Implicit in RGB |
| OLPF | Intentional blur for anti-moiré | Medium (pre-filter) |

## 3.4 The Sensor

### Photoelectric Conversion
- **QE**: 50-80% peak (consumer), 90%+ (BSI scientific). Wavelength-dependent.
- **Fill factor**: 30-60% (front-illuminated), 80-95% (BSI)

### Charge Accumulation and Non-Linearity

**FWC**: 5,000–50,000 e⁻ (consumer), 100,000+ (large pixels).

**Non-linearity near saturation** (confirmed in research):
- Conversion gain degradation: Floating diffusion capacitance is voltage-dependent
- Source follower non-linearity: gain varies with output voltage
- **Linear FWC** is ~70-85% of absolute saturation (EMVA 1288 evaluates lower 70%)

```glsl
float softSaturate(float signal, float fwc) {
    float linear_range = 0.85 * fwc;
    if (signal < linear_range) return signal;
    float excess = signal - linear_range;
    float knee = fwc - linear_range;
    return linear_range + knee * (1.0 - exp(-excess / knee));
}
```

### Blooming — Anti-Blooming Drain Physics

**Iteration 4 — New research on ABD mechanisms**:

When a pixel accumulates charge beyond its FWC, two things can happen:
1. **Without anti-blooming**: Excess charge diffuses into neighbouring pixels → bright streaks/bleeding
2. **With anti-blooming drain (ABD)**: Excess charge is shunted to a drain

Two ABD architectures:
- **Lateral Overflow Drain (LOD)**: An overflow gate adjacent to the photodiode with a controlled potential barrier. When charge exceeds the barrier height, it flows laterally into a reverse-biased drain diode → ground.
- **Vertical Overflow Drain (VOD)**: Drain beneath the photodiode. Excess charge flows vertically into the substrate. Uses epitaxial layer structure with controlled implant profiles.

**For simulation**: Modern CMOS sensors almost universally have anti-blooming structures. Blooming rarely occurs in well-designed sensors unless exposure is extremely overexposed (>100× FWC). Our simulation can:
1. Default: Hard-clip at FWC (sufficient for most applications)
2. Advanced: Soft saturation curve (see above)
3. Full physics: Model charge overflow to 4 nearest neighbours (compute shader needed)

### Complete Noise Source Inventory

| # | Source | Distribution | Type | Status |
|---|--------|-------------|------|--------|
| 1 | Photon shot noise | Poisson(N×QE) | Temporal, signal-dep | ✅ |
| 2 | Dark current (mean) | Deterministic | Systematic | ✅ |
| 3 | Dark current shot noise | Poisson(λ_dark) | Temporal | ✅ |
| 4 | DSNU | Half-normal (spatial) | Fixed-pattern | ✅ |
| 5 | Hot pixels | Bernoulli + constant | Fixed-pattern | ✅ |
| 6 | PRNU | N(1, σ) gain | Fixed-pattern, multiplicative | ✅ |
| 7 | Read noise | N(0, σ_read) | Temporal | ✅ |
| 8 | **1/f noise** | 1/f spectrum | Temporal (correlated) | ❌ |
| 9 | **Column FPN** | N(0, σ_col) per column | Fixed-pattern | ❌ |
| 10 | **Quantisation** | Uniform ±0.5 LSB | Systematic | ❌ |
| 11 | **RTS noise** | 2-state Markov | Temporal (discrete) | ❌ |
| 12 | **Dead pixels** | Zero/stuck value | Fixed-pattern | ❌ |
| 13 | **Pixel crosstalk** | Neighbour coupling | Spatial | ❌ |
| 14 | **Blooming** | Charge overflow | Signal-dep | ❌ |
| 15 | **Non-linearity** | Compression near FWC | Signal-dep | ❌ |
| 16 | **Cosmic ray hits** | Random bright events | Temporal (rare) | ❌ |
| 17 | **Row noise** | Per-row variation | Mixed | ❌ |

### 1/f Noise — Deep Analysis

**CDS does NOT eliminate 1/f noise** — it suppresses partially (high-pass filter), but 1/f remains dominant in many CIS.

Advanced techniques: CMS (multiple sampling), in-pixel chopping (~22 dB gain), buried-channel SF.

**Simulation**: Requires temporal state (persistent buffer or compute shader feedback).
- FFT: shape white noise as 1/f^α
- Random walk: Gaussian increment per frame → ~1/f² (Brownian, visually similar)
- Pre-generated library: cycle through temporally-correlated frames

### Column FPN

Vertical stripes. Two components:
- **Offset**: Signal-independent per-column offset (σ ≈ 0.1-1% of full scale)
- **Gain**: Signal-dependent per-column gain variation

```glsl
uint colSeed = pcg_hash(uint(fragCoord.x) * 2654435761u + 12345u);
float colOffset = u_colFPN_offset * rand_normal_from(colSeed);
float colGain = 1.0 + u_colFPN_gain * rand_normal_from(pcg_hash(colSeed + 99991u));
color = color * colGain + vec3(colOffset);
```

### RTS (Random Telegraph Signal) Noise

Discrete switching caused by single charge traps at Si-SiO₂ interface.

**Prevalence**: 1-2% of pixels in modern sCMOS, up to 5% in some sensors. **Increasing with process shrink** (smaller transistors → fewer traps but each trap has larger relative effect).

Not feasible in stateless fragment shader — needs persistent per-pixel state or precomputed RTS maps.

### Pixel Crosstalk

Two types:
- **Optical**: Diffraction/reflection from metal layers; worse at oblique angles
- **Electrical**: Lateral diffusion of photo-generated carriers; worse for NIR (deeper absorption)

Quantification: 1-5% optical + 2-10% electrical per neighbour (modern BSI sensors).

```glsl
float alpha = u_crosstalkFrac; // e.g. 0.02
vec3 result = center*(1.0-4.0*alpha) + alpha*(left+right+up+down);
```

### Cosmic Ray Hits

**HST (LEO) data**: 1.8 events/chip/s, ~6.7 pixels/event, ~3.8% of pixels in 2000s exposure.

| Environment | Rate | Notes |
|-------------|------|-------|
| Ground level | ~few/sensor/hour | Negligible for terrestrial |
| LEO | ~2 events/cm²/min | Significant for long exposures |
| GEO / deep space | Higher | SAA and no geomagnetic shielding |
| CMOS in LEO (2 years) | ~0.1% permanent hot pixels | Cumulative damage |

## 3.5 Signal Processing Chain

### Photon Transfer Curve (PTC) — The Standard Characterisation Method

Plots **noise² vs. mean signal** across exposure levels:

```
                    PRNU-dominated (σ² ∝ S²)
                   /
                  /
               Shot-noise-dominated (σ² ∝ S, slope=1 on log-log)
              /
             /
-----------    ← Read noise floor (σ² = const) 
                
0                                               Saturation
                     Mean Signal →
```

| Region | Reveals |
|--------|---------|
| Low signal (flat) | Read noise σ_read |
| Mid signal (slope 1) | Conversion gain K = 1/slope (e⁻/DN) |
| High signal (slope 2) | PRNU σ |
| Rollover | Full well capacity |
| Departure from slope 1 | Non-linearity onset (70-85% of FWC) |

**EMVA 1288 uses lower 70% of PTC** for parameter extraction.

**Validation strategy**: Generate synthetic PTC from our shader → compare against real sensor datasheets.

### Full ISP Pipeline

```
Raw Bayer (e⁻ → ADC → DN)
  ↓ Black level subtraction (remove dark pedestal)
  ↓ Linearisation (correct ADC non-linearity)
  ↓ Bad pixel correction (interpolate hot/dead)
  ↓ Flat-field correction (÷ PRNU/vignetting map)
  ↓ Demosaicing (MHC → RGB)
  ↓ White balance (per-channel multiply)
  ↓ CCM (3×3 → sRGB/AdobeRGB)
  ↓ Tone mapping / gamma (linear → perceptual)
  ↓ Noise reduction (spatial/temporal)
  ↓ Sharpening (counter OLPF + demosaic softening)
  ↓ Compression (JPEG quantisation)
Final Image
```

## 3.6 Environmental Effects

| Factor | Effect | Simulation Level |
|--------|--------|-----------------|
| Temperature | Dark current Arrhenius, read noise ∝ √T, QE shift | Medium (uniform) |
| Radiation (SEE) | Cosmic ray hits (instant bright dots) | Easy (Bernoulli) |
| Radiation (TID) | Cumulative dark current increase, new hot pixels | Hard (state over time) |
| Displacement damage | Permanent lattice defects → dark current increase | Hard |
| Mechanical vibration | Motion blur (PSF) | Engine camera model |
| EMI | Periodic noise patterns | Medium |

## 3.7 PRNU — Per-Channel Analysis

**Confirmed**: PRNU is wavelength-dependent. Each channel has its own pattern due to:
1. CFA dye non-uniformity (thickness/concentration variations)
2. Wavelength-dependent photodiode QE non-uniformity
3. Angle-dependent PRNU is colour-dependent (SPIE)

| Model | Use Case | Cost |
|-------|----------|------|
| Single gain (current) | General visualisation | 1 hash/px |
| Per-channel PRNU | Scientific accuracy | 3 hashes/px |
| Spectral PRNU | Hyperspectral | N hashes/px |

---

# Part 4: Feasibility and Priority

## 4.1 Shader Feasibility Tiers

### Tier 1: Already Implemented ✅
Photon shot noise, dark current + shot noise, DSNU, hot pixels, PRNU, read noise.

### Tier 2: Straightforward Additions 🔧

| Element | LOE | Implementation |
|---------|-----|----------------|
| Bayer mosaic + MHC demosaic | Medium | 2 shader passes; 13 texel fetches for MHC |
| Vignetting (cos⁴θ) | Easy | Distance-based multiply |
| Lens distortion | Easy-Med | UV coordinate remapping |
| Chromatic aberration | Medium | Per-channel UV offsets |
| Quantisation | Trivial | `floor(v * levels) / levels` |
| Soft saturation | Easy | Exponential compression curve |
| Column FPN | Easy | 1D spatial hash per column |
| Per-channel PRNU | Easy | 3× `rand_normal()` |
| Dead pixels | Trivial | Bernoulli + spatial seed → 0 |
| White balance + CCM + gamma | Easy | Multiply, 3×3 matmul, pow() |

### Tier 3: Complex but Feasible 🔶

| Element | Challenge |
|---------|-----------|
| 1/f noise | Temporal correlation → persistent buffer |
| Temperature-dependent dark current | Arrhenius parameterisation |
| Pixel crosstalk | Nearest-neighbour kernel |
| Cosmic ray hits (single-pixel) | Low-probability high-energy events |
| RTS noise | Two-state Markov, persistent state |
| Full ISP chain | Many stages, each individually simple |

### Tier 4: Engine-Level or Impractical 🔴
Full spectral rendering, cumulative radiation damage, atmospheric radiative transfer, multi-pixel cosmic ray tracks, lens flare/ghosts, spectral material textures.

## 4.2 Priority Ranking for Implementation

| # | Element | Impact | Effort | Rationale |
|---|---------|--------|--------|-----------|
| **1** | **Bayer + MHC demosaic** | ★★★★★ | Med | Biggest single gap. Fundamentally changes noise character. |
| **2** | **Vignetting** | ★★★★ | Easy | Universal optical artifact. Immediately recognisable. |
| **3** | **Column FPN** | ★★★★ | Easy | Classic CMOS signature. Makes noise look "digital." |
| **4** | **Quantisation** | ★★★ | Trivial | Visible in smooth gradients. One line of code. |
| **5** | **Per-channel PRNU** | ★★★ | Easy | More realistic colour non-uniformity. |
| **6** | **Soft saturation** | ★★★ | Easy | Realistic highlight roll-off. |
| **7** | **Dead pixels** | ★★ | Trivial | Complements existing hot pixels. |
| **8** | **ISP pipeline** | ★★★★ | Med | Makes output look like a real camera image. |

## 4.3 PRNU Interaction with Bayer

**Iteration 4 clarification**: In a real sensor, PRNU applies per-photosite — i.e., in the Bayer domain on scalar values. Each photosite has ONE quantum efficiency, applied to the ONE colour it sees through its filter. When we model per-channel PRNU in RGB space, we're approximating this correctly IF each channel's PRNU is independent. In Bayer domain, it would be even more correct: each pixel gets a single PRNU gain factor, but that factor depends on which colour filter covers it.

---

# Part 5: Integration Architecture

## 5.1 MATLAB Cross-Validation

```
MATLAB (ISETCam)                    Shader Pipeline
│                                    │
│ Scene spectral radiance            │ Rendered RGB
│ → Optics (spectral PSF)            │ → Bayer mosaic
│ → Sensor (spectral QE)             │ → PRNU
│ → Noise (shot, dark, read)         │ → Dark noise
│ → ADC                              │ → Shot noise
│ → ISP                              │ → Read noise
│                                    │ → Demosaic + ISP
│                                    │
└── Compare PTC curves ←────────────┘
```

**Validation method**: Generate synthetic PTC from shader output at multiple exposure levels. Compare against MATLAB PTC and real sensor EMVA 1288 datasheets.

## 5.2 Game Engine Embedding

```
Custom Engine / Unity / Unreal
│
├── Render scene → FBO
│
├── Post-processing chain:
│   ├── [Optional] Bayer mosaic
│   ├── Vignetting + distortion
│   ├── PRNU (spatial seed)
│   ├── Dark noise (spatial + temporal seed)
│   ├── Shot noise (signal-dependent Poisson)
│   ├── Read noise (Gaussian)
│   ├── Column FPN (1D spatial seed)
│   ├── Quantisation
│   ├── [Optional] MHC demosaic
│   └── ISP (WB, CCM, gamma)
│
└── Display
```

**Portability**: The core noise math (PCG hash, distribution samplers, physical model) is identical in GLSL, HLSL, Metal, CUDA, and C. No engine-specific features needed.

## 5.3 Working in Electron Domain vs Normalised Domain

**Iteration 4 — Final Recommendation**:

Currently we work in [0, 1] normalised space. For physical accuracy, we should ideally convert to electron counts:

```
signal_electrons = signal_normalised × FWC
```

Then apply noise in electron domain (where Poisson statistics are correct), then convert back:

```
signal_normalised = signal_electrons / FWC
```

This is important because Poisson noise depends on the absolute count, not normalised values. Our current approximation (`photonScale` parameter) simulates this by scaling before Poisson sampling, but working in true electron domain would make parameter correspondence to real sensor specs (from EMVA 1288 datasheets) direct and unambiguous.

---

# Part 6: Final Assessment — How Well Do Our Shaders Hold Up?

## 6.1 Against a Basic Camera Simulation

| Requirement | Coverage |
|-------------|----------|
| Shot noise statistics | ✅ Correct (Poisson) |
| Read noise statistics | ✅ Correct (Gaussian) |
| Dark current baseline | ✅ Correct |
| Dark current temporal noise | ✅ Correct (Poisson) |
| DSNU spatial pattern | ✅ Correct (spatial seed) |
| Hot pixels | ✅ Correct (Bernoulli + fixed) |
| PRNU gain variation | ✅ Correct (multiplicative) |
| **Verdict** | **~80% of basic noise physics covered** |

## 6.2 Against a Full Physical Simulation

| Gap | Impact | Priority |
|-----|--------|----------|
| No Bayer/demosaicing | **High** — wrong noise correlation structure | P1 |
| No vignetting | **High** — universal optical artifact missing | P2 |
| No column FPN | **Medium** — missing characteristic CMOS signature | P3 |
| No temperature dependence | **Medium** — limits dark current accuracy | P5+ |
| No per-channel PRNU | **Low-Medium** — subtle improvement | P5 |
| No ISP pipeline | **High** — output doesn't look like a "camera image" | P8 |
| No spectral rendering | Low (RGB sufficient for general use) | Tier 4 |

## 6.3 Self-Correction Summary Across All Iterations

| Claim | Original (Iter 1) | Corrected (Iter 2-4) |
|-------|-------------------|---------------------|
| Dark noise = dark current shot noise | ❌ | EMVA 1288: σ_d includes ALL temporal noise in darkness |
| CDS eliminates 1/f noise | ❌ | CDS suppresses partially; 1/f often remains dominant |
| Dark current doubles every 5.5-8°C | ⚠️ | Range is 5-10°C depending on activation energy |
| PRNU is wavelength-independent | ❌ | PRNU is measurably wavelength-dependent per channel |
| Noise can be applied in RGB domain equally | ⚠️ | Must apply in Bayer domain for correct spatial correlation |
| Hard clamp at saturation is correct | ❌ | Real sensors have soft saturation curve near FWC |
| Blooming is common | ⚠️ | Modern CMOS has anti-blooming; blooming rare in practice |

---

*Document complete — 4 iterations of research and self-correction. 11 online research queries. Major topics covered: RGB-to-Bayer conversion and MHC demosaicing, dark noise taxonomy with EMVA 1288 definitions, Arrhenius dark current model, comprehensive physical sensor simulation inventory (17 noise/effect sources), cosmic ray rates, pixel crosstalk, RTS noise, PTC validation method, blooming physics, and integration architecture for MATLAB and game engines.*
