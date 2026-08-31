# Latent

Latent is an experimental scene-referred computational-photography core for Android. Its architectural center is not Vulkan or a sequence of filters: it is a **typed semantic imaging model** backed by deterministic reference semantics, a backend-independent graph/compiler direction, production Vulkan lowerings, and differential verification.

APIs are still evolving. The current implementation already covers a substantial single-RAW path and the first output/codec slices; Android capture, burst reconstruction, a complete graph compiler/execution plan, and production Vulkan rendering are still in progress or planned.

## System at a glance

```text
capture / recorded input
        |
        v
RawFrame                         sensor-referred
        |
        | normalize / correct / demosaic / color
        v
SceneFrame                       scene-referred, linear AP1/D60,
        |                        unbounded, negative-preserving
        +--> scene analysis
        |
        +--> SDR render --------> RenderedFrame (Rec.709/sRGB)
        |
        +--> HDR render --------> RenderedFrame (BT.2020/PQ)
                                     |
                                     v
                           Ultra HDR staging / codec
```

The same semantic work can have multiple realizations:

```text
semantic contract -> reference FP32 implementation -> graph/lowering -> Vulkan or external backend
                                                     |
                                                     v
                                      golden / differential / integration evidence
```

The rule is simple: **semantics define what an image means; backends are only ways to realize those semantics.**

## Current capability

The latest stacked implementation contains:

- explicit sensor / scene / display reference-domain types and validation;
- `RawFrame`, `SceneFrame`, and `RenderedFrame` contracts with provenance/confidence-aware metadata;
- deterministic FP32 single-RAW reconstruction with negative and >1 scene values preserved;
- DNG Chapter 6 dual-illuminant color science, Bradford adaptation, Robertson CCT, and camera -> XYZ D50 -> ACEScg/AP1 D60 transforms;
- Malvar-He-Cutler demosaic plus a retained deterministic box baseline;
- defect correction, Android-convention lens shading, and lazy propagated-noise semantics validated by Monte Carlo tests;
- RAW10/RAW12 canonical unpacking plus Vulkan ingress/capability planning;
- differential Vulkan K1a sensor preprocessing and K1b demosaic/color kernels;
- scene analysis and independent deterministic SDR/HDR reference rendering;
- explicit SDR/HDR rendition staging plus optional external libultrahdr integration, with real JPEG Ultra HDR encode/probe coverage in CI.

Important boundaries remain deliberate: AHardwareBuffer import execution is not yet an Android-device implementation; `ComputeRunner` is still a synchronous correctness harness; rendering has no production Vulkan lowering yet; HEIF/AVIF routing is not yet device/integration validated; burst reconstruction is not implemented.

## Repository map

- `include/latent/imaging/` — semantic image/data contracts and color/noise primitives.
- `include/latent/reference/`, `src/reference/` — deterministic executable reference semantics.
- `include/latent/graph/`, `src/graph/` — current semantic IR seed; not yet the complete compiler/control plane.
- `include/latent/vulkan/`, `src/vulkan/`, `shaders/` — capabilities, ingress decisions, execution harness, and production kernels.
- `include/latent/render/`, `src/render/` — scene analysis, output encoding, and reference rendering.
- `include/latent/codec/`, `src/codec/` — explicit output-rendition staging and optional codec adapters.
- `tests/` — semantic, golden, differential, lifetime, render, and codec evidence.
- `docs/architecture.md` — authoritative system model and invariants.
- `docs/roadmap.md` — branch topology, maturity matrix, and plan of record.
- `docs/verification.md` — validation/evidence matrix and commands.
- `docs/decisions/` — durable architectural decisions.
- `AGENTS.md` — compact operating contract for coding agents.

## Build and test

Core/default configuration:

```bash
cmake -S . -B build \
  -DLATENT_BUILD_TESTS=ON \
  -DLATENT_STRICT_WARNINGS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure --verbose
```

Portable configuration without the Vulkan runtime:

```bash
cmake -S . -B build-novk \
  -DLATENT_BUILD_TESTS=ON \
  -DLATENT_ENABLE_VULKAN_RUNTIME=OFF
cmake --build build-novk --parallel
ctest --test-dir build-novk --output-on-failure
```

Optional externally installed libultrahdr 2.x integration:

```bash
cmake -S . -B build-uhdr \
  -DLATENT_BUILD_TESTS=ON \
  -DLATENT_ENABLE_VULKAN_RUNTIME=OFF \
  -DLATENT_ENABLE_ULTRAHDR=ON \
  -DLATENT_ULTRAHDR_ROOT=/path/to/libultrahdr/install
cmake --build build-uhdr --parallel
ctest --test-dir build-uhdr --output-on-failure --verbose
```

## How to read the project

For implementation work, start with `AGENTS.md`, then read only the relevant semantic contract and its tests. For system design, read `docs/architecture.md`. For what is merged/stacked/planned and what should be built next, read `docs/roadmap.md`. For evidence requirements, use `docs/verification.md`.

Apache-2.0. See [`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
