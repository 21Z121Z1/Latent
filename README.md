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

It intentionally does **not** claim that AHardwareBuffer import, burst reconstruction, DNG calibration interpolation, production Vulkan kernels, or Ultra HDR encoding are implemented yet.

## Build

```bash
cmake -S . -B build -DLATENT_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

See [`docs/architecture.md`](docs/architecture.md) for invariants and staged implementation boundaries.
