# Latent agent operating contract

Latent is a **typed semantic imaging system**: requested image meaning is compiled/lowered into execution backends and proven against deterministic reference semantics. Vulkan, platform handles, codecs, and heuristics realize the system; they do not define its meaning.

This file is intentionally small because it is the default agent entry point. Read deeper material only when the task requires it:

- stable system model and known structural gaps -> `docs/architecture.md`
- durable boundary decisions -> `docs/decisions/`
- capability maturity and sequencing -> `docs/roadmap.md`
- active multi-step implementation state -> `docs/plans/`
- evidence requirements and commands -> `docs/verification.md`
- live PR/branch/SHA/review/CI state -> query GitHub at decision time

Never use a copied delivery snapshot as architectural truth.

## System orientation

```text
capture / fixture
    -> RawFrame / future RawBurst        sensor-referred
    -> SceneFrame                        scene-referred AP1/D60,
                                         linear, unbounded, negative-preserving
    -> RenderedFrame                     explicit SDR/HDR display rendition
    -> codec/export                      encoded artifact

semantic request + observations + delegated policy + capabilities
    -> semantic graph
    -> compiler
    -> ExecutionPlan
    -> reference / Vulkan / platform / external executor
    -> outputs + evidence / ExecutionTrace
```

`GraphCompiler`, first-class `ExecutionPlan`, typed multi-source lineage, authority-separated compiler contexts, `RawBurst`, and structured `ExecutionTrace` are target architecture, not current APIs.

## Route questions to their authority

| Question | Source of truth |
| --- | --- |
| What does this value/image mean? | semantic types, validation, reference behavior, tests |
| What boundary/invariant should hold? | `docs/architecture.md` + latest applicable accepted ADR |
| Why was that boundary chosen? | `docs/decisions/` |
| What is transitional / what comes next? | `docs/roadmap.md` |
| What is the current multi-PR migration state? | `docs/plans/` |
| What is live on GitHub now? | GitHub query, never a dated doc snapshot |
| What evidence is required? | `docs/verification.md` + relevant tests/workflow |

If code/tests, architecture, and an accepted ADR disagree, identify which source is stale; do not silently reconcile them.

## Classify inputs before making decisions

ADR-0003 defines four authority classes:

- **semantic rules** — invariants/schemas defining correctness;
- **observations/evidence** — captured, calibrated, estimated, profiled, or measured facts with provenance where relevant;
- **image intent / delegated policy** — fixed requested result plus choices explicitly delegated to Latent;
- **execution capabilities** — device/platform/backend facts.

Capabilities may select legal lowerings, not silently change fixed image intent. Policy defaults may not masquerade as observations. Semantic variation is allowed only when the request explicitly delegates it and the decision remains traceable.

## Read the smallest useful slice

| Task | Start here |
| --- | --- |
| Sensor/capture | `imaging/RawFrame.h`, `imaging/Types.h`, relevant reference/ingress tests |
| RAW reconstruction | `reference/ReferenceReconstruct.h`, then DNG/demosaic/sensor/noise code and tests |
| Scene semantics | `imaging/SceneFrame.h`, `imaging/Types.h`, scene-analysis tests |
| Graph/compiler | `graph/ImagingIR.h/.cpp`, architecture gaps, ADRs, active plan |
| Vulkan | `vulkan/DeviceCaps.h`, `IngressPlan.h`, `ComputeRunner.h`, kernel wrappers/shaders, differential tests |
| Rendering | `imaging/RenderedFrame.h`, `render/SceneAnalysis.h`, `render/ReferenceRenderer.h`, render tests |
| Codec/export | `codec/UltraHdrStaging.h`, `codec/UltraHdrEncoder.h`, codec tests |
| Build/CI | `CMakeLists.txt`, `.github/workflows/ci.yml`, `docs/verification.md`, latest GitHub run |

Do not scan the repository by default; semantic contracts and their tests are the high-information navigation nodes.

## Non-negotiable invariants

- Sensor, scene, display, and codec/container domains are distinct; domain transitions are explicit.
- `SceneFrame` is the scene master: linear ACEScg/AP1 D60, unbounded, negative-preserving.
- `sceneScaleEV` is a representation coordinate, never render exposure/display white/middle gray/capture exposure.
- `renderExposureEV` belongs to rendition intent and never feeds back into RAW normalization or scene coordinates.
- Reconstruction does not silently clip sub-black, >nominal-white, negative scene, or gamut-excursion values.
- Metadata/observations retain source/validity/confidence where provenance matters.
- Deterministic FP32 reference behavior is the semantic oracle; optimized/Vulkan work needs semantic-equivalent differential evidence.
- Vulkan 1.1 compute is the production baseline; optional capabilities select accelerations, not correctness semantics.
- AHardwareBuffer import success is not proof of zero-copy; measured traffic is a separate claim.
- Ultra HDR gain-map/container behavior stays at the codec boundary unless an ADR changes ownership.
- Semantic identity/lineage is separate from physical storage; do not embed backend handles into semantic frame meaning or overload `sourceRawId` for multi-source provenance.
- Do not grow mixed-purpose configuration bags; target APIs separate semantic request, observations, delegated policy, and capabilities.

## Change protocol

For non-trivial work:

1. Identify the semantic transition or execution-only lowering being changed.
2. Classify inputs by authority and locate the authoritative type/invariant/reference behavior.
3. For a new semantic operation, define/extend its schema and deterministic reference semantics before or with production lowering.
4. Keep policy, observations, semantic meaning, and backend mechanism separate.
5. Add the narrowest regression/golden/property/differential/integration evidence appropriate to the change.
6. Update roadmap only for maturity/sequencing; update/create an ADR only for durable boundaries; update a plan for multi-step migration state.
7. Query latest GitHub state and validate the latest SHA using `docs/verification.md`.
8. Re-read the final diff for duplicate truth, hidden clipping, semantic drift, backend leakage, broken lineage, cross-authority substitution, and unsupported claims.

Prefer canonical schemas/constructors, typed identities, explicit lineage, immutable semantic transitions, backend-neutral resources, structured diagnostic codes, stable IDs/fingerprints, and machine-readable plans/traces over free-form duplicated state.

**North star:** semantics say what the image means; observations say what was learned; intent says what is wanted/delegated; capabilities say what can run; the compiler makes an explicit plan; executors perform it; evidence proves the requested meaning was preserved.
