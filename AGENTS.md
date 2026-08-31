# Latent agent operating contract

Latent is not a collection of camera filters. Treat it as a **typed semantic imaging system whose implementations are compiled/lowered into execution backends and proven against executable reference semantics**.

This file is the fastest operational entry point for an agent. Keep it concise. Deeper design belongs in `docs/architecture.md`; live status and branch topology belong in `docs/roadmap.md`; validation belongs in `docs/verification.md`; durable architectural decisions belong in `docs/decisions/`.

## 30-second orientation

The semantic data flow is:

```text
capture / recorded input
        |
        v
RawFrame / future RawBurst            sensor-referred
        |
        | reconstruction semantics
        v
SceneFrame                            scene-referred, linear AP1/D60,
        |                             unbounded, negative-preserving
        +--> scene analysis
        |
        +--> independent SDR render --> RenderedFrame (Rec.709/sRGB)
        |
        +--> independent HDR render --> RenderedFrame (BT.2020/PQ)
                                      |
                                      v
                              codec staging / export
                                      |
                                      v
                              JPEG/HEIF/AVIF, etc.
```

The realization model is:

```text
semantic contracts
      -> semantic graph
      -> graph validation / compilation
      -> execution plan
      -> reference or production executor
      -> evidence (goldens, differential tests, integration tests, trace)
```

Only the first, parts of the second, and the current reference/Vulkan execution slices are implemented today. `GraphCompiler`, a first-class `ExecutionPlan`, `RawBurst`, and structured execution traces are target architecture, not existing APIs.

## Authority and truth hierarchy

When trying to understand what is true, use this order:

1. **Semantic types, validation code, and tests** are the current executable contract.
2. **`docs/architecture.md`** is the intended system boundary and explains how current pieces fit together.
3. **`docs/roadmap.md`** says what is merged, stacked, transitional, or planned.
4. **Accepted ADRs in `docs/decisions/`** preserve decisions that must survive PR history.
5. PR descriptions and old branches are historical evidence, not architectural authority.

If code/tests and architecture disagree, do not silently choose one. Determine whether the implementation or the design is stale and make the discrepancy explicit in the change.

## Read only what the task needs

Start from the semantic transition involved in the task, then read outward.

| Task | Primary files | Then inspect |
| --- | --- | --- |
| Sensor/capture semantics | `include/latent/imaging/RawFrame.h`, `include/latent/imaging/Types.h` | `reference/`, ingress tests, capture roadmap |
| RAW reconstruction | `include/latent/reference/ReferenceReconstruct.h` | `DngColor`, `Demosaic`, `SensorLinearOps`, noise tests |
| Scene semantics | `include/latent/imaging/SceneFrame.h` | `Types.h`, reference reconstruction, scene-analysis tests |
| Graph/compiler work | `include/latent/graph/ImagingIR.h`, `src/graph/ImagingIR.cpp` | architecture control-plane section, ADRs |
| Vulkan execution | `include/latent/vulkan/DeviceCaps.h`, `ComputeRunner.h`, kernel wrappers | shaders, differential tests, `docs/verification.md` |
| Rendering | `RenderedFrame.h`, `render/SceneAnalysis.h`, `render/ReferenceRenderer.h` | output encoding, render tests |
| Codec/export | `codec/UltraHdrStaging.h`, `codec/UltraHdrEncoder.h` | codec tests, external dependency boundary |
| Build/CI | `CMakeLists.txt`, `.github/workflows/ci.yml` | `docs/verification.md` |

Do not scan every source file by default. The contracts above are the high-information navigation nodes.

## Non-negotiable invariants

- Sensor, scene, display, and encoded/container concerns are distinct domains. Crossing a domain boundary must be explicit.
- `SceneFrame` is the scene master: linear ACEScg/AP1 with D60 semantics, unbounded, and allowed to contain negative values.
- `sceneScaleEV` is a coordinate/representation scale. It is not render exposure, display white, middle gray, or capture exposure.
- `renderExposureEV` belongs only to rendering intent and must never be folded back into RAW normalization or scene coordinates.
- Reconstruction must not silently clip sub-black values, highlights above nominal white, negative scene coordinates, gamut excursions, or transfer-function inputs.
- Camera metadata carries source/validity/confidence. Prefer explicit provenance over an unqualified scalar.
- The deterministic FP32 reference path is the executable semantic oracle. Production Vulkan work must be differentially testable against equivalent reference semantics.
- Vulkan capabilities select lowerings and fast paths; they do not redefine imaging semantics. Vulkan 1.1 compute is the production baseline; later features are optional accelerators.
- Successful AHardwareBuffer import is not proof of zero-copy. Distinguish allocation sharing, import success, and measured memory traffic.
- Codec standards behavior is a leaf integration concern. Gain-map math/container semantics stay in standards-oriented codec libraries unless an ADR explicitly changes that boundary.
- Third-party dependencies must not contaminate the deterministic reference core with incompatible compiler/FP policy.

## Current transitional structures: do not mistake them for the final architecture

Several APIs are intentionally incomplete seeds:

- `ImagingGraph` is a prototype semantic IR. Its operation traits (`access`, `pure`, `canFuse`, `changesReferenceDomain`) are caller-provided today; target architecture should derive these from a canonical operation schema so callers cannot create self-contradictory nodes.
- `OperationKind::GainMapEncode` predates the explicit codec-boundary design. Do not use it as justification to move Ultra HDR gain-map semantics into the imaging core. Reconcile this taxonomy when the graph/compiler is promoted.
- `ImagingBackend` currently exposes only `reconstructSingleRaw()`. Treat it as a transitional adapter, not the future universal backend interface.
- `ComputeRunner` is a deliberately synchronous one-queue correctness harness. It is not the final frame-graph scheduler, allocator, or multi-frame executor.
- `SceneFrame`, `RenderedFrame`, and `ImageType` currently repeat some semantic fields. Avoid adding new duplicated semantic state; the roadmap calls for a tighter single-source-of-truth model.

## Change protocol

For any non-trivial change:

1. Identify the semantic state transition being changed: sensor -> scene, scene -> display, display -> codec, or execution-only lowering.
2. Locate the authoritative type/invariant and the reference behavior before editing production code.
3. If adding a semantic operation, define its schema and reference semantics before or together with a production lowering.
4. Keep policy separate from mechanism: capture/reconstruction/render/codec intent must not leak into unrelated layers.
5. Add the narrowest regression or golden test that fails for the old behavior and proves the new contract.
6. For Vulkan/optimized implementations, add or extend differential evidence rather than replacing semantic tests with device-specific expectations.
7. Update `docs/roadmap.md` when maturity/status changes. Add or supersede an ADR when changing a durable boundary, invariant, precision rule, dependency rule, or ownership rule.
8. Run the relevant validation ladder from `docs/verification.md`; inspect failures rather than weakening gates.
9. Re-read the final diff for semantic drift, duplicate truth, hidden clipping, accidental backend coupling, and claims not supported by tests.

## Design preference for future APIs

Prefer structures that reduce the number of facts an agent must hold simultaneously:

- explicit sum/enumeration types over magic values;
- canonical constructors/schema registries over free-form combinations;
- immutable semantic values with explicit transitions over mutation across domains;
- generated/derived execution traits over duplicated caller annotations;
- stable IDs and machine-readable diagnostics over prose-only logs;
- one canonical status/decision source with links, not copied descriptions;
- plans/traces that explain *why* a lowering was selected, not only what ran.

The architectural north star is: **semantics say what the image means; the compiler says how that meaning becomes work; executors perform the work; evidence proves the lowering preserved the meaning.**
