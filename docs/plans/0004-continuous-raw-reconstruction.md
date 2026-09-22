# Plan 0004: Continuous observation reconstruction

Status: Active. PR #18 is intentionally stacked on unmerged PR #17 at
`a13a46a1f7c5d2eeec9a170ea6bd1a14f159d0c7`. PR #19 reconciles the parallel
registration increment on PR #18 (`3d5cdfcba89f295fa6be79653e0a1793a4073109`)
with a non-rewriting merge. The cubic guide is the only canonical solver;
the superseded bilinear implementation is not retained as another backend.
Scope: continuous registration and direct, noise-aware sample reconstruction;
capture/export/production increments follow their own applicable gates.

## Implemented increment

The coarse multi-hypothesis pyramid remains. Conditioned, bounded robust
Gauss-Newton on a cardinal-cubic Bayer-cell guide replaces quarter-pixel search.
Incomplete or partially clipped Bayer cells do not enter that guide; edge windows retain enough real
support. This guide is not a resampled RAW observation or a fusion backend.

Registration evidence separates photometric agreement from geometry:
noise-corrected gradient observability, a conditional linearized localization
scale, reciprocal error, and competing retained hypotheses. Flat/noisy-flat
regions are not declared geometrically observable. Periodic alternatives remain
ambiguous. Inconsistent local cycles reduce fusion confidence. The finite search
cannot certify global uniqueness; the localization scale is not an unconditional
calibrated posterior. Thresholds are Latent-owned conservative policy.

Unobservable global motion uses an explicit identity prior; unobservable local
motion uses the chosen global prior. Photometric compatibility is recomputed at
the actual prior vector, not at an abandoned noisy optimum. Unknown localization
and cycle errors stay unavailable, not zero. Native Android JSON schema 2 exposes
status, prior, guide support, conditional localization, cycle error and every
tile; installed-APK tests exercise both finite and null evidence. Admission uses
the actual evidence type sizes, not a hand-maintained per-tile constant.

Independent continuous sinusoidal irradiance is analytically integrated over a
unit-square photosite. The phase sweep covers all Bayer patterns. Poisson/read
realizations use fixed seeds and the C++ standard distributions; distributions,
not cross-standard-library random bit identity, are the fixture contract.

## Evidence and remaining gates

Before modification, the new 32-case phase gate failed: maximum 0.162788 px,
RMS 0.124461 px. After continuous fitting: maximum approximately 0.00505 px,
RMS 0.00376 px. Sixteen low-light realizations: RMS 0.12725 px, maximum 0.16126 px;
all errors fell inside the reported conditional 3-sigma scale in this experiment.
This is not proof of universal uncertainty calibration.

A second independent continuous-irradiance family covers four CFA patterns,
large displacement, Poisson/read noise, periodic and aperture ambiguity, noisy
flat fields, partial CFA clipping, piecewise local motion and invalid inputs.
On the same 32 observations, endpoint RMS (clean/noisy) is 0.128452/0.130264 px
for a13, 0.016419/0.086326 for the superseded bilinear increment, and
0.009067/0.048378 for the unified cubic implementation. Cubic fitting improves
accuracy but its host phase-sweep cost is approximately 1.8x bilinear. Neither
fixture family or existing difficult assertion was weakened in reconciliation.
The existing nonlinear-texture estimate is 0.815578/-1.23739 px for true
0.75/-1.25 px; it is not exact. Static 8-frame MSE ratio is 0.127311, effective
support 7.91129; uncertainty remains conditional on adaptive fusion weights.

Local strict Release no-Vulkan 9/9 and SwiftShader 11/11 passed on the unified
source. Exact-head Actions must pass before this increment is called verified;
Android schema/null-prior changes require new assemble/lint/JVM/installed-APK
instrumentation. Actions evidence is bound in the PR, not inferred from the
stacked base or superseded increment's green runs.

Direct sample reconstruction, conditional SR, explicit per-sample visibility,
computational RAW export, expanded capture policy, and reduced GPU traffic remain
implementation work, not capabilities established by this registration increment.

A fixed seven-frame Google HDR+ burst was acquired separately for evaluation:
`0006_20160722_115157_431`, object generations and checksums retained outside the
repository. No dataset image is committed. Dataset-derived crops retain CC-BY-SA
4.0 attribution; decoder rawpy 0.25.1/LibRaw 0.21.4 is an evaluation dependency,
not imported reconstruction code. Real observations are not ground-truth scenes.

## Public provenance

- [Hasinoff et al. 2016](https://graphics.stanford.edu/papers/hdrp/): RAW alignment/merge boundaries and multiscale hypotheses; [published errata](https://www.hdrplusdata.org/errata.txt) checked.
- [HDR+ dataset](https://www.hdrplusdata.org/dataset.html): original sensor observations, calibration and license.
- [Wronski et al. 2019](https://storage.googleapis.com/gweb-research2023-media/pubtools/5211.pdf): irregular CFA samples, noise-aware guidance and geometric limits of SR.
- [Night Sight](https://research.google/blog/night-sight-seeing-in-the-dark-on-pixel-phones/) and [bracketing](https://research.google/blog/hdr-with-bracketing-on-pixel-phones/): motion/integration tradeoffs, not device-specific constants.
- [Project Indigo](https://research.adobe.com/articles/indigo/indigo.html) and [Levoy's experimental platforms](https://graphics.stanford.edu/papers/camera20/): capture/untone RAW/render separation and programmable observation streams, not private implementation specifications.

All listed primary materials were read. No private report, vendor parameters,
third-party HDR+ source, or incompatible algorithm implementation was used.
Physical sensor/HAL/IMU/AHB/mobile performance/energy/thermal/image-quality claims
remain **UNVERIFIED — DEVICE ONLY**.
