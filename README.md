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

It intentionally does **not** claim that AHardwareBuffer import, burst reconstruction, production Vulkan kernels, or Ultra HDR encoding are implemented yet.

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

## License

Apache-2.0. See [`LICENSE`](LICENSE).

## Build

```bash
cmake -S . -B build -DLATENT_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

See [`docs/architecture.md`](docs/architecture.md) for invariants and staged implementation boundaries.
