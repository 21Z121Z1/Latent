# Latent

Latent is an experimental scene-referred computational photography core for Android. The project is built around semantic imaging types, a backend-independent imaging graph, a deterministic FP32 reference implementation, and a Vulkan production path.

The repository is under active construction; APIs are not yet stable.

## Current foundation

The first implementation slice establishes:

- explicit sensor / scene / display reference-domain semantics;
- `RawFrame`, `SceneFrame`, and `RenderedFrame` data contracts;
- metadata provenance/confidence fields instead of unqualified Camera2 values;
- a backend-independent imaging IR with validation and conservative fusion planning;
- a deterministic FP32 single-RAW reference reconstruction path that preserves negative and >1 scene values;
- a Vulkan capability model where Vulkan 1.1 compute is the baseline and AHardwareBuffer/FP16 features are optional fast paths;
- differential-friendly CTest coverage and CI.

It intentionally does **not** claim that AHardwareBuffer import, burst reconstruction, production Vulkan rendering, Android capture integration, or production HEIF/AVIF output are implemented yet.

## Color science and demosaic

The second slice adds a tested FP32 color pipeline:

- the DNG Chapter 6 dual-illuminant camera color model (mired interpolation,
  camera-neutral <-> WB-xy, ForwardMatrix and inverse-color-matrix paths to
  XYZ D50), differential-tested against BSD-licensed reference
  implementations and official ACES matrices;
- Bradford chromatic adaptation and Robertson (1968) correlated-color
  temperature per the Adobe DNG SDK;
- `XYZ D50 -> linear AP1/ACEScg` so `SceneFrame` coordinates come from real
  colorimetry instead of caller-supplied matrices;
- Malvar-He-Cutler (2004) gradient-corrected demosaic with golden-vector and
  property tests, alongside the retained box-average baseline.

## Sensor reconstruction ops

The third slice adds the pre-demosaic stages and uncertainty tracking:

- map-driven defect correction plus conservative noise-aware defect
  detection;
- Android-convention lens-shading maps applied between black-level
  subtraction and demosaic;
- NOISE_PROFILE propagation through every stage, exposed as a lazy
  per-pixel sigma query on `SceneFrame` and verified by Monte Carlo
  simulation.

## Vulkan ingress adapter

The fourth slice prepares GPU execution with a strict testability split:

- MIPI/Android RAW10/RAW12 unpacking into a canonical uint16 buffer, with
  byte-exact golden vectors, round trips across partial groups, and stride
  handling;
- an ingress decision table mapping device capabilities onto direct-import
  candidates vs the portable-copy baseline (camera RAW never assumes
  zero-copy);
- a volk-based runtime capability adapter that fills `DeviceCaps` from real
  Vulkan queries and degrades gracefully without a GPU; CI exercises it
  against SwiftShader.

## First differential Vulkan kernel

The fifth slice executes real GPU work with a closed verification loop:

- a fused pointwise kernel (black level -> normalize -> lens shading ->
  white balance -> FP16 Bayer) compiled from GLSL at build time;
- a minimal compute executor that CI exercises against Mesa lavapipe;
- a differential test proving every output sample matches the FP32 reference
  within one fp16 ulp, with bit-exactness reported per case.

## Fused demosaic and scene-color kernel

The sixth slice extends GPU execution to the RAW-to-scene boundary:

- a fused neighborhood kernel that consumes the FP16 Bayer output of the
  preprocess kernel, performs either deterministic box-average or
  Malvar-He-Cutler demosaic, applies the camera-to-scene color matrix in
  FP32, scales the explicit scene coordinate, and stores packed RGBA16F;
- FP32 accumulation for all interpolation and color cancellation-sensitive
  math while retaining the compact FP16 storage contract;
- preservation of unbounded and negative scene coordinates before any
  display-referred rendering decision;
- a six-case differential suite covering all CFA patterns, odd and extreme
  aspect-ratio extents, both demosaic methods, and lens shading on/off,
  with zero-NaN, sign-preservation, median/p99 relative-error, and scaled
  absolute-error gates.

## Independent SDR/HDR reference rendering

The next slice establishes the output-side semantic boundary before codec
integration:

- scene luminance analysis is invariant to the purely representational
  `sceneScaleEV` coordinate choice;
- `renderExposureEV` is applied only after undoing `sceneScaleEV`, so capture
  radiometry and final picture brightness remain separate controls;
- one `SceneFrame` is rendered independently to a Rec.709/sRGB SDR rendition
  and a BT.2020/PQ HDR rendition with explicit nominal-white, target-peak, and
  headroom metadata;
- the official-ACES-derived scalar tone-scale primitive is applied to
  luminance, while output-primary conversion and a deterministic neutral-axis
  gamut compression are explicit rendering operations;
- encoded display pixels are carried by `RenderedFrame`, while the original
  scene master remains untouched even when it contains negative or >1 values;
- tests cover scene-scale invariance, independent SDR/HDR intent, exposure
  separation, gamut/negative handling, provenance, and invalid inputs.

This renderer is a reference contract, not a claim to implement the complete
ACES 2 Output Transform. Production Vulkan rendering remains a separate
milestone.

## Ultra HDR codec staging and integration

The codec slice keeps standards/container behavior outside the imaging core:

- `latent::codec` deterministically stages Rec.709/sRGB SDR renditions as
  aligned RGBA8888 and BT.2020/PQ HDR renditions as aligned RGBA1010102;
- staging validates display-domain semantics, common source/extent, encoded
  sample range, D65 output white, and Ultra HDR target-peak bounds;
- `LATENT_ENABLE_ULTRAHDR=ON` enables an adapter to an externally built
  libultrahdr 2.x installation; libultrahdr is deliberately not brought into
  Latent with `FetchContent`, so its directory-wide fast-math build settings
  cannot contaminate the deterministic FP32 reference core;
- the adapter supplies explicit `UHDR_SDR_IMG` and `UHDR_HDR_IMG` inputs and
  delegates gain-map math, ISO 21496 metadata, and JPEG/HEIF/AVIF container
  behavior to libultrahdr rather than reproducing the standard locally;
- CI independently builds libultrahdr v2.0.2 and verifies a real Ultra HDR
  JPEG encode from Latent's `SceneFrame -> SDR/HDR -> staging` path.

HEIF/AVIF enum routing is present in the adapter, but the current CI validation
uses the dependency-light JPEG path; production HEIF/AVIF validation remains a
separate libheif/Android integration milestone.

## License

Apache-2.0. See [`LICENSE`](LICENSE).

## Build

```bash
cmake -S . -B build -DLATENT_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

To enable an externally installed libultrahdr 2.x codec:

```bash
cmake -S . -B build-uhdr \
  -DLATENT_BUILD_TESTS=ON \
  -DLATENT_ENABLE_ULTRAHDR=ON \
  -DLATENT_ULTRAHDR_ROOT=/path/to/libultrahdr/install
cmake --build build-uhdr --parallel
ctest --test-dir build-uhdr --output-on-failure
```

See [`docs/architecture.md`](docs/architecture.md) for invariants and staged implementation boundaries.
