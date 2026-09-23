# Joint RAW reconstruction: specification, provenance and evidence

The `latent.direct-cfa.2` operation extends the existing direct-CFA tiled
executor. It does not introduce another CFA interpreter, registration stack,
scene model, renderer, or learned-model runtime. Its output is **reference-exposure,
green-balanced camera-linear RGB**, with conditional variance and support. It is
not an AP1/D60 `SceneFrame`. White balance/color conversion and scene semantics
remain an explicit downstream boundary; no display encoding or clipping occurs
inside this operation.

## Public reference and independent-reimplementation boundary

The reference inspected was
[JSR at `6ab0f5bf76d2ccfbf7e21798cc575232ef9b272b`](https://github.com/y-g-jiang/Jiangtherapee-Super-Resolution-World-Best/tree/6ab0f5bf76d2ccfbf7e21798cc575232ef9b272b).
The README blob was `f39410b9592e1931a4479b182d41c6191e1ea451`.
Inspection covered the README, repository directory/tree metadata and
[v9.8 release notes](https://github.com/y-g-jiang/Jiangtherapee-Super-Resolution-World-Best/releases/tag/v9.8).

No repository-wide license grant was present. The third-party directory covers
LibRaw. The release notes explicitly say that those notices do not relicense
JSR application/Core. Public visibility of Python files and `.npz` assets is not
used as redistribution permission. The README's statement that no algorithm
source is published conflicts with the current tree; neither establishes a
license. The README still describes the full paper as forthcoming.

**No JSR algorithm source was opened for this implementation. No JSR source,
decompiled pseudocode, tables, weights, executable, extracted resources, or
converted model is included.** No JSR binary was executed, and no black-box or
numerical-parity claim is made. Latent's code, GLSL, constants, fixtures and
linear-algebra derivation were independently authored under this repository's
license. The released Windows application's performance/results are not Latent
measurements.

Terminology in the following ledger is intentional:

| Classification | Public behavior / engineering consequence |
| --- | --- |
| DOCUMENTED | JSR describes RAW-to-linear-RGB burst reconstruction before WB/display processing, with statistics organized by color and subpixel phase. |
| DOCUMENTED | Its controller/refiner use intensity-degree-aware normalization and restoration; conditional positive intensity scaling is distinct from identity reconstruction accuracy. |
| DOCUMENTED | It describes learned and legacy RGB paths, channel-specific LCA consistency across those paths, and a refinement network. |
| DOCUMENTED | It describes global homography plus bounded-degree residual geometry, global tile coordinates/halos, axis-constant artifact controls and sampling/content-aware quality prediction. |
| INFERRED | Geometry diversity, intensity scaling and tile-origin invariance are useful independent acceptance tests. These statements do not identify JSR's private coefficients or make its full behavior reproducible. |
| OWN DESIGN | Latent uses local quadratic RAW regression, dimensionless robust weighting, circular phase evidence, a leverage/SNR gate and a Gaussian fallback. No neural controller/refiner is emulated. |
| OWN DESIGN | The existing continuous guide registration remains canonical. Local source-to-reference Jacobians include affine and linear row terms. This is not JSR's homography/polynomial solver or a channel-LCA implementation. |
| MEASURED | The executable tests described below measure Latent, with independent continuous irradiance, explicit sensor integration and frozen-weight uncertainty oracles. They do not measure JSR. |

[Wronski et al., *Handheld Multi-Frame Super-Resolution* (2019)](https://arxiv.org/abs/1905.03277)
is an additional public algorithmic reference for direct irregular RAW sampling,
joint demosaicing/reconstruction and the limits of motion-dependent detail.
Neither that paper nor JSR is the source of Latent's test results or fitted
constants.

## Observation and policy boundaries

`SensorSampling` and `samplingChannelAt` remain the sole CFA authority.
Normalization supplies validated color-channel IDs, green balance, exposure/gain
normalization, clipping validity and variance in reference-camera units. The
kernel never guesses physical CFA from megapixels or interprets a second GPU
CFA enum. Grouped, cropped and remosaiced buffers keep their existing semantics.

`ReconstructionMotion` is a correspondence observation. Its affine-plus-row
Jacobian is inverted once per output position. Local residual motion is
piecewise constant within its observed guide tile. Source sample offsets are
expressed in reference photosite coordinates before fitting. A singular
row-augmented Jacobian is rejected, even when the affine part alone is invertible.
This does not make rolling-shutter geometry or registration uncertainty exact.

`DirectKernelPolicy` is delegated estimator policy. The default remains
conservative native-resolution reconstruction. The explicit
`DirectKernelPolicy::superResolution()` preset selects a 0.5-reference-pixel
kernel and a full local-texture alias allowance for well-registered bursts.
The output grid is still separate: requesting the preset does not silently
change resolution, burst membership, CFA mode, or capture parameters.
`quadraticStrength=0` is the same estimator's Gaussian ablation, not a second
implementation. A one-frame runtime plan specializes to that path and records
why; no single frame gains fictitious temporal/phase evidence.

## Independent estimator

For each output position and color, define normalized reference-coordinate
sample offsets `u,v` and basis

```
phi = [1, u, v, u*u, u*v, v*v]
G = sum_i w_i phi_i phi_i^T
b = sum_i w_i phi_i y_i
V = sum_i w_i^2 variance_i phi_i phi_i^T
```

Weights combine the spatial Gaussian, measured correspondence confidence and a
pooled, robust photometric precision. The precision floor is **dimensionless**:
`floor = relativeVarianceFloor * A^2`. `A^2` is the maximum reference-channel
second-moment estimate (mean squared + de-noised spatial variance + mean noise);
when all reference amplitudes are zero, available source statistics supply the
scale. Precision is `floor / max(floor, pooledNoise)`, bounded by one; noiseless
zero evidence has an explicit finite unit-precision case. This common scaling
does not change relative least-squares weights.

The residual cutoff applies to **both** the noise term and the allowed spatial
alias term. Applying it only to noise incorrectly rejected valid phase changes
in high-frequency RAW stripes. Spatial texture uses weighted Welford scatter.
For independent noise, its expected noise contribution is
`weightedMean(variance_i) - variance(weightedMean(y_i))`, not simply the variance
of the mean. For unknown spatial correlation, subtracting the full weighted
sample variance is conservative against inventing texture.

The fit solves the six-dimensional system

```
h_fit = inverse(G / G00 + lambda * diag(0,1,1,1,1,1)) * e0
```

with FP32 Cholesky. The intercept is not penalized, preserving constants. A
failed finite/positive-pivot check disables the fit. Normalized prediction
leverage `h_fit[0]` smoothly tapers it between half the declared leverage cap and
the cap. This rejects ill-supported polynomial extrapolation at cropped borders
or in sparse color support; it is not a calibrated posterior condition number.

The final linear functional mixes the fit with the Gaussian zeroth moment:

```
h = ((1 - modelMix) * e0 + modelMix * h_fit) / G00
value = h^T b
conditionalVariance = h^T V h
```

`modelMix` includes delegated strength, phase diversity, noise-corrected texture
SNR and leverage admission. A separate continuous spatial-support transition
retains the reference fallback. The fallback shares RAW samples with fusion;
its variance uses a correlated standard-deviation upper bound rather than an
independent-mixture fiction. No RGB clamp, learned bias, display curve or
brightness pedestal is added.

### Phase support is not a resolution promise

The accumulator records weighted circular moments at phase harmonics `x`, `y`,
`x+y`, and `x-y`, relative to the reference warp in global sensor coordinates.
Marginal circular variances are penalized by both cross-covariance and
pseudo-covariance. The resulting bounded diagnostic rejects repeated phases,
one-axis phases and diagonal/anti-diagonal phase degeneracy. It is a conservative
heuristic, not a full alias-identifiability matrix, MTF estimate, posterior
probability, content-aware PSNR predictor or prediction of an unshot frame's gain.
In particular it can miss useful integer-CFA shifts. It never turns frame count
alone into a claim of genuine 2x resolution.

### Signed weights and uncertainty

Fitted RAW coefficients may be negative. Keeping only diagonal parameter
uncertainty or the zeroth noise moment would give incorrect uncertainty. The
full six-basis noise moment matrix is retained.

For same-frame correlated noise (including remosaiced data), the separate bound
uses `q_f = sum_i w_i abs(phi_i) sigma_i`. Its variance contribution is
`abs(h)^T (sum_f q_f q_f^T) abs(h)`. Independent and bounded-correlated
contributions are separate, so mixed input models cannot accidentally apply
absolute fitted weights to a signed independent covariance matrix.

Effective frame support uses per-frame first moments `m_f = sum_i w_i phi_i`.
An incremental QR root `R` with `R^T R = sum_f m_f m_f^T` gives
`1 / ||R h||^2`. Expanding that Gram matrix caused catastrophic cancellation in
border cases with signed weights, despite close RGB results. The square-root
representation fixes that numerical issue without loosening differential gates.
Reference-fallback correlation is included in the frame-support expression.

All uncertainty is **conditional on the chosen correspondences, masks, weights,
model mix and noise observations**. It excludes model mismatch, wrong motion,
unknown inter-frame correlation and calibration error. No unconditional
calibrated posterior is claimed.

### Conditional intensity-scale equivariance

With fixed geometry and clipping/validity, scale the complete dependency support
by positive `c`, including noise variances by `c*c`. Welford means are degree one;
spatial/noise moments and the floor are degree two. Relative precision, residual
weights, phase statistics, SNR and leverage gates are degree zero. `G` and `h`
therefore stay invariant, `b` is degree one and `V` degree two. Thus the output
scales by `c` and conditional variance by `c*c`, up to FP32 error.

This is not arbitrary exposure invariance: changing physical shot/read noise,
saturation, support, correspondence estimation or calibration violates the
hypotheses. Latent's reference-anchored scale also is not JSR's documented
all-accepted-evidence controller/refiner normalization scheme.

## Execution, memory and compatibility

The CPU oracle and Vulkan shader implement the same bounded operation. The
shader receives two-vec4 warps, moment state and FP32 output; descriptor/stride
changes are compiled together. Tiling/arena admission uses actual C++ type sizes,
not a stale bytes-per-pixel literal. The shader's 96-byte push block is within
Vulkan's minimum guaranteed push-constant capacity. Raw input is streamed;
there is no full-resolution burst or full-resolution flow allocation hidden in
the estimator. Output/supplied-motion/source residency is charged separately.
Driver/allocator internal memory remains outside the arena bound and is not
misreported as included.

The per-output state is larger than the Gaussian baseline. This is a reference
and portable FP32 implementation, not a demonstrated mobile-throughput optimum.
Explicit traces report admitted bytes, reads/transfers, backend, timing, actual
kernel policy, output grid, fitted-channel count and mean phase/model evidence.

`ReconstructedPixel` grows with `samplingDiversity` and `modelBlend`; this is an
in-memory ABI change. The Android `LATENT-CAMERA-RGB-V1` file **remains 16 FP32
lanes / 64 bytes per pixel**. Its sink now explicitly packs the documented fields
in a bounded row arena instead of serializing a C++ struct. New per-pixel
diagnostics are available through the C++ API; that legacy file stores only its
original fields, while the JSON trace includes aggregate diagnostics. Kotlin
gets the algorithm version from the native report instead of duplicating it.

## Camera-linear to scene composition

`finishDirectSceneTile` is the deterministic FP32 boundary from the existing
camera RGB/coverage/variance payload to scene RGB. It calls the SAME
`cameraToSceneMatrix` resolver as native-grid demosaic finishing, including both
DNG matrix routes. Input is already balanced between G0/G1 by the reference
observation; finishing applies `[WB_R, mean(WB_G0, WB_G1), WB_B]`, then AP1/D60
conversion and the explicit positive finite `sceneScaleEV` coordinate. Sensor
correction flags are rejected here: lens shading and defect rejection belong to
RAW normalization. There is no clamp, re-quantization or second demosaic.

`reconstructRawScene` adapts the canonical tiled executor into that boundary.
Reference identity is explicit because it fixes both radiometry and the green
balance. An inconsistent requested green ratio fails before reading RAW. Output
RGB plus optional variance storage is admitted BEFORE allocating the image; its
actual vector capacity is also charged by the tiled planner. Color is finished
row-by-row directly into the final scene. CPU/Vulkan selection changes only the
RAW execution; scene finishing remains the FP32 reference operation. Existing
SDR/HDR rendering consumes the resulting ordinary `SceneFrame` unchanged.

The source burst, sequence, calibration, reference and ALL captured frame IDs
are retained in immutable lineage, including reduced/rejected members. Aggregate
direct moments do not expose per-frame regional tap counts: those contributions
remain explicitly unavailable (empty), not guessed from frame count. The native
`FusedRaw` route retains its existing richer regional audit. A missing RGB
channel cannot silently become black in `SceneFrame`; scene materialization
fails while `reconstructRawTiles` remains available for masked evidence output.

With camera-channel conditional variances `v_c` but no cross-color covariance,
the separate scene bound is

```
bound_s = (sceneScale * sum_c abs(M_sc) * WB_c * sqrt(v_c))^2
```

This triangle bound permits arbitrary cross-channel correlation. It retains the
RAW estimator's fixed-weight/support/geometry conditions; it is not a calibrated
posterior and does not include model, motion or calibration error. It stays in
`JointSceneResult::conditionalVarianceUpperBound`; `SceneFrame`'s single-RAW
shot/read noise representation is not populated with a fabricated fit.

## Validation and reproduction

`latent_joint_raw_tests` supplies independent 4x4 photosite integration of
continuous irradiance. Nine axis/diagonal sinusoid cases span 0.20, 0.35 and 0.60
cycles per reference photosite, with sixteen actual CFA-phase observations and
the explicit high-detail preset. Joint, Gaussian and single-RAW reconstructions
use exactly the same 0.5-step output grid and inputs. Tests require improvement
over both baselines, not merely a larger output file. They do not assert full
optical recovery, universal performance, or quality on real sensors.

Other gates include positive powers-of-two and nonbinary gain changes from
sub-black through super-white values; zero/signed constants; degenerate phases;
axis-constant cross-axis artifact controls; exact CPU tile/halo equivalence for
non-square grouped CFA and cropped 2x grids; malformed policies/Jacobians and
empty evidence. Independent per-tap coefficient oracles verify DC gain, signed
conditional variance, QR frame support and mixed/correlated bounds. Seeded
4096-trial Monte Carlo checks test the fixed operator, not adaptive uncertainty.

`latent_joint_raw_vulkan_tests` runs the same SR/gain/phase fixtures against the
CPU oracle and the actual Vulkan executor; it must not fall back in required-GPU
CI. Existing exhaustive grouped-CFA CPU/Vulkan checks retain their original
RGB/variance/frame/confidence tolerances and add phase/model comparisons.
A separate known-geometry comparison exercises the actual retained
`makeReferenceFusionSession -> finishTemporalFusion -> Malvar demosaic` route,
then explicitly bilinearly samples that native RGB at the SAME output grid.
It uses 16 known half-pixel Cartesian shifts and 12 independent R/G/B horizontal
and vertical stripe targets at 0.35/0.60 cycles per RAW photosite. To avoid a
motion-rejection straw man, this diagnostic gives legacy fusion a permissive
100-sigma cutoff and 1e-4 variance floor; every evaluated tap must retain effective
support above 15 frames. The test gates lower joint MSE against aperture-integrated
truth and unchanged flat spectral axes, and logs both errors. This measures
phase-preserving reconstruction versus actual native-grid collapse, not merely
versus the direct estimator's Gaussian ablation. It does not claim universal
real-camera quality or recommend those diagnostic fusion settings for capture.
`latent_joint_scene_tests` and its required-real-Vulkan variant prove the complete
RAW -> camera RGB -> scene -> SDR/HDR composition. They cover all four Bayer
orders, group sizes 1 through 4 with nonzero origins, 1x/2x grids, fractional
output coordinates, exact tiled/full equivalence, explicit non-first references,
frame-budget reduction with complete capture lineage, unequal green response,
K=1, cancellation, insufficient memory before source reads and all-clipped
failure. The tile oracle independently composes a double-precision color matrix
and enumerates all eight correlated noise signs to attain the variance bound.
Both DNG routes are compared with the established native-grid demosaic boundary
on constant spectral fields. Malformed coverage, NaN/Inf, arithmetic overflow,
invalid DNG/WB and scene-coordinate underflow-to-zero fail explicitly.

Scene differential tolerances PROPAGATE the existing camera budgets through the
absolute WB/matrix row norm: RGB `3e-5`; variance interval
`1e-7 + 1e-3*v`. Monotone square roots propagate the variance endpoints, including
near zero. Finishing-only checks allow 2e-6 relative FP32 color/bound error and
4e-6 back-normalized gain error for rounded multiply, square root and accumulation.
The native-grid DNG composition comparison permits 4e-6 relative error for the
extra demosaic operations. The unequal-green RAW test additionally permits
4e-4 absolute error for 12-bit ADC quantization. No raw comparison gate is relaxed.
Existing temporal/registration/Camera2/RAW transport suites remain required.
Android's existing installed-APK output-size check guards V1 wire compatibility.
Exact-head CI results and downloadable artifacts belong to the PR/checks, not a
stale success badge or hard-coded run number here.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLATENT_STRICT_WARNINGS=ON
cmake --build build --parallel 4
LATENT_REQUIRE_VULKAN=1 ctest --test-dir build --output-on-failure
# CPU-only configuration remains supported.
cmake -S . -B build-novk -DCMAKE_BUILD_TYPE=Release -DLATENT_ENABLE_VULKAN_RUNTIME=OFF
cmake --build build-novk --parallel 4
ctest --test-dir build-novk --output-on-failure
# Matched-grid, matched-policy ablation and bounded execution evidence.
./build/latent_highres_benchmark --width 192 --height 128 --frames 8 --chart --scale 2
./build/latent_highres_benchmark --width 192 --height 128 --frames 8 --chart --scale 2 --gaussian
./build/latent_highres_benchmark --width 192 --height 128 --frames 8 --chart --scale 2 --gpu
```

Remaining product/research work is explicit: independently trained controllers,
channel-specific LCA calibration, full forward optical models, more general
registration, calibrated content/geometry quality prediction, real-RAW
perceptual evaluation and physical Android throughput/thermal/quality evidence.
This increment does not promote a debug high-resolution path to the production
camera default or advertise verified 50/200 MP detail.
