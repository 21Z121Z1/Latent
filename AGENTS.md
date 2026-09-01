# Latent agent operating contract

Latent is not a collection of camera filters. Treat it as a **typed semantic imaging system whose requested meaning is compiled/lowered into execution backends and proven against executable reference semantics**.

This file is the fastest operational entry point for an agent. Keep it concise. Stable design belongs in `docs/architecture.md`; capability maturity and sequencing belong in `docs/roadmap.md`; validation rules belong in `docs/verification.md`; durable decisions belong in `docs/decisions/`; multi-step implementation state belongs in `docs/plans/`. Exact open/merged PRs, branch heads, workflow results, and review state are volatile facts: query GitHub rather than trusting a copied snapshot in documentation.

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

The control inputs are not one undifferentiated config blob:

```text
semantic request/rules     observations/evidence
          \                    /
           +--> compiler <----+
          /                    \
intent / delegated policy      execution capabilities
                    |
                    v
              ExecutionPlan
```

Only the semantic contracts, prototype graph, reference paths, and current Vulkan/external execution slices are implemented today. `GraphCompiler`, a first-class `ExecutionPlan`, `RawBurst`, typed multi-source lineage, authority-separated compiler contexts, and structured execution traces are target architecture, not existing APIs.

## Route each question to the correct source of truth

There is no useful single global “truth hierarchy”; different facts have different authorities. Route the question first:

| Question | Authoritative source |
| --- | --- |
| What does an image/value mean right now? | semantic types, validation code, reference behavior, tests |
| What system boundary/invariant is intended? | `docs/architecture.md` + latest applicable accepted ADR |
| Why was a durable boundary chosen? | `docs/decisions/` |
| What capability should be built next / what is transitional? | `docs/roadmap.md` |
| What are the steps/acceptance criteria for an active multi-PR change? | `docs/plans/` |
| What PR is open/merged, what SHA is current, what CI/review state is live? | GitHub live data |
| How must a change be verified? | `docs/verification.md` + relevant tests/workflows |

PR descriptions and old branches are delivery/history evidence, not architectural authority. Never copy a volatile GitHub fact into a durable design document merely for convenience; link/query it when needed.

If code/tests, architecture, and an accepted ADR disagree, do not silently choose one. Determine which layer is stale and make the discrepancy explicit in the change.

## Classify inputs before reasoning about them

For any non-trivial decision, identify the authority class of every relevant input (ADR-0003):

- **Semantic rule** — invariant/schema defining correctness. A device or policy cannot override it.
- **Observation/evidence** — captured, calibrated, estimated, profiled, or measured fact. Preserve provenance/validity/confidence and fallback source.
- **Image intent / delegated policy** — what output is requested and what choices the caller explicitly allows Latent to make.
- **Execution capability** — what the current device/platform/backend can execute.

A capability may choose a lowering, not silently change the requested picture. A policy default may not masquerade as an observation. A heuristic may change semantics only when the request explicitly delegated that choice and the trace records it.

## Read only what the task needs

Start from the semantic transition involved in the task, then read outward.

| Task | Primary files | Then inspect |
| --- | --- | --- |
| Sensor/capture semantics | `include/latent/imaging/RawFrame.h`, `include/latent/imaging/Types.h` | `reference/`, ingress tests, capture roadmap |
| RAW reconstruction | `include/latent/reference/ReferenceReconstruct.h` | `DngColor`, `Demosaic`, `SensorLinearOps`, noise tests |
| Scene semantics | `include/latent/imaging/SceneFrame.h` | `Types.h`, reference reconstruction, scene-analysis tests |
| Graph/compiler work | `include/latent/graph/ImagingIR.h`, `src/graph/ImagingIR.cpp` | architecture control-plane section, ADRs, active plan |
| Vulkan execution | `include/latent/vulkan/DeviceCaps.h`, `ComputeRunner.h`, kernel wrappers | shaders, differential tests, `docs/verification.md` |
| Rendering | `RenderedFrame.h`, `render/SceneAnalysis.h`, `render/ReferenceRenderer.h` | output encoding, render tests |
| Codec/export | `codec/UltraHdrStaging.h`, `codec/UltraHdrEncoder.h` | codec tests, external dependency boundary |
| Build/CI | `CMakeLists.txt`, `.github/workflows/ci.yml` | `docs/verification.md`, then query latest GitHub run |

Do not scan every source file by default. The contracts above are the high-information navigation nodes.

## Non-negotiable invariants

- Sensor, scene, display, and encoded/container concerns are distinct domains. Crossing a domain boundary must be explicit.
- `SceneFrame` is the scene master: linear ACEScg/AP1 with D60 semantics, unbounded, and allowed to contain negative values.
- `sceneScaleEV` is a coordinate/representation scale. It is not render exposure, display white, middle gray, or capture exposure.
- `renderExposureEV` belongs only to rendering intent and must never be folded back into RAW normalization or scene coordinates.
- Reconstruction must not silently clip sub-black values, highlights above nominal white, negative scene coordinates, gamut excursions, or transfer-function inputs.
- Camera metadata carries source/validity/confidence where provenance matters. Prefer explicit provenance over an unqualified scalar.
- The deterministic FP32 reference path is the executable semantic oracle. Production Vulkan work must be differentially testable against equivalent reference semantics.
- Vulkan capabilities select lowerings and fast paths; they do not redefine imaging semantics. Vulkan 1.1 compute is the production baseline; later features are optional accelerators.
- Successful AHardwareBuffer import is not proof of zero-copy. Distinguish allocation sharing, import success, and measured memory traffic.
- Codec standards behavior is a leaf integration concern. Gain-map math/container semantics stay in standards-oriented codec libraries unless an ADR explicitly changes that boundary.
- Third-party dependencies must not contaminate the deterministic reference core with incompatible compiler/FP policy.
- Semantic identity/lineage and physical storage are separate concerns. Platform handles and allocator choices must never become the definition of a `RawFrame`, `SceneFrame`, or rendition.
- Observation, intent/policy, capability, and semantic-rule authority classes must remain distinguishable through compilation and tracing.

## Current transitional structures: do not mistake them for the final architecture

Several APIs are intentionally incomplete seeds:

- `ImagingGraph` is a prototype semantic IR. Its operation traits (`access`, `pure`, `canFuse`, `changesReferenceDomain`) are caller-provided today; target architecture should derive these from a canonical operation schema so callers cannot create self-contradictory nodes.
- `OperationKind::GainMapEncode` predates the explicit codec-boundary design. Do not use it as justification to move Ultra HDR gain-map semantics into the imaging core. Reconcile this taxonomy when the graph/compiler is promoted.
- `ImagingBackend` currently exposes only `reconstructSingleRaw()`. Treat it as a transitional adapter, not the future universal backend interface.
- `ComputeRunner` is a deliberately synchronous one-queue correctness harness. It is not the final frame-graph scheduler, allocator, or multi-frame executor.
- `SceneFrame`, `RenderedFrame`, and `ImageType` currently repeat some semantic fields. Avoid adding new duplicated semantic state; the roadmap calls for a tighter single-source-of-truth model.
- `RawFrame`, `SceneFrame`, and `RenderedFrame` currently own CPU vectors because they are convenient host/reference values. Do **not** evolve them by embedding Vulkan/AHardwareBuffer handles. Target execution binds semantic values to backend resources in an `ExecutionPlan`.
- `sourceRawId` is a single-source lineage convenience. Do **not** overload it to represent a burst or multi-stage provenance graph. Future multi-frame work needs typed identities and explicit lineage/source sets.
- `ReconstructionConfig` currently mixes algorithm selection, calibration/observation-derived values, and semantic parameters. Do not expand this pattern into a universal config; the target control plane separates request/observation/policy/capability inputs.

## Change protocol

For any non-trivial change:

1. Identify the semantic state transition being changed: sensor -> scene, scene -> display, display -> codec, or execution-only lowering.
2. Classify relevant inputs as semantic rules, observations, image intent/delegated policy, or execution capabilities.
3. Locate the authoritative type/invariant and the reference behavior before editing production code.
4. If adding a semantic operation, define its schema and reference semantics before or together with a production lowering.
5. Keep intent and policy separate from mechanism: capture/reconstruction/render/codec meaning must not leak into backend capability code.
6. Add the narrowest regression or golden test that fails for the old behavior and proves the new contract.
7. For Vulkan/optimized implementations, add or extend differential evidence rather than replacing semantic tests with device-specific expectations.
8. Update `docs/roadmap.md` only when capability maturity or sequencing changes. Add/supersede an ADR when changing a durable boundary. For work spanning multiple architectural steps or PRs, create/update a plan in `docs/plans/`.
9. Query GitHub for the latest PR/head/CI state instead of recording volatile delivery snapshots in architecture/roadmap text.
10. Run the relevant validation ladder from `docs/verification.md`; inspect failures rather than weakening gates.
11. Re-read the final diff for semantic drift, duplicate truth, hidden clipping, accidental backend coupling, broken lineage, physical-resource leakage, cross-authority substitution, and claims not supported by tests.

## Design preference for future APIs

Prefer structures that reduce the number of facts an agent must hold simultaneously:

- explicit sum/enumeration types over magic values;
- canonical constructors/schema registries over free-form combinations;
- immutable semantic values with explicit transitions over mutation across domains;
- generated/derived execution traits over duplicated caller annotations;
- typed identities and explicit lineage over reused scalar IDs;
- semantic descriptors separated from physical resource bindings;
- typed authority/context objects over universal configuration structs;
- stable IDs and machine-readable diagnostics over prose-only logs;
- live queries for volatile state, durable documents only for durable truth;
- plans/traces that explain *why* a lowering/fallback/policy choice was selected, not only what ran.

The architectural north star is: **semantics say what the image means; observations say what was learned; intent says what is wanted; capabilities say what can run; the compiler turns those into an explicit plan; executors perform it; evidence proves the plan preserved the requested meaning.**
