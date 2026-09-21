# Plan 0003: Temporal RAW reconstruction and Android capture

Status: Active
Related: ADR-0001 through ADR-0005; Plans 0001 and 0002
Current step: Pass latest-SHA Android/native CI, then perform the final independent review.

## Scope and authority

Implement one clean-room temporal RAW path. Keep the deterministic C++ oracle
and existing Vulkan backend. Kotlin and Compose own Android UI and Camera2.
Do not create a second backend solely to add Rust.

This work adds the required P0 slice: typed burst identity, multi-source lineage,
physical bindings, canonical temporal stages, and a versioned plan and trace.
It does not complete the general graph compiler or Plans 0001 and 0002.

## Implemented increments

- Start from freshly queried main `28535ff41ead853a219ad4003a1300a7dbf6ac07`.
  The new branch is `feat/temporal-raw-android`, draft PR 17. The two unmerged
  implementation commits from PR 16 are reused with their history intact.
- Deterministic selection, gain-aware radiometry, multiscale/local registration,
  same-channel CFA sampling, Tukey fusion, conditional uncertainty, and scene
  reconstruction are implemented. Each stage retains separate confidence terms.
- Vulkan lowers sampling, weights, persistent accumulation, and regional support
  to FP32 compute. Normalization and registration remain CPU stages. No FP16
  arithmetic or storage is used. One current source buffer is reused.
- CapturePlan separates observations, intent, and capabilities. Constant exposure
  uses measured sensor results. ISO is not reclassified as physical gain. Gyro
  constrains capture only when its clock is comparable and its samples are fresh.
- Android contains Camera2 preview and RAW capture, timestamp pairing, borrowed
  direct-buffer JNI, fixture replay, Material 3 controls, and transactional
  MediaStore output. Cancellation starts a new camera/reader generation. A reader
  cannot close while native execution retains an Image lease. Preview is separate
  from reconstruction. The RAW leases are released before JPEG work.
- Native code maps crop phase and sensor-layout metadata. JVM tests cover lifetime,
  pairing, cancellation, and gyro admission. Instrumentation tests exercise JNI,
  all CFA crop phases, real replay rendering, UI transitions, and MediaStore rollback.

## Remaining acceptance work

1. Pass assembly, lint, unit tests, native loading, and fixture UI flow on the
   current Android increment. Earlier APK assembly is not current-code evidence.
2. Inspect current-SHA Actions jobs, steps, logs, artifacts, APK, and emulator replay.
3. Review the full diff independently and close any semantic or lifetime defects.

Reference gates cover validation, lineage, N=1 image equivalence, all Bayer
layouts, odd borders, sub-black, radiometric variance scaling, translations,
motion rejection, clipping, replay, and Monte Carlo calibration. A band-limited
sub-pixel fixture now gates displacement error and usable alignment confidence.
An eight-frame full-pipeline fixture gates mean drift, MSE reduction, conditional
variance calibration, and effective support. Fixed-weight and conditional variance
evidence do not establish unconditional calibration of adaptive weights.

## Verification ledger

| Requirement | Environment | Evidence | Status | Latest SHA |
| --- | --- | --- | --- | --- |
| Fresh unchanged baseline | GitHub Ubuntu 24.04, lavapipe | Run 35520430992, fresh job 106245198495; native 5/5, no-Vulkan 4/4, Ultra HDR 4/4; complete log inspected | VERIFIED | 28535ff41ead853a219ad4003a1300a7dbf6ac07 |
| Baseline reproduced locally | GCC strict, SwiftShader | Explicit ICD and required-Vulkan run 5/5; no-Vulkan 4/4, no execution skip | VERIFIED | 28535ff41ead853a219ad4003a1300a7dbf6ac07 |
| Capture policy and cancellation | Local strict builds and Actions run 35573951380 | no-Vulkan 6/6; real Vulkan 8/8; native/sanitizer jobs succeeded | VERIFIED | 8c739bb5abe9163b2c785149980535102ba90394 |
| Temporal reference, CFA transport, and Vulkan differential | Local GCC strict, SwiftShader | no-Vulkan 7/7; Vulkan 9/9; band-limited sub-pixel error <=0.25 px with usable confidence; 8-frame MSE ratio 0.133 and conditional variance ratio 0.967; 478381 differential values with observed max error 0; 320 progressive dispatches | VERIFIED | 0a7d21d18841c0499947a51e7dd2096842cbf32c |
| Host/software execution benchmark | Local GCC strict, SwiftShader | 257x193x4, 3 repeats; reference median about 1.21 s; Vulkan-path median about 1.17 s; Vulkan working-set bound 7,968,416 bytes; concurrent source frames 1. This is not mobile-performance evidence. | VERIFIED | 0a7d21d18841c0499947a51e7dd2096842cbf32c |
| Initial Android assembly and parser tests | Actions run 35576794534, job 106260342510 | Both ABIs and APK assembled; parser unit tests passed; lint reported 16 errors; full log/report inspected | PARTIALLY VERIFIED | d106516924160ff714700d17b4599642ccd77ade |
| Current Camera2/UI/JNI integration | Actions required | Camera2 RAW capture, metadata transport, resource lifetime, UI, updated SDK/dependencies, and tests are committed; latest-SHA assembly/lint/emulator replay remain required | UNVERIFIED | 0a7d21d18841c0499947a51e7dd2096842cbf32c |
| Real RAW, metadata, IMU, OEM, external import | Physical Android device | No device connected | UNVERIFIED | Not applicable |
| Mobile latency, thermals, energy, memory traffic | Physical Android device | No device connected | UNVERIFIED | Not applicable |

## Environment and clean-room record

The container cannot resolve external hosts and has no Android SDK. Chromium's
SwiftShader provides a real local Vulkan ICD. Actions supplies lavapipe and the
Android toolchain. Reproduction artifacts supply fixed sources and Git bundles;
artifact digests and internal checksums are checked.

The initial Android lint failure exposed old SDK/dependency pins, missing explicit
backup exclusions, and unused UI resources. The active increment updates compatible
fixed toolchain versions and connects the controls. It does not suppress these checks.

The earlier contract iteration reported directory-only inspection of the uploaded
archive. This implementation iteration has not opened it. No Apple private code,
parameters, tuning, topology, or weights are implementation inputs. No third-party
HDR+ implementation is copied. The public HDR+ project and paper abstract were read;
no full-paper review is claimed where the PDF was not retrieved.

The Android adapter follows these public definitions:

- https://developer.android.com/reference/android/hardware/camera2/CameraCharacteristics
- https://developer.android.com/reference/android/hardware/camera2/CaptureResult
- https://developer.android.com/build/releases/agp-9-4-0-release-notes
- https://developer.android.com/build/migrate-to-built-in-kotlin
- https://gradle.org/release-checksums/

AOSP DngCreator JNI was inspected only to cross-check documented RAW dimensions and
LSC coordinates. Its header specifies Apache-2.0. No implementation was ported.
Unknown resampled sensor modes fail explicitly. The dynamic Camera2 color transform
is labelled an estimate, not a DNG-profile calibration. External-memory capture import,
learned stages, and adaptive bracketing are not implemented. No zero-copy claim is made.
