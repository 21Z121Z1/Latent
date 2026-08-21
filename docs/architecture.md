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

## Near-term milestones

1. Android NDK capture contracts and metadata decoder tests using recorded metadata fixtures.
2. DNG-style color-calibration model and tested camera -> XYZ D50 -> D60/AP1 transform.
3. Reference defect correction, LSC, and edge-aware demosaic.
4. Vulkan loader/device-capability adapter plus canonical RAW ingress buffer.
5. First differential Vulkan kernels: black/normalize/LSC/WB + demosaic/color transform.
6. Scene analysis and independent SDR/HDR render branches.
7. libultrahdr integration from explicit SDR/HDR renditions.
8. RawBurst temporal model, alignment, robust merge, and memory-lifetime analysis.
