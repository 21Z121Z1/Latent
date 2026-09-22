# Plan 0003: Temporal RAW reconstruction and Android capture

Status: In review — first-generation scope; subsequent reconstruction work belongs in a stacked follow-up.
Related: ADR-0005; Plans 0001 and 0002 remain independently scoped.

## Implemented boundary

The default burst entry point compiles one typed temporal plan: reference selection,
normalization, global/tile translation, robust same-CFA fusion, and scene reconstruction.
N=1 preserves single-RAW image semantics. Exposure/gain coordinates scale variance
quadratically; ISO remains an observation, not measured effective gain. Conditional
aleatoric variance does not include adaptive-weight or geometry uncertainty.

One Vulkan backend lowers FP32 sampling, weighting, progressive accumulation, and
regional support. Normalization and registration remain on the CPU. A persistent
source buffer bounds fusion payload; no end-to-end GPU-residency or zero-copy claim.

Android owns Camera2 preview/capture, converged constant-exposure CapturePlan,
timestamp pairing, borrowed RAW16 JNI, Compose controls, and transactional JPEG output.
Cancellation starts a new camera/reader generation. Image leases outlive synchronous
JNI and are released before orientation/encoding. Unknown sensor resampling fails
explicitly. Dynamic Camera2 color is labelled an estimate, not DNG calibration.

## Final audit corrections

- Export the native policy's motion/noise/quality flags through JNI. Available gyro
  evidence and an actually applied exposure constraint are separate trace fields.
- Preserve the original nonlinear-texture fractional-displacement fixture alongside
  the newer band-limited test; neither tolerance is weakened.
- Capture the result UI inside instrumentation, before activity teardown. A cleanup
  screenshot of the launcher is not visual evidence of the camera result.
- Remove obsolete Android/lint/PR status from this plan and README.

## Verification ledger

The recovered implementation was `3d8c5d758a4151cddc298e149d985c74af4c431d`.
Its exact-head runs and downloaded artifacts were independently checked on 2026-09-22:

| Gate | Evidence on recovered head | Result |
| --- | --- | --- |
| Native strict/Vulkan/SPIR-V | Actions 35680461646, linux job 106596124264; artifact 10674932404; all steps successful, 9/9 CTest | VERIFIED |
| No-Vulkan and libultrahdr 2.0.2 | Same linux job and archived LastTest logs; 7/7 each | VERIFIED |
| Clang ASan/UBSan | Same run, job 106596124505; artifact 10674952223; 7/7 | VERIFIED |
| Android assemble/lint/JVM | Actions 35680461644, job 106596124182; artifact 10674457303; both ABIs, 8 JVM tests, no lint issues | VERIFIED |
| Installed APK/JNI/Compose/MediaStore | Same Android artifact; 7 instrumentation tests, no failures/errors/skips | VERIFIED |
| Independent local reproduction | Artifact checksums and Git bundle checked; GCC strict + explicit SwiftShader ICD, 9/9 CTest | VERIFIED |

The audit corrections require **new exact-head** native/sanitizer and Android runs.
The final review record in PR #17 binds the reviewed SHA, run/job IDs, artifact
checksums, and outcomes. A green run from the recovered head is not a pass for a
later revision. All required workflows check out the PR head rather than its merge ref.

Recovered-head quantitative evidence: 478,381 CPU/Vulkan values had observed max
error 0 (not a universal bit-exactness claim); 320 lifetime dispatches passed.
Eight-frame static MSE ratio was 0.133149, conditional variance ratio 0.967070,
and mean effective support 7.85266. Band-limited displacement error was 0.25 px;
the restored nonlinear-texture case has 0.5 px error per axis under its original
0.6 px bound. These expose first-generation limits, not general registration quality.

The archived 257x193x4, three-repeat host benchmark measured reference median
1373.86 ms and Vulkan-path median 1347.89 ms, admission bound 7,968,416 bytes,
and one concurrent source frame. This is software-runner regression evidence,
not mobile performance or a measured peak allocation/traffic count.

## Device-only and next-generation boundaries

**UNVERIFIED — DEVICE ONLY:** real RAW delivery/calibration, OEM stream combinations,
IMU clock relation, external-memory traffic, mobile GPU latency, energy, thermal
behavior, display appearance, and cross-device image quality.

Joint SR, explicit visibility/occlusion modeling, bracketed/ZSL capture,
computational DNG, and GPU-resident normalization/registration are not implemented
by this plan. They require independent quality gates, not broader claims for #17.

## Reproduction and provenance

The local container has no external DNS or Android SDK. Checked Actions artifacts
supply pinned source/dependency archives and Git history; SwiftShader supplies the
local Vulkan execution environment. Actions supplies lavapipe and Android/KVM.
No Apple archive, private parameter, kernel, topology, or weight was used during
this audit. No third-party HDR+ implementation was copied.

Camera transport follows the public CameraCharacteristics, CaptureResult, and
LensShadingMap contracts at developer.android.com. Black/noise use sensor-layout
order; white balance/shading use R, green-even, green-odd, B. Native CFA phase
mapping and independent Android fixtures test this distinction. Public research
for a subsequent algorithm revision must be read and recorded in that revision;
this delivery audit is not a claim of implementing a paper or Project Indigo.
