# Latent roadmap and capability status

Status date: 2026-09-01.

This document is the plan of record for **capability maturity, architectural leverage, dependencies, and sequencing**. It is deliberately **not** a live branch/PR dashboard.

Exact branch heads, open/merged PR state, review threads, workflow results, and commit counts are volatile repository facts. Query GitHub when they matter. Copying them here creates stale pseudo-authority and increases agent error rates.

## 1. Maturity vocabulary

Use these terms consistently:

- **Contracted** — semantic types/invariants exist and are validated.
- **Reference** — deterministic executable reference behavior exists.
- **Production** — a Vulkan/platform/external production realization exists.
- **Verified** — golden/differential/integration evidence runs in tests/CI.
- **Transitional** — implemented and useful, but explicitly not the final control-plane shape.
- **Planned** — design direction is known but no stable implementation contract exists yet.

A capability may occupy several states simultaneously: rendering can be Contracted + Reference + Verified while production Vulkan lowering remains Planned.

## 2. Current capability maturity

The current merged implementation baseline includes independent SDR/HDR reference rendering and explicit Ultra HDR codec staging/JPEG integration. Delivery-state details should still be verified from GitHub before acting on a specific SHA.

| Capability | Contract | Reference | Production realization | Verification | Notes |
| --- | --- | --- | --- | --- | --- |
| Sensor/reference domains and `ImageType` validation | yes | n/a | n/a | yes | Core semantic foundation. |
| `RawFrame` + provenance/confidence metadata | yes | yes | partial ingress support | yes | Android capture adapter still planned. |
| RAW10/RAW12 canonical unpack | yes | yes | CPU portable path | yes | Direct Android AHB import execution still planned. |
| Black/white selection + normalization | yes | yes | Vulkan K1a | differential | Negative/sub-black preservation is intentional. |
| Defect correction + LSC + WB | yes | yes | K1a for LSC/WB; defect correction remains reference-side | yes | Keep map/detection provenance explicit. |
| Demosaic | yes | MHC + baseline | Vulkan K1b | differential | FP32 accumulation, FP16 storage boundaries validated. |
| DNG camera color -> AP1/D60 | yes | yes | Vulkan K1b matrix lowering | differential | Full DNG profile preparation remains reference/orchestration side. |
| Noise propagation | yes | yes | no dedicated production lowering | Monte Carlo + analytic | Marginal variance semantics only; no covariance model yet. |
| Scene analysis | yes | yes | no Vulkan lowering | yes | Invariant to `sceneScaleEV`. |
| Independent SDR/HDR render | yes | yes | planned Vulkan lowering | yes | Current renderer is not claimed to be full ACES 2 Output Transform. |
| Ultra HDR rendition staging | yes | deterministic | CPU packing | yes | Explicit SDR/HDR inputs; gain-map math stays external. |
| libultrahdr JPEG integration | external contract | n/a | yes | real encode + probe in CI | External isolated target. |
| HEIF/AVIF Ultra HDR | enum/routing only | n/a | incomplete | incomplete | Needs libheif/platform validation. |
| `ImagingGraph` semantic IR | prototype | partial validation/fusion | no universal compiler | unit tests | Transitional seed, not yet the single control plane. |
| Canonical operation schema | planned | planned | n/a | planned | Removes caller-controlled intrinsic traits. |
| Authority-separated compiler context | planned | n/a | n/a | planned | ADR-0003: separate semantic rules, observations, intent/policy, capabilities. |
| Typed semantic identity + multi-source lineage | planned | n/a | n/a | planned | Required before burst provenance expands. |
| Semantic/resource separation | planned | host/reference pattern exists | planned bindings | planned | ADR-0002; do not embed platform handles in semantic frames. |
| Graph compiler + first-class `ExecutionPlan` | planned | planned | planned | planned | Highest-leverage architectural gap. |
| Structured `ExecutionTrace` | planned | n/a | planned | planned | Needed for explainability/agent diagnostics. |
| Android NDK capture contracts | planned | fixtures planned | planned | planned | Should create capture semantics, not leak platform handles inward. |
| `RawBurst` + temporal reconstruction | planned | planned | planned | planned | Requires identity/lineage and lifetime model first. |
| Production Vulkan rendering | planned | reference oracle exists | planned | differential planned | Should lower from the same render semantics. |
| Multi-frame scheduler/allocator | planned | n/a | planned | lifetime/perf tests planned | `ComputeRunner` is transitional. |

## 3. Architectural priority order

This order is based on **control leverage**, not feature novelty. The goal is to prevent capture, burst, render, and executor work from multiplying ad-hoc orchestration paths or ambiguous configuration authority.

### P0 — Consolidate the semantic control plane

Before substantial new multi-pass features, close the gap between `ImagingGraph`, direct reference calls, kernel wrappers, `ImagingBackend`, and mixed-purpose configuration structs.

Deliverables:

1. Define a canonical operation schema registry so intrinsic traits are derived, not caller-invented.
2. Reconcile graph ingress/egress taxonomy:
   - graph inputs vs `RawIngress`;
   - render-domain transitions;
   - external codec/export nodes;
   - remove or redefine the legacy `GainMapEncode` implication so gain-map math remains at the codec boundary.
3. Introduce canonical semantic type constructors/descriptors to reduce duplicated primaries/white/transfer/reference/range state.
4. Define typed semantic identities and multi-source lineage so future burst provenance does not overload `sourceRawId`.
5. Separate semantic values from physical resource bindings; host vectors and Vulkan/AHardwareBuffer resources are realizations, not image meaning.
6. Separate compiler inputs by authority (ADR-0003): semantic request/rules, observations/evidence, intent/delegated policy, and execution capabilities.
7. Define a first-class `ExecutionPlan` with stable operation/resource IDs, selected lowerings, precision/storage decisions, lifetimes, barriers, capability requirements, policy decisions, and fallback reasons.
8. Replace the idea of one monolithic `ImagingBackend` with thin request APIs over graph compilation + executors.
9. Define `ExecutionTrace` before introducing complex asynchronous/multi-frame behavior.
10. Add tests for graph schemas, illegal states, authority boundaries, compiler determinism, lineage, resource binding, fallback reasons, and plan/trace debug representations.

Detailed sequencing and acceptance criteria live in `docs/plans/0001-semantic-control-plane.md`.

Completion criterion: a new semantic operation has one obvious route from contract -> reference -> compiler -> lowering -> evidence, with semantic lineage, input authority, and physical execution separately inspectable.

### P1 — Android capture contracts and recorded fixtures

Deliverables:

- NDK/Camera2 capture boundary that produces semantic capture objects rather than leaking platform handle semantics inward;
- robust metadata decoding with source/validity/confidence;
- explicit classification of captured facts vs calibration/profile estimates vs policy defaults;
- active-array/crop/coordinate conventions for LSC/defects;
- recorded metadata/raw fixtures for deterministic tests;
- AImage/AHardwareBuffer ingress adapter with portable-copy correctness baseline;
- explicit device probe results recorded as capability/trace facts.

Completion criterion: the same recorded capture can be reconstructed deterministically without a live camera, while live Android capture supplies the same semantic/observation contracts.

### P2 — `RawBurst` and temporal reconstruction semantics

Do not start by writing an alignment shader. Start by defining burst identity, lineage, observations, and temporal semantic operations.

Deliverables:

- `RawBurst` identity, timestamp/exposure ordering, per-frame provenance, and calibration relation;
- temporal access patterns and graph barriers;
- motion/alignment semantic outputs and confidence;
- robust merge/reference implementation;
- propagated uncertainty through temporal fusion;
- memory-lifetime model for multi-frame resources;
- differential/golden fixtures for synthetic and recorded bursts.

Completion criterion: burst output semantics and lineage can be explained and tested independently of the production scheduler.

### P3 — Production executor evolution

Promote the Vulkan runtime from correctness harness to `ExecutionPlan` consumer.

Deliverables:

- resource allocator with explicit lifetime/aliasing rules;
- multi-pass scheduling;
- timeline/synchronization2 fast paths where supported, without making them baseline requirements;
- device-local/staging strategy;
- non-coherent host-memory correctness;
- multi-frame in-flight policy;
- structured timing/memory counters in `ExecutionTrace`;
- fallback and device capability reasoning.

Completion criterion: executor behavior is explainable from an `ExecutionPlan` + capability context; it does not hide image-policy decisions in wrapper control flow.

### P4 — Production rendering and Android display/output integration

Deliverables:

- Vulkan lowering of independent SDR/HDR render semantics;
- differential validation against the reference renderer;
- explicit display capability/target negotiation;
- Android surface/color-mode integration;
- HDR/SDR preview policy kept separate from scene master;
- evaluate/replace the current neutral-axis gamut strategy only through an explicit render-policy/ADR change.

Completion criterion: reference and production render branches agree within defined budgets on the same `SceneFrame` and fixed render intent; runtime capabilities do not silently alter that intent.

### P5 — Codec completion and format validation

Deliverables:

- HEIF/AVIF integration tests with actual platform/external dependencies;
- standards metadata validation;
- failure/fallback policy by container/device;
- avoid duplicating libultrahdr gain-map behavior unless a measured requirement and ADR justify owning that implementation.

### P6 — Device specialization and performance policy

Only after the control plane and trace are explicit:

- per-device profiles;
- measured zero-copy decisions;
- precision/fusion specialization;
- autotuning or performance heuristics;
- profile versioning and reproducibility;
- quality/latency/power policy inputs.

Any heuristic choice must be reproducible from semantic request + observation/profile version + delegated policy + capability context and recorded in the trace.

## 4. Knowledge freshness rule

Agents should minimize both search cost and stale-state risk:

- durable invariants -> architecture + accepted ADRs;
- capability maturity/sequencing -> this roadmap;
- active multi-step implementation state -> `docs/plans/`;
- live PR/branch/SHA/review/CI status -> GitHub query at decision time;
- historical rationale for a delivery increment -> PR/commit history only when needed.

Do not maintain exact branch counts, commit counts, current CI run IDs, or open-PR lists in this file. Those facts expire faster than architecture changes and should remain live data.

## 5. Decision triggers

Create or supersede an ADR before making any change that alters one of these:

- reference-domain ownership or a domain transition;
- the definition of `SceneFrame` or future `RawBurst`;
- semantic identity/lineage or physical-resource ownership;
- authority boundaries among semantic rules, observations, intent/policy, and capabilities;
- precision/error-budget policy;
- which layer owns tone/gamut/gain-map behavior;
- reference-vs-production authority;
- graph/compiler/executor responsibilities;
- metadata provenance precedence;
- external dependency isolation;
- baseline Vulkan capability policy;
- device-profile or heuristic reproducibility rules.

Small implementation details do not need ADRs. Durable boundaries do.
