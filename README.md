# Latent

Latent is an experimental scene-referred computational-photography core for Android. Its architectural center is not Vulkan or a sequence of filters: it is a **typed semantic imaging model** backed by deterministic reference semantics, a backend-independent graph/compiler direction, production Vulkan lowerings, and differential verification.

APIs are still evolving. The merged implementation already covers a substantial single-RAW path plus independent SDR/HDR reference rendering and the first output/codec integration. Android capture, burst reconstruction, a complete graph compiler/execution plan, and production Vulkan rendering remain planned or incomplete.

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
semantic contract -> reference FP32 implementation -> graph/compiler -> execution plan
                                                                |
                                                    +-----------+-----------+
                                                    v                       v
                                               reference               Vulkan/external
                                                    \                       /
                                                     +---- evidence/trace --+
```

The control plane also separates four kinds of input that must not masquerade as one another:

```text
semantic rules    observations/evidence    image intent/policy    execution capabilities
       \                 |                       |                       /
        +----------------+--------> compiler <---+----------------------+
                                      |
                                      v
                                ExecutionPlan
```

The rule is: **semantics define what an image means; observations say what was learned; intent says what is wanted or delegated; capabilities say what can run; backends only realize an explicit plan.**

## Current capability

The merged implementation includes:

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

Important boundaries remain deliberate: AHardwareBuffer import execution is not yet an Android-device implementation; `ComputeRunner` is still a synchronous correctness harness; rendering has no production Vulkan lowering yet; HEIF/AVIF routing is not yet fully integration/device validated; burst reconstruction is not implemented; the complete graph compiler, authority-separated contexts, `ExecutionPlan`, and `ExecutionTrace` are target architecture rather than current APIs.

For the exact current PR/head/review/CI state, query GitHub. README intentionally does not mirror volatile delivery snapshots.

## Repository map

- `include/latent/imaging/` — semantic image/data contracts and color/noise primitives.
- `include/latent/reference/`, `src/reference/` — deterministic executable reference semantics.
- `include/latent/graph/`, `src/graph/` — current semantic IR seed; not yet the complete compiler/control plane.
- `include/latent/vulkan/`, `src/vulkan/`, `shaders/` — capabilities, ingress decisions, execution harness, and production kernels.
- `include/latent/render/`, `src/render/` — scene analysis, output encoding, and reference rendering.
- `include/latent/codec/`, `src/codec/` — explicit output-rendition staging and optional codec adapters.
- `tests/` — semantic, golden, differential, lifetime, render, and codec evidence.
- `docs/architecture.md` — stable system model and invariants.
- `docs/roadmap.md` — capability maturity, dependencies, and sequencing.
- `docs/verification.md` — validation/evidence matrix and commands.
- `docs/decisions/` — durable architectural decisions and supersession history.
- `docs/plans/` — persistent multi-step implementation plans and acceptance criteria.
- `AGENTS.md` — compact operating/router contract for coding agents.

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

For implementation work, start with `AGENTS.md`, then read only the relevant semantic contract and its tests. For stable system design, read `docs/architecture.md` and applicable ADRs. For capability maturity and what should be built next, read `docs/roadmap.md`. For an active multi-step implementation, use `docs/plans/`. For evidence requirements, use `docs/verification.md`. For live delivery state, query GitHub directly.

Apache-2.0. See [`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md`).
