# Plan 0001: Promote the semantic control plane

Status: Proposed
Owner: repository architecture
Related: ADR-0001, ADR-0002, ADR-0003, ADR-0004, Plan 0002
Current step: Not started; Step 1 canonical operation schemas is the first implementation dependency.

## Goal

Turn the current `ImagingGraph` seed plus direct reference/Vulkan entry points into one explicit route from semantic request -> validated graph -> authority-separated compile context -> execution plan -> executor -> trace, without changing existing single-RAW image semantics.

This plan is intentionally sequenced before large burst/multi-pass executor work because those features would otherwise amplify the current orchestration split and mixed-purpose configuration state. ADR-0004/Plan 0002 is a cross-cutting self-description/versioning slice of this work: the same canonical registries used by compilation must eventually derive the machine-readable system catalog and structured artifact identity rather than spawning a parallel manifest.

## Acceptance criteria

The work is complete when all of the following are true:

1. Every supported semantic operation has one canonical schema defining intrinsic traits and type/domain constraints.
2. Callers cannot arbitrarily contradict purity/access/fusion/domain-change facts for a known operation.
3. External graph inputs/outputs and codec boundaries have an explicit representation; `RawIngress`/`GainMapEncode` ambiguity is removed or redefined consistently with accepted ADRs.
4. Semantic values have canonical descriptors and typed identities/lineage; backend storage is represented separately.
5. Compiler inputs distinguish semantic request/rules, observations/evidence, image intent/delegated policy, and execution capabilities; one class cannot silently masquerade as another.
6. A first-class `ExecutionPlan` records stable operation/resource IDs, selected lowerings, precision/storage choices, lifetimes/dependencies, capability requirements, policy decisions, and fallback reasons.
7. A compiler deterministically produces the same plan/debug representation for the same semantic graph/request + observation/profile versions + policy + capabilities.
8. Existing single-RAW convenience APIs can be expressed as thin builders/adapters over the new control plane without changing their validated results.
9. Existing K1a/K1b Vulkan lowerings are reachable through the plan/executor path and remain differentially equivalent to reference semantics.
10. `ExecutionTrace` can correlate request/semantic lineage, observations/fallbacks, policy decisions, compiler choices, plan resources, backend dispatches, and capability facts.
11. Existing default, no-Vulkan, and relevant codec CI configurations remain green; new compiler/schema/context/trace tests are added.
12. Canonical registries support ADR-0004/Plan 0002: a derived build-wide catalog, structured diagnostics, canonical artifact versions/fingerprints, and compatibility checks can be added without duplicating semantic/lowering truth.

## Architectural constraints

- Follow `docs/architecture.md`.
- ADR-0001: semantic control plane is authoritative; executors do not invent imaging policy.
- ADR-0002: semantic identity/lineage is separate from physical resource storage.
- ADR-0003: semantic rules, observations, intent/policy, and execution capabilities have distinct authority.
- ADR-0004: machine-readable system introspection/artifact identity is derived from canonical registries and must not become a separate hand-edited source of truth.
- Preserve current `SceneFrame` meaning and the `sceneScaleEV`/`renderExposureEV` separation.
- Keep FP32 reference behavior deterministic.
- Keep Vulkan 1.1 compute as production baseline and optional features as accelerators.
- Keep Ultra HDR gain-map/container behavior at the external codec boundary.
- Do not require Android/AHardwareBuffer availability to test compiler semantics.
- Do not make live GitHub branch/PR state part of a compiler/design contract.

## Current facts

- `ImagingGraph` contains typed values, `OperationKind`, caller-filled `OperationDescriptor`, validation, and conservative linear fusion grouping.
- `OperationDescriptor` currently exposes intrinsic traits directly to callers.
- graph inputs already exist as `Value{graphInput=true}`, while `OperationKind::RawIngress` is also present.
- graph validation currently rejects operations with no inputs, making the exact intended role of `RawIngress` unclear.
- `OperationKind::GainMapEncode` exists even though current Ultra HDR implementation uses explicit SDR/HDR `RenderedFrame` values and delegates gain-map generation to libultrahdr.
- `ImagingBackend` currently wraps only `reconstructSingleRaw()`.
- `ComputeRunner` executes individual Vulkan pipelines synchronously and directly manages buffers/descriptor resources.
- K1a/K1b kernel wrappers and differential tests already provide a strong reference-to-production validation pattern.
- frame structs currently own host vectors and propagate a scalar `sourceRawId`.
- `ReconstructionConfig` mixes algorithm choice, calibration/observation-derived parameters, semantic coordinate choices, and confidence values; it is useful today but must not become the universal control object.
- `DeviceCaps` is already a useful example of capability facts being kept separate from image semantics.
- there is no current build-wide machine-readable system catalog; implemented semantic operations/reference paths/lowerings/build features still require several source surfaces to reconstruct.
- graph validation and some edge planning currently expose free-form string diagnostics/reasons rather than stable codes/structured fields.

## Step 1 — Freeze semantic schemas around existing operations

Introduce a canonical operation-schema representation without yet changing execution.

Expected schema data:

- operation kind/name;
- accepted input arity/types/domains;
- output-domain/type rule;
- access pattern/radius;
- purity and fusion class;
- temporal/reduction/external barrier class;
- precision requirements;
- required semantic/observation inputs;
- reference implementation identifier/availability;
- supported production lowering identifiers/capability requirements.

Migration:

- preserve a compatibility graph-builder surface where useful;
- derive intrinsic descriptor fields from the schema;
- only allow caller-specified values for true operation parameters, not intrinsic semantics.

Validation:

- one test per operation kind for canonical traits;
- construction rejects trait contradictions;
- existing graph validation/fusion tests remain green.

## Step 2 — Reconcile graph boundaries and type construction

Resolve early-IR ambiguities before building a compiler on top of them.

Tasks:

- decide whether external ingress is represented only as graph inputs or with a dedicated boundary node whose validation semantics are explicit;
- model external export/codec boundaries without implying that gain-map math is a scene/ISP operation;
- add canonical constructors/schemas for sensor, scene AP1/D60, SDR Rec.709/sRGB, and HDR BT.2020/PQ image types;
- reduce the number of independent fields a caller must set correctly;
- preserve explicit domain-change validation.

Validation:

- legal sensor -> scene -> display paths construct successfully;
- illegal scene transfer/range/domain combinations fail at construction/validation;
- codec/export boundaries cannot silently mutate scene semantics.

## Step 3 — Introduce semantic identity and lineage

Implement ADR-0002 before `RawBurst` makes single-source provenance inadequate.

Tasks:

- define typed IDs for semantic values/runs as needed;
- define lineage/source-set representation that handles one or many capture inputs;
- provide adapters from current `sourceRawId` for single-RAW paths;
- do not expose backend handles in lineage/descriptors.

Validation:

- current single-RAW lineage round-trips exactly;
- synthetic multi-source lineage can represent N inputs and intermediate derivations without overloading IDs;
- scene -> rendition -> codec lineage remains queryable.

## Step 4 — Separate semantic values from resource bindings

Keep host/reference containers usable while adding a backend-neutral execution resource model.

Tasks:

- semantic graph values carry type/lineage, not Vulkan objects;
- execution resources carry storage/format/ownership/lifetime information;
- define explicit host/reference bindings for current vectors;
- define Vulkan binding descriptors independently of semantic frame structs;
- do not attempt allocator sophistication yet.

Validation:

- the same semantic value can be bound to a host reference buffer or Vulkan resource without changing its semantic descriptor;
- reference-only build remains independent of Vulkan types/headers where intended.

## Step 5 — Split request, observations, policy, and capabilities

Implement ADR-0003 before introducing a universal compiler API.

Conceptual inputs:

```text
SemanticRequest / validated SemanticGraph
ObservationContext
IntentPolicy
CapabilityContext
```

Exact names may differ, but the authority boundaries must be explicit.

Tasks:

- identify current `ReconstructionConfig`, render config, metadata/profile, and `DeviceCaps` fields by authority class;
- distinguish fixed image intent from choices explicitly delegated to automatic policy;
- require observation fallback/selection to retain source/validity/confidence or equivalent evidence;
- ensure capability data can select legal lowerings but cannot mutate fixed semantic intent;
- make policy/capability choices representable in diagnostics before optimizing them.

Validation:

- changing only `DeviceCaps` may change plan/lowering but not the fixed semantic output contract;
- an unavailable capability produces explicit fallback/rejection rather than an undeclared image change;
- changing an observation source/fallback is visible in diagnostics;
- a policy-selected algorithm is possible only when the request delegates that choice;
- fixed `renderExposureEV` and equivalent image intent survive backend changes unchanged.

## Cross-cutting dependency — Plan 0002 derived system introspection

Plan 0002 begins from the same canonical registries established by Steps 1-5 and must not create another control path.

Before plan/trace artifacts are treated as durable machine-readable data, the combined work should establish:

- version/fingerprint vocabulary that distinguishes content identity from scoped runtime IDs;
- queryable canonical operation/reference/lowering registrations;
- a derived `SystemCatalog` describing static build capability without absorbing runtime `DeviceCaps`, roadmap maturity, or GitHub state;
- structured diagnostic codes/fields with human messages as views;
- deterministic canonical serialization and explicit compatibility/version handling.

The inspection CLI is not a prerequisite for early schema work. The architectural prerequisite is that Steps 1-5 do not design registries in a way that forces later introspection to duplicate their truth.

## Step 6 — Define `ExecutionPlan`

Start with the current single-RAW graph and existing two-kernel Vulkan chain.

Minimum plan contents:

- semantic operation/group IDs;
- resource IDs and semantic-value association;
- selected lowering;
- storage/precision format;
- dependencies/barriers;
- lifetime intervals/classes;
- capability requirements and fallback reasons;
- delegated policy decisions where applicable;
- external input/output bindings;
- schema/catalog/compiler version or fingerprint context needed to interpret deterministic plan data as ADR-0004 lands.

Do not add performance heuristics yet. Prefer a deterministic, conservative compiler.

Validation:

- stable debug representation for golden tests;
- deterministic plan for fixed graph + observations/profile + policy + capability input;
- Vulkan-off capabilities select reference/portable paths without semantic graph changes;
- unsupported requirements produce explicit diagnostics rather than partial plans.

## Step 7 — Lower current K1a/K1b through the compiler

Connect already-proven production kernels before adding new ones.

Tasks:

- define lowering rules for the existing sensor preprocess and demosaic/color fusion groups;
- preserve FP32 compute/FP16 storage policies and differential budgets;
- keep defect correction/reference-only cases explicit rather than silently skipping them;
- keep direct kernel-wrapper APIs temporarily as test adapters if needed.

Validation:

- existing differential matrices remain green;
- plan-selected K1a/K1b outputs equal the existing direct-wrapper path within the same gates;
- compiler diagnostics explain why a fallback occurs.

## Step 8 — Migrate convenience APIs

Make current high-level entry points thin adapters rather than parallel orchestration engines.

Candidates:

- `reconstructSingleRaw()` builds/executes the canonical semantic request;
- future render/export convenience functions do the same for their subgraphs.

Do not remove simple reference APIs until tests and callers have a stable replacement path.

Validation:

- current public/reference tests produce unchanged semantic results;
- direct graph path and convenience path produce equivalent graph/plan fingerprints where applicable.

## Step 9 — Add `ExecutionTrace`

Trace should be structured data first; human-readable formatting is a view.

Minimum trace facts:

- request/run IDs;
- semantic value IDs and lineage;
- concrete observation candidates, sources, selected fallbacks, and confidence where relevant;
- compiler/profile/capability inputs or stable fingerprints;
- fixed intent plus any delegated policy choices actually made;
- operation -> lowering mapping;
- plan resource -> physical binding mapping;
- precision/storage choices;
- fallback/rejection reasons;
- catalog/semantic-schema/compiler/trace version or fingerprint context needed to interpret the artifact;
- dispatch/integration result status;
- optional measured timing/memory counters, clearly separated from inferred claims.

Validation:

- deterministic portions have stable tests;
- trace can answer which lowering ran and why without parsing free-form logs;
- trace can distinguish “observed”, “defaulted/estimated”, “requested”, and “selected because of capability/policy” facts;
- incompatible persisted artifact versions fail explicitly rather than being silently reinterpreted;
- no secret/platform-opaque handles are required for semantic diagnosis.

## Step 10 — Retire transitional duplication deliberately

Only after the new path is proven:

- narrow/remove caller-controlled descriptor fields that are now schema-derived;
- decompose or adapt mixed-purpose configuration objects where this improves authority clarity;
- decide whether `ImagingBackend` remains a tiny convenience abstraction or is removed;
- keep `ComputeRunner` internal to the Vulkan executor or replace it incrementally;
- retire string-only diagnostic compatibility fields only after structured replacements are proven;
- update architecture/roadmap and supersede ADRs only if boundaries changed.

Do not combine this cleanup with unrelated algorithm changes.

## Validation matrix for the full plan

At minimum:

```bash
cmake -S . -B build \
  -DLATENT_BUILD_TESTS=ON \
  -DLATENT_STRICT_WARNINGS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure --verbose

cmake -S . -B build-novk \
  -DLATENT_BUILD_TESTS=ON \
  -DLATENT_ENABLE_VULKAN_RUNTIME=OFF
cmake --build build-novk --parallel
ctest --test-dir build-novk --output-on-failure
```

Compiler/context tests should not require a Vulkan device. Vulkan lowering/executor tests should continue to run against the CI software driver when available. Existing libultrahdr integration must remain isolated and green when compiler changes touch output graph boundaries.

As Plan 0002 lands, the matrix also requires catalog completeness/referential-integrity tests, canonical serialization/fingerprint tests, diagnostic-code uniqueness/field tests, and a no-Vulkan machine-readable introspection smoke test.

## Migration strategy

- Land schema/type work before execution-plan work.
- Land authority/context decomposition before creating a universal compiler entry point.
- Design canonical registries so Plan 0002 introspection is derived from them rather than copied into another registry/manifest.
- Add version/fingerprint context before treating plan/trace serialization as durable interchange.
- Keep existing APIs functional as adapters while each new layer gains tests.
- Avoid a flag-day rewrite of reference reconstruction or Vulkan kernels.
- Prefer one semantic source of truth with temporary adapters over two synchronized orchestration implementations.
- Each PR should leave the repository in a state where the old and new paths are either equivalent or the migration boundary is explicit.

## Status ledger

- Completed: architectural direction captured in ADR-0001, ADR-0002, ADR-0003, and ADR-0004.
- Completed: current IR/backend/configuration/introspection gaps inventoried.
- Completed: documentation/operating model established, including Plan 0002 for the derived introspection slice.
- Current: no implementation step has started; Step 1 canonical operation schemas is the next dependency.
- Remaining: Steps 1-10 plus the cross-cutting Plan 0002 implementation work.

## Risks and open questions

- Exact C++ naming/ownership for semantic IDs, lineage, compiler contexts, plan resources, compiler objects, catalog entries, diagnostics, and fingerprints is intentionally unresolved until implementation work reads actual call sites/tests.
- Whether convenience reference frames remain owning structs or gain non-owning views should be decided from usage/performance evidence, not from this plan alone.
- Future burst scheduling may require execution-plan features not visible in the current single-RAW graph; keep the plan extensible without designing an abstract scheduler before temporal semantics exist.
- Some algorithm choices sit between fixed semantics and delegated policy. APIs must make the delegation explicit rather than assigning every knob permanently to one global policy category.
- Persisted artifact compatibility should remain explicit but modest before 1.0; version rejection is preferable to premature compatibility machinery.
