# Plan 0001: Promote the semantic control plane

Status: Proposed
Owner: repository architecture
Related: ADR-0001, ADR-0002

## Goal

Turn the current `ImagingGraph` seed plus direct reference/Vulkan entry points into one explicit route from semantic request -> validated graph -> compiled execution plan -> executor -> trace, without changing existing single-RAW image semantics.

This plan is intentionally sequenced before large burst/multi-pass executor work because those features would otherwise amplify the current orchestration split.

## Acceptance criteria

The work is complete when all of the following are true:

1. Every supported semantic operation has one canonical schema defining intrinsic traits and type/domain constraints.
2. Callers cannot arbitrarily contradict purity/access/fusion/domain-change facts for a known operation.
3. External graph inputs/outputs and codec boundaries have an explicit representation; `RawIngress`/`GainMapEncode` ambiguity is removed or redefined consistently with accepted ADRs.
4. Semantic values have canonical descriptors and typed identities/lineage; backend storage is represented separately.
5. A first-class `ExecutionPlan` records stable operation/resource IDs, selected lowerings, precision/storage choices, lifetimes/dependencies, capability requirements, and fallback reasons.
6. A compiler deterministically produces the same plan/debug representation for the same graph + capabilities/profile + policy inputs.
7. Existing single-RAW convenience APIs can be expressed as thin builders/adapters over the new control plane without changing their validated results.
8. Existing K1a/K1b Vulkan lowerings are reachable through the plan/executor path and remain differentially equivalent to reference semantics.
9. `ExecutionTrace` can correlate request/semantic lineage, compiler choices, plan resources, backend dispatches, and relevant fallback/capability decisions.
10. Existing default and no-Vulkan CI configurations remain green; new compiler/schema/trace tests are added.

## Architectural constraints

- Follow `docs/architecture.md`.
- ADR-0001: semantic control plane is authoritative; executors do not invent imaging policy.
- ADR-0002: semantic identity/lineage is separate from physical resource storage.
- Preserve current `SceneFrame` meaning and the `sceneScaleEV`/`renderExposureEV` separation.
- Keep FP32 reference behavior deterministic.
- Keep Vulkan 1.1 compute as production baseline and optional features as accelerators.
- Keep Ultra HDR gain-map/container behavior at the external codec boundary.
- Do not require Android/AHardwareBuffer availability to test compiler semantics.

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
- reference implementation identifier/availability;
- production lowering identifiers/capability requirements.

Migration:

- preserve a compatibility graph-builder surface where useful;
- derive intrinsic descriptor fields from the schema;
- only allow caller-specified values for true parameters, not intrinsic semantics.

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

## Step 5 — Define `ExecutionPlan`

Start with the current single-RAW graph and existing two-kernel Vulkan chain.

Minimum plan contents:

- semantic operation/group IDs;
- resource IDs and semantic-value association;
- selected lowering;
- storage/precision format;
- dependencies/barriers;
- lifetime intervals/classes;
- capability requirements and fallback reason;
- external input/output bindings.

Do not add performance heuristics yet. Prefer a deterministic, conservative compiler.

Validation:

- stable debug representation for golden tests;
- deterministic plan for fixed graph/capability input;
- Vulkan-off capabilities select reference/portable paths without graph changes;
- unsupported requirements produce explicit diagnostics rather than partial plans.

## Step 6 — Lower current K1a/K1b through the compiler

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

## Step 7 — Migrate convenience APIs

Make current high-level entry points thin adapters rather than parallel orchestration engines.

Candidates:

- `reconstructSingleRaw()` builds/executes the canonical semantic request;
- future render/export convenience functions do the same for their subgraphs.

Do not remove simple reference APIs until tests and callers have a stable replacement path.

Validation:

- current public/reference tests produce unchanged semantic results;
- direct graph path and convenience path produce equivalent graph/plan fingerprints where applicable.

## Step 8 — Add `ExecutionTrace`

Trace should be structured data first; human-readable formatting is a view.

Minimum trace facts:

- request/run IDs;
- semantic value IDs and lineage;
- selected metadata/fallbacks where available;
- compiler/profile/capability inputs;
- operation -> lowering mapping;
- plan resource -> physical binding mapping;
- precision/storage choices;
- fallback reasons;
- dispatch/integration result status;
- optional measured timing/memory counters, clearly separated from inferred claims.

Validation:

- deterministic portions have stable tests;
- trace can answer which lowering ran and why without parsing free-form logs;
- no secret/platform-opaque handles are required for semantic diagnosis.

## Step 9 — Retire transitional duplication deliberately

Only after the new path is proven:

- narrow/remove caller-controlled descriptor fields that are now schema-derived;
- decide whether `ImagingBackend` remains a tiny convenience abstraction or is removed;
- keep `ComputeRunner` internal to the Vulkan executor or replace it incrementally;
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

Compiler-specific tests should not require a Vulkan device. Vulkan lowering/executor tests should continue to run against the CI software driver when available. Existing libultrahdr integration must remain isolated and green when compiler changes touch output graph boundaries.

## Migration strategy

- Land schema/type work before execution-plan work.
- Keep existing APIs functional as adapters while each new layer gains tests.
- Avoid a flag day rewrite of reference reconstruction or Vulkan kernels.
- Prefer one semantic source of truth with temporary adapters over two synchronized orchestration implementations.
- Each PR should leave the repository in a state where the old and new paths are either equivalent or the migration boundary is explicit.

## Status ledger

- Completed: architectural direction captured in ADR-0001 and ADR-0002.
- Completed: current branch/IR/backend gaps inventoried.
- Current: documentation/operating model established.
- Remaining: Steps 1-9 are implementation work and have not been started by this documentation change.

## Risks and open questions

- Exact C++ naming/ownership for semantic IDs, lineage, plan resources, and compiler objects is intentionally unresolved until Step 1/3 design work reads actual call sites/tests.
- Whether convenience reference frames remain owning structs or gain non-owning views should be decided from usage/performance evidence, not from this plan alone.
- Future burst scheduling may require execution-plan features not visible in the current single-RAW graph; keep the plan extensible without designing an abstract scheduler before temporal semantics exist.
