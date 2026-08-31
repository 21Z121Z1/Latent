# Latent roadmap and system status

Status date: 2026-09-01.

This document is the plan of record for **what exists, what is stacked but not merged, what is transitional, and what should be built next**. Stable design belongs in `architecture.md`; do not put long-lived architectural truth in branch names or PR descriptions.

## 1. Branch topology

The repository currently has seven branches. They are not seven competing architectures.

| Branch | Relationship to current `main` | Meaning |
| --- | --- | --- |
| `main` | authoritative merged baseline | Contains PRs #1-#8 through scene analysis/output primitives. Head: merge of PR #8. |
| `feat/scene-analysis-output-primitives` | fully behind `main` | Historical PR #8 implementation branch; already absorbed. |
| `feat/vulkan-demosaic-color-kernel` | fully behind `main` | Historical PR #6 implementation branch; already absorbed. |
| `fix/compute-runner-lifetime` | fully behind `main` | Historical PR #7 fix branch; already absorbed. |
| `feat/reference-sdr-hdr-renderer` | ahead of `main` | Open PR #9. Adds independent SDR/HDR reference rendering. |
| `feat/ultrahdr-codec-staging` | stacked on the renderer branch | Open PR #10, deliberately stacked on #9. Adds explicit rendition staging and optional libultrahdr integration. This is the newest complete implementation view. |
| `docs/agent-first-system-architecture` | stacked on the Ultra HDR branch | Open PR #11. Documentation/control-plane reframe only; no imaging/build code changes. |

The documentation/control-plane work is intentionally reviewed on top of `feat/ultrahdr-codec-staging`, because that branch contains the current superset of implemented semantics. Once #9 and #10 land, the docs can follow the stack into `main` without changing architectural meaning.

Branch commit counts are deliberately not part of this table: they are delivery state and change on every update. Use GitHub comparison/PR metadata when exact counts matter.

## 2. Maturity vocabulary

Use these terms consistently:

- **Contracted** — semantic types/invariants exist and are validated.
- **Reference** — deterministic executable reference behavior exists.
- **Production** — a Vulkan/platform/external production realization exists.
- **Verified** — golden/differential/integration evidence runs in tests/CI.
- **Transitional** — implemented and useful, but explicitly not the final control-plane shape.
- **Planned** — design direction is known but no stable implementation contract exists yet.

A feature may occupy several states simultaneously: e.g. rendering can be Contracted + Reference + Verified while production Vulkan lowering remains Planned.

## 3. Capability maturity matrix

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
| Independent SDR/HDR render | yes on #9/#10 | yes | planned | yes | Current renderer is not claimed to be full ACES 2 Output Transform. |
| Ultra HDR rendition staging | yes on #10 | yes/deterministic | CPU packing | yes | Explicit SDR/HDR inputs; gain-map math stays external. |
| libultrahdr JPEG integration | external contract on #10 | n/a | yes | real encode + probe in CI | External isolated target. |
| HEIF/AVIF Ultra HDR | enum/routing only | n/a | incomplete | incomplete | Needs libheif/platform validation. |
| `ImagingGraph` semantic IR | prototype | partial validation/fusion | no universal compiler | unit tests | Transitional seed, not yet the single control plane. |
| Graph compiler + first-class `ExecutionPlan` | planned | planned | planned | planned | Highest-leverage architectural gap. |
| Structured `ExecutionTrace` | planned | n/a | planned | planned | Needed for explainability/agent diagnostics. |
| Android NDK capture contracts | planned | fixtures planned | planned | planned | Should create capture semantics, not leak platform handles inward. |
| `RawBurst` + temporal reconstruction | planned | planned | planned | planned | Requires lifetime/scheduling model first. |
| Production Vulkan rendering | planned | reference oracle exists | planned | differential planned | Should lower from same render semantics. |
| Multi-frame scheduler/allocator | planned | n/a | planned | lifetime/perf tests planned | `ComputeRunner` is transitional. |

## 4. Architectural priority order

This order is intentionally based on **control leverage**, not visual feature novelty. The goal is to prevent the next wave of capture/burst/render work from multiplying ad-hoc orchestration paths.

### P0 — Consolidate the semantic control plane

Before large new multi-pass features, close the gap between `ImagingGraph`, direct reference calls, kernel wrappers, and `ImagingBackend`.

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
6. Define a first-class `ExecutionPlan` with stable operation/resource IDs, selected lowerings, precision/storage decisions, lifetimes, barriers, and capability reasons.
7. Replace the idea of one monolithic `ImagingBackend` with thin request APIs over graph compilation + executors.
8. Define `ExecutionTrace` before introducing complex asynchronous/multi-frame behavior.
9. Add tests for graph schemas, illegal states, compiler determinism, lineage, resource binding, fallback reasons, and plan/trace debug representations as appropriate.

Detailed sequencing and acceptance criteria live in `docs/plans/0001-semantic-control-plane.md`.

Completion criterion: a new semantic operation has one obvious route from contract -> reference -> compiler -> lowering -> evidence, with semantic lineage and physical execution separately inspectable.

### P1 — Android capture contracts and recorded fixtures

Deliverables:

- NDK/Camera2 capture boundary that produces semantic capture objects rather than leaking platform handle semantics inward;
- robust metadata decoding with source/validity/confidence;
- active-array/crop/coordinate conventions for LSC/defects;
- recorded metadata/raw fixtures for deterministic tests;
- AImage/AHardwareBuffer ingress adapter with portable-copy correctness baseline;
- explicit device probe results recorded in traces.

Completion criterion: the same recorded capture can be reconstructed deterministically without a live camera, while live Android capture supplies the same semantic contract.

### P2 — `RawBurst` and temporal reconstruction semantics

Do not start by writing an alignment shader. Start by defining the burst object and temporal operation semantics.

Deliverables:

- `RawBurst` identity, timestamp/exposure ordering, per-frame provenance, and calibration relation;
- temporal access patterns and graph barriers;
- motion/alignment semantic outputs and confidence;
- robust merge/reference implementation;
- propagated uncertainty through temporal fusion;
- memory-lifetime model for multi-frame resources;
- differential/golden fixtures for synthetic and recorded bursts.

Completion criterion: burst output semantics can be explained and tested independently of the production scheduler.

### P3 — Production executor evolution

Promote the Vulkan runtime from correctness harness to execution-plan consumer.

Deliverables:

- resource allocator with explicit lifetime/aliasing rules;
- multi-pass scheduling;
- timeline/synchronization2 fast paths where supported, without making them baseline requirements;
- device-local/staging strategy;
- non-coherent host-memory correctness;
- multi-frame in-flight policy;
- structured timing/memory counters in `ExecutionTrace`;
- fallback and device capability reasoning.

Completion criterion: executor behavior is explainable from an `ExecutionPlan` + `DeviceCaps`, not from hidden wrapper control flow.

### P4 — Production rendering and Android display/output integration

Deliverables:

- Vulkan lowering of independent SDR/HDR render semantics;
- differential validation against the reference renderer;
- explicit display capability/target negotiation;
- Android surface/color-mode integration;
- HDR/SDR preview policy kept separate from scene master;
- evaluate/replace the current neutral-axis gamut strategy only through an explicit render-policy/ADR change.

Completion criterion: reference and production render branches agree within defined budgets on the same `SceneFrame` and render intent.

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

Any heuristic choice must be traceable and reproducible from a profile/version + request.

## 5. Immediate merge/stack hygiene

Current recommended order:

1. Review/merge PR #9 (`feat/reference-sdr-hdr-renderer`) into `main`.
2. Rebase/update PR #10 onto the new `main` only after #9 merges, preserving the explicit codec boundary.
3. Carry PR #11 documentation on top of the latest semantic superset, then retarget it as the stack collapses.
4. Do not begin a competing long-lived feature branch from an older historical branch.

This is repository hygiene, not architecture. The semantic design should remain invariant under branch-stack cleanup.

## 6. Decision triggers

Create or supersede an ADR before making any change that alters one of these:

- reference-domain ownership or a domain transition;
- the definition of `SceneFrame`;
- semantic identity/lineage or physical-resource ownership;
- precision/error-budget policy;
- which layer owns tone/gamut/gain-map behavior;
- reference-vs-production authority;
- graph/compiler/executor responsibilities;
- metadata provenance precedence;
- external dependency isolation;
- baseline Vulkan capability policy;
- device-profile or heuristic reproducibility rules.

Small implementation details do not need ADRs. Durable boundaries do.
