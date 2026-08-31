# ADR-0001: Semantic control plane is authoritative over execution backends

Status: Accepted
Date: 2026-09-01

## Context

Latent began correctly with the rule “scene semantics first, graph compilation second, Vulkan execution third,” but the staged implementation now exposes several parallel-looking control surfaces:

- semantic image types (`RawFrame`, `SceneFrame`, `RenderedFrame`, `ImageType`);
- a general `ImagingGraph` seed;
- direct reference entry points such as `reconstructSingleRaw()` and `renderReference()`;
- `ImagingBackend`, which currently wraps only single-RAW reconstruction;
- Vulkan kernel wrappers and the synchronous `ComputeRunner`;
- external codec staging/encoding APIs.

Each is useful at its current maturity, but if all of them evolve independently into orchestration APIs, the system will accumulate duplicated policy, inconsistent operation traits, and backend-specific semantics. That would make the codebase harder for both humans and agents to reason about and would weaken differential verification because there would be no single semantic request from which reference and production realizations are derived.

There are already two concrete warning signs:

1. `OperationDescriptor` lets callers provide intrinsic traits such as access pattern, purity, fusion eligibility, temporal status, and reference-domain change. The same semantic operation can therefore be described inconsistently.
2. `OperationKind::GainMapEncode` remains in the early imaging IR even though the implemented Ultra HDR design deliberately keeps gain-map/container semantics at an external codec boundary driven by explicit SDR/HDR renditions.

## Decision

Latent will converge on a **single semantic control plane** with these responsibilities and precedence:

1. **Semantic types and operation schemas define meaning.**
   - Sensor/scene/display/codec domains are explicit.
   - Operation schemas own intrinsic traits and type constraints.
   - Callers provide parameters/intent, not arbitrary claims about purity, access pattern, or domain transitions.

2. **The deterministic FP32 reference implementation is the executable semantic oracle.**
   - It is not a fallback backend whose behavior may drift from the graph.
   - Every optimized/production lowering must remain comparable to equivalent reference semantics.

3. **A graph compiler will translate semantic requests into an explicit execution plan.**
   - Capability selection, fusion, precision/storage choices, resource lifetime, and fallback reasoning belong here.
   - For the same graph + capability/profile/policy inputs, compilation should be deterministic unless a versioned/tuned policy explicitly says otherwise.

4. **Executors execute plans; they do not invent imaging policy.**
   - Vulkan is the primary production compute executor.
   - The current `ComputeRunner` is a transitional correctness harness.
   - High-level convenience APIs such as single-RAW reconstruction may remain, but should become thin builders/adapters over the semantic graph/compiler path rather than independent orchestration stacks.

5. **External codec/container behavior stays at the system edge.**
   - Ultra HDR gain-map generation, ISO metadata, and container behavior remain owned by standards-oriented codec integration unless a later ADR explicitly changes that ownership.
   - Early IR taxonomy that implies otherwise must be reconciled when the compiler IR is promoted.

6. **Evidence is linked to semantic operations and selected lowerings.**
   - Reference/golden/property evidence proves semantics.
   - Differential/integration/lifetime evidence proves realizations.
   - A future `ExecutionTrace` should record selected lowerings, fallbacks, capabilities, precision/storage decisions, metadata selections, and relevant measurements.

## Consequences

Positive consequences:

- There is one obvious place to ask “what does this operation mean?” and one obvious place to ask “why did this backend path run?”
- New agents can navigate from semantic type -> reference behavior -> compiler schema -> lowering -> tests without reconstructing PR history.
- Backend optimization can become more aggressive because semantic equivalence and error budgets remain explicit.
- Burst, multi-pass rendering, and device specialization can be added without multiplying top-level orchestration APIs.
- Capability fallback and performance behavior can become explainable through plans/traces.

Costs and constraints:

- The current `ImagingGraph` API will require schema tightening rather than indefinite additive growth.
- `ImagingBackend` should not be expanded into a second universal interface; existing convenience entry points may need adapters during migration.
- Some duplicated semantic fields across `ImageType`, `SceneFrame`, and `RenderedFrame` should eventually be consolidated.
- The compiler/execution-plan layer must be designed before large asynchronous/burst scheduling work, adding near-term architectural work before some feature work.
- External boundary operations (ingress/export/codec) need clearer graph representation than the first enum set provides.

## Validation

Conformance to this ADR should become testable through the following properties:

- intrinsic operation traits come from canonical schemas and cannot be contradicted by callers;
- graph validation rejects illegal domain/type transitions;
- a fixed semantic graph + fixed capabilities/policy compiles to a deterministic plan/debug representation;
- production Vulkan lowerings remain differentially comparable to reference semantics;
- convenience APIs produce the same semantic graph/outputs as direct graph construction;
- external codec behavior does not alter `SceneFrame` or reconstruction semantics;
- execution/fallback decisions can be inspected in a structured trace once that facility exists.

Until the compiler migration is complete, `docs/architecture.md` and `AGENTS.md` must explicitly label `ImagingGraph`, `ImagingBackend`, and `ComputeRunner` as transitional seeds so their current shapes are not mistaken for permanent boundaries.
