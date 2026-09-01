# ADR-0003: Separate semantic rules, observations, intent, and execution capabilities

Status: Accepted
Date: 2026-09-01

## Context

Latent increasingly combines several kinds of information that look similar in C++ because they are all fields or configuration values, but they have different authority and mutability:

- semantic invariants such as reference-domain rules and `SceneFrame` meaning;
- observations such as Camera2 metadata, calibration, measured black levels, noise models, and device profiles;
- requested image intent such as reconstruction choices, render exposure, output targets, and export requests;
- execution facts such as Vulkan capabilities, Android/platform availability, memory/import support, and measured device behavior;
- optimization policy such as quality/latency/power trade-offs.

If these classes are not explicit, a caller or backend can accidentally make a capability look like image semantics, make a fallback look like captured fact, or let a performance heuristic silently change the requested image. This is especially costly for an agent: it must rediscover from control flow which values are facts, which may be chosen, which may be approximated, and which are invariant.

The current repository already demonstrates the desired direction in several places: metadata has source/validity/confidence, `DeviceCaps` is separate from image types, `renderExposureEV` is kept separate from `sceneScaleEV`, and optional Vulkan features select fast paths rather than redefine correctness. The control plane should generalize this separation.

## Decision

Latent will classify control-plane inputs by authority. The conceptual classes are:

1. **Semantic rules** — versioned project invariants and operation schemas. Examples: legal reference-domain transitions, `SceneFrame` semantics, intrinsic operation traits, precision/error requirements that define correctness.
2. **Observations and evidence** — facts learned from capture results, calibration, recorded fixtures, measurement, or a versioned device profile. They carry provenance/validity/confidence where uncertainty matters.
3. **Image intent and policy** — choices requested by the application/user or explicitly delegated to Latent. Examples: requested outputs, render intent/exposure, quality goals, and whether an algorithmic choice is fixed or may be selected automatically.
4. **Execution capabilities** — facts about what a runtime/platform/device can execute. Examples: Vulkan version/features, AHardwareBuffer import support, codec availability, supported formats, measured traffic/timing properties.

These classes have directional authority rules:

- execution capabilities may select a lowering or reject an unsupported request, but may not silently redefine semantic meaning;
- optimization policy may choose only among alternatives explicitly permitted by the semantic request and its error/quality contract;
- policy defaults may not masquerade as observations; observation fallback must remain identifiable and traceable;
- observations may influence semantic parameters through explicit, validated selection rules, but may not mutate project invariants;
- changing a semantic invariant is an architecture/version decision, not a device-specific fallback;
- when an approximation changes observable image semantics beyond an existing equivalence budget, it must be represented as a different requested/allowed semantic choice, not hidden inside lowering.

The target compiler interface should therefore make these categories inspectable rather than accepting one undifferentiated configuration blob. Exact C++ names are deferred, but conceptually compilation consumes something equivalent to:

```text
SemanticRequest / SemanticGraph
        + ObservationContext
        + IntentPolicy
        + CapabilityContext
        -> GraphCompiler
        -> ExecutionPlan
```

`ExecutionPlan` must record the decisions that depend on policy/capability inputs, while `ExecutionTrace` must record the concrete observations, fallbacks, profile/capability facts, and selected lowerings that explain a run.

A useful distinction inside intent is **fixed semantic intent vs delegated policy**. For example, an explicit `renderExposureEV` is part of the requested rendition and cannot be changed because a GPU is slow. A request that says “choose a denoiser within quality class Q under latency budget L” explicitly delegates a choice to policy/compiler logic and that choice must be traceable.

## Consequences

### Positive

- Agents can determine whether a value is immutable truth, observed evidence, requested intent, or execution constraint without reconstructing hidden control flow.
- Device specialization becomes safer because capabilities only affect legal lowerings unless the request explicitly permits semantic variation.
- Fallback behavior becomes auditable: an estimated/calibrated value cannot silently become indistinguishable from capture metadata.
- `ProcessingRequest`, compiler APIs, plans, and traces gain a natural decomposition that scales to Android capture, bursts, rendering, and device profiles.
- Reproducibility improves because a result can be tied to semantic request + observation/profile versions + policy + capabilities rather than an opaque configuration object.

### Costs

- Some current structs, especially `ReconstructionConfig`, mix parameters from more than one authority class and will need adapters or decomposition as the control plane is promoted.
- Compiler/test fixtures will need explicit contexts rather than a few loosely related booleans/scalars.
- Automatic algorithms must state what choice was delegated and what evidence/policy selected it.

## Validation

Implementations conform when:

- compiler tests can vary `DeviceCaps` and obtain different legal plans without changing the semantic graph/output contract;
- changing an observation/fallback source is visible in plan/trace diagnostics and does not masquerade as a fixed semantic constant;
- fixed image intent is preserved across reference/Vulkan lowerings within the declared error budget;
- delegated policy choices are explicit, deterministic for the same context/profile, and recorded with reasons;
- unsupported capability cases fail or fall back explicitly instead of silently changing image semantics;
- architecture and API tests make invalid cross-authority substitutions difficult or impossible to construct.
