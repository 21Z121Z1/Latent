# Architecture foundation

Latent's stable architectural boundary is **scene semantics first, graph compilation second, Vulkan execution third**.

## Invariants implemented in the first slice

1. `SceneFrame` is the central computational image object. It is scene-referred, linear, unbounded, and explicitly permits negative values.
2. `sceneScaleEV` is metadata. It is not the same thing as display white, middle gray, or render exposure.
3. Sensor, scene, and display reference domains are explicit types. A scene-referred image using an sRGB/PQ/HLG transfer is rejected by graph type validation.
4. Camera metadata is represented as value + source + validity + confidence. Optical-black estimates outrank dynamic black metadata; dynamic black outranks static black. Dynamic white outranks static white.
5. Sensor normalization preserves negative values after black subtraction and values above 1.0. Clipping is not part of reconstruction.
6. Logical imaging operations are independent from physical dispatches. The graph can group safe point/neighborhood operations and stops conservative fusion at reductions, temporal operations, and external interfaces.
7. Vulkan 1.1 compute is the production capability baseline. AHardwareBuffer import, FP16, timeline semaphores, synchronization2, subgroup operations, and other later features are optional acceleration capabilities.
8. The reference backend is FP32 and deterministic. Production Vulkan kernels must eventually differential-test against semantic-equivalent reference operations.

## Current single-RAW reference path

```text
RawFrame
  -> metadata validation
  -> optical/dynamic/static black selection
  -> dynamic/static white selection
  -> sensor normalization (FP32, no clamp)
  -> white-balance gains on CFA samples
  -> deterministic baseline demosaic
  -> caller-supplied camera-RGB -> ACEScg/AP1 D60 matrix (FP32)
  -> apply explicit `2^sceneScaleEV` coordinate scale
  -> SceneFrame (linear AP1/D60 semantics, explicit sceneScaleEV)
```

The current demosaic is intentionally a deterministic baseline, not the intended final image-quality algorithm. DNG dual-illuminant interpolation, lens shading, defect correction, noise-aware reconstruction, chromatic aberration correction, denoise, SDR/HDR rendering, gain-map encoding, burst alignment/merge, and production Vulkan kernels remain separate milestones.

## Vulkan ingress boundary

The future Android/Vulkan adapter must expose two paths:

```text
FAST PATH
AImage -> AHardwareBuffer -> capability/format probe -> Vulkan external-memory import

PORTABLE PATH
AImage planes -> respect packing/stride -> one unpack/copy -> Vulkan-owned canonical RAW storage
```

Direct AHardwareBuffer import is never a correctness prerequisite. Actual zero-copy claims require device profiling of memory traffic, not merely successful handle import.

## Precision policy

- Reference reconstruction: FP32.
- RAW storage: integer packed/native until ingress conversion.
- Production scene storage target: RGBA16F where validated.
- CCM, burst accumulation, motion/statistics, and other cancellation/reduction-sensitive math: FP32 compute/accumulation.
- FP16 storage and FP16 arithmetic are queried separately.

## Color science (implemented)

`imaging/ColorScience` and `reference/DngColor` implement the DNG Chapter 6
camera color model with FP32 reference semantics:

- SMPTE RP 177 derivation of RGB-to-XYZ matrices from primary chromaticities;
  the derived AP0/AP1 chain is differential-tested against the official
  ACES AP0<->AP1 transform matrices.
- Bradford chromatic adaptation, tested against published D65->D50 values.
- Robertson (1968) xy->CCT using the Wyszecki & Stiles table (with the
  325-mired correction) exactly as the Adobe DNG SDK does; McCamy's
  approximation is provided as a cross-check only.
- Dual-illuminant camera profiles: mired-domain interpolation with clamping,
  camera-neutral <-> WB-xy conversion (iterative per the DNG spec), and two
  camera -> XYZ D50 paths: the preferred ForwardMatrix path
  (`FM * D * inv(AB*CC)`) and the inverse-color-matrix fallback with
  explicit chromatic adaptation.
- `XYZ D50 -> linear AP1 (ACEScg)` via Bradford adaptation to the ACES white.
- Golden-vector tests reproduce the BSD-licensed colour-hdri DNG reference
  implementation results for a Canon 5D Mark II profile.

## Demosaic (implemented)

`reference/Demosaic` provides:

- `MalvarHeCutler2004`: gradient-corrected linear demosaic with the exact
  5x5 filters from the 2004 paper, replicate-clamped borders. Golden vectors
  are generated from the BSD-licensed colour-demosaicing reference.
- `BaselineBoxAverage`: the original deterministic box-average baseline,
  retained for differential comparison.

The default reconstruction path is normalize -> MHC demosaic -> DNG color
model -> SceneFrame (linear AP1, unbounded, negative-preserving).

## Sensor reconstruction ops (implemented)

`reference/SensorLinearOps` adds the pre-demosaic stages of the K1 chain:

- **Defect correction**: map-driven replacement with the median of
  same-CFA-channel neighbors (diagonal distance one, axial distance two);
  deterministic, negative-preserving, non-defect samples untouched.
- **Conservative defect detection**: a pixel is flagged only when its delta
  to the same-channel neighbor median exceeds both a noise-aware threshold
  (`sigmaMultiplier * sigma`, using the propagated noise model when
  available) and an absolute floor. Detection is advisory; correction stays
  map-driven.
- **Lens shading**: Android-convention maps (4xNxM gains [R, Geven, Godd, B],
  all >= 1.0) applied by bilinear grid interpolation after black-level
  subtraction and before demosaic, per the DNG stage-opcode ordering.

## Noise propagation (implemented)

`imaging/Noise` + `reference/NoisePropagation` propagate NOISE_PROFILE
metadata through the whole reference pipeline:

- exact closed-form transform from raw-code domain to the normalized
  sensor-linear domain selected by black/white levels;
- per-tap variance composition through LSC gains, WB gains, and the exact
  demosaic kernel weights of both methods;
- `SceneFrame::propagatedNoise` stores a lazy record;
  `reference::propagatedSigma` evaluates the scene-linear standard deviation
  at any pixel/channel in O(taps) without materializing full-res maps.

A Monte Carlo test (256 realizations, fixed seed, shot+read noise, WB gains,
lens shading, color matrix, scene scale) verifies predictions against
empirical reconstruction spread: median error ~3%, p95 ~9%, and
reconstruction unbiasedness within sampling noise.

## Vulkan ingress adapter (implemented)

`imaging/RawPacking` + `vulkan/IngressPlan` + `vulkan/VulkanRuntime`
implement milestone 4 with a strict testability split:

- **Canonical RAW ingress (pure logic, fully tested on CPU)**: MIPI/Android
  RAW10 (4-in-5 bytes) and RAW12 (2-in-3 bytes) group-layout unpacking to
  canonical little-endian uint16 samples at native depth, byte-exact golden
  vectors, pack/unpack round trips across partial groups and widths 1..9,
  row-stride padding handling, and layout validation.
- **Ingress decision table (pure logic)**: `planIngress()` maps device
  capabilities plus an AHardwareBuffer descriptor onto
  DirectImportCandidate / PortableCopy / Unsupported. Camera RAW defaults to
  the portable unpack baseline even when the AHB extension exists;
  RAW_PRIVATE is unsupported without a device profile; YUV routes through the
  external-format/YCbCr sampler path; BLOB+GPU_DATA_BUFFER imports as
  VkBuffer memory. Every decision records which of the three zero-copy
  metrics it can honestly claim.
- **Runtime capability adapter**: volk-based dynamic loading with graceful
  degradation at every step (loader -> instance -> physical device -> device).
  Real queries populate the DeviceCaps record: separate FP16 storage vs
  arithmetic, timeline semaphores, synchronization2, subgroup stage/operation
  checks via Vulkan11Properties, descriptor indexing, float controls (core in
  1.2), integer dot product / cooperative matrix extension presence, and AHB
  external memory availability. A CI job installs SwiftShader so the runtime
  smoke test exercises real driver queries; without any ICD the test skips.

The actual `vkGetAndroidHardwareBufferPropertiesANDROID` probe and import
remain Android-device milestones: this layer defines the decision recipes and
capability ground truth they will consume.

## First differential Vulkan kernel (implemented)

`shaders/sensor_preprocess.comp` + `vulkan/ComputeRunner` +
`vulkan/SensorPreprocessKernel` deliver the K1a pointwise chain as the first
production kernel with a full differential harness:

- fused black-level subtraction, white normalization, bilinear lens-shading
  gain, and white-balance gain; FP32 math throughout, IEEE binary16 storage
  via core `packHalf2x16` (no 16-bit-storage extension required);
- SPIR-V compiled at build time by a pinned FetchContent glslang;
- `imaging/Half.h` provides bit-identical RNE half conversion for CPU-side
  expectations, verified against 111 golden vectors including subnormals,
  overflow-to-infinity, and ties-to-even cases;
- differential test across four CFA patterns, odd extents, padded pair tails,
  LSC on/off, and sub-black samples: every sample within tolerance, zero
  NaNs, negative values preserved below black, and >=99.5% bit-exact
  (observed 100% on MoltenVK; drivers may break exact ties differently, which
  is precisely why the article mandates tolerance budgets);
- runs for real in CI against Mesa lavapipe and locally against MoltenVK;
  skips gracefully without any Vulkan loader.

## Fused demosaic/color Vulkan kernel (implemented)

`shaders/demosaic_color.comp` + `vulkan/DemosaicColorKernel` deliver the K1b
RAW-to-scene chain as the first production neighborhood kernel:

- consumes the canonical FP16 Bayer bit patterns produced by K1a;
- implements both reference demosaic methods with the same replicate-clamped
  borders and exact Malvar-He-Cutler coefficients as `reference/Demosaic`;
- performs interpolation and camera-to-scene matrix multiplication in FP32,
  then stores packed RGBA16F so scene values remain unbounded and may be
  negative before rendering;
- applies the caller's row-major color matrix and explicit scalar scene
  scale as separate semantic inputs;
- enables `VK_KHR_portability_subset` only when the physical device reports
  it, keeping conformant Android drivers on the normal path;
- is differentially tested against the CPU reference after both kernels have
  crossed their FP16 storage boundaries: four CFA patterns, odd/extreme
  extents, both methods, and LSC on/off. Gates require zero NaNs, sign
  preservation, median error <=0.2%, p99 <=1%, and a pixel-scale absolute
  tolerance for near-cancellation outputs.

This is still execution of a single-RAW reconstruction graph, not burst
reconstruction, denoising, rendering, or an Android capture integration.

## Near-term milestones

1. Android NDK capture contracts and metadata decoder tests using recorded metadata fixtures.
2. ~~DNG-style color-calibration model and tested camera -> XYZ D50 -> D60/AP1 transform.~~ (done)
3. ~~Reference defect correction, LSC, and noise-profile propagation.~~ (done)
4. ~~Vulkan loader/device-capability adapter plus canonical RAW ingress buffer.~~ (done)
5. ~~First differential Vulkan kernels: black/normalize/LSC/WB plus demosaic/color transform.~~ (done)
6. Scene analysis and independent SDR/HDR render branches.
7. libultrahdr integration from explicit SDR/HDR renditions.
8. RawBurst temporal model, alignment, robust merge, and memory-lifetime analysis.
