# Continuous registration and geometry evidence

Status: Active. This increment is intentionally stacked on unmerged PR #17,
`feat/temporal-raw-android` at `a13a46a1f7c5d2eeec9a170ea6bd1a14f159d0c7`.
It does not extend #17 or claim joint super-resolution.

## Implemented

- Coarse-to-fine global hypotheses and tile translations retain their existing
  role; bounded, noise-weighted Lucas-Kanade with Huber descent replaces the
  quarter-pixel exhaustive refinement grid. The line search includes overlap
  penalties; requiring nondecreasing overlap would incorrectly trap identity.
- Geometry evidence is distinct from photometric fusion compatibility: debiased
  2D texture, tested global-basin margin, forward/backward error, and explicit
  identity/global priors. Scores/5% gates and the one-pixel cycle gate are
  Latent policy diagnostics, not sensor calibration or probability quantiles.
- Partial clipped CFA cells cannot change a registration proxy's spectral
  mixture. Non-finite normalized observations fail before search.
- Geometry and regional prior flags survive the native execution trace and
  Android JNI diagnostics. The admission estimate reserves the additional trace.

## Verification ledger

Independent continuous-irradiance samples, not an interpolated discrete input,
exercise four CFA patterns and four displacements, with and without Poisson/read
noise (32 cases). Existing reference/fusion fixtures and assertions are retained.

| Local Release GCC measurement | Baseline a13a46a | This increment |
| --- | ---: | ---: |
| Clean endpoint RMSE, sensor pixels | 0.128452 | 0.016419 |
| Poisson/read endpoint RMSE | 0.130264 | 0.086326 |
| Maximum clean / noisy endpoint error | 0.176918 / 0.176918 | 0.021031 / 0.141550 |
| 257x193x4 reference median, ms (3 repeats) | 163.155 | 104.085 |

Local strict no-Vulkan 8/8 and SwiftShader Vulkan 10/10 passed. Periodic texture,
noisy flat fields, aperture ambiguity, CFA clipping, local-motion interiors and
invalid input have separate gates. Local-motion maximum interior error was
0.0221 sensor pixels. The retained nonlinear-texture fixture still has about
0.52 pixels of bias per axis: interpolation/aliasing is not solved by this change.
No claimed physical-device result follows from these fixtures or host timings.

Latest-SHA Actions, Android reports/artifacts and final diff review are recorded
on the PR, not inferred from an earlier green commit. Android changes in this
increment require fresh assemble/lint/JVM/installed-APK instrumentation evidence.
At authoring, that independent second-environment gate is pending.

## Public rationale and boundaries

[Hasinoff et al. (2016)](https://graphics.stanford.edu/papers/hdrp/hasinoff-hdrplus-sigasia16-preprint.pdf)
separates RAW registration from robust merging;
[Wronski et al. (2019), section 4](https://arxiv.org/html/1905.03277v2)
motivates continuous refinement for sensor-sample reconstruction. The solver and
fixtures are independently written. No third-party HDR+ implementation, private
report, vendor tuning table or learned weights were used.

The HDR+ dataset, Night Sight, bracketing and Indigo primary publications were
also read for the continuing campaign; they do not establish implemented ZSL,
bracketing, joint SR or computational DNG in this increment. Registration remains
CPU-resident and locally translational. Geometry scores are conditional evidence,
not a complete visibility/occlusion model. Physical RAW/OEM/IMU behavior, actual
AHardwareBuffer traffic, mobile GPU performance, thermals and image quality remain
**UNVERIFIED — DEVICE ONLY**.
