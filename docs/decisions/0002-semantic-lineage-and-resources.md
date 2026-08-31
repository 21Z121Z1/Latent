# ADR-0002: Separate semantic identity and lineage from physical resource storage

Status: Accepted
Date: 2026-09-01

## Context

The first implementation slices intentionally use host-friendly frame structs:

- `RawFrame` owns canonical `std::vector<uint16_t>` storage;
- `SceneFrame` owns an FP32 RGB vector;
- `RenderedFrame` owns an FP32 encoded RGB vector;
- `SceneFrame`, `RenderedFrame`, and `UltraHdrRenditionPair` propagate a scalar `sourceRawId`.

Those shapes are excellent for deterministic reference code and tests, but they become dangerous if treated as the universal production resource model.

Two future requirements expose the mismatch:

1. Vulkan/AHardwareBuffer/device-local execution needs backend-specific resource bindings, lifetimes, aliasing, synchronization, and storage formats without polluting image meaning with platform handles.
2. Burst/multi-frame reconstruction produces values with many input frames and intermediate derivations. A scalar `sourceRawId` cannot represent that lineage without becoming ambiguous or overloaded.

Embedding Vulkan/AHardwareBuffer handles into semantic frame types, or reusing `sourceRawId` as a burst/scene/run identifier, would make the semantic layer depend on execution details and would destroy a clean provenance model.

## Decision

Latent will keep **semantic identity/lineage** and **physical resource storage** as separate architectural concerns.

1. Semantic values will have typed/stable identities appropriate to their domain, such as capture/frame, burst, scene, rendition, plan, and run identities. Exact C++ names are not fixed by this ADR.
2. Multi-source derivations will use explicit lineage/source sets or a provenance graph, not overloaded scalar raw IDs.
3. Semantic descriptors own image meaning: domain, colorimetry, range, precision class/requirements, extent, metadata provenance, and lineage.
4. Physical storage belongs to an execution/reference binding:
   - reference adapters may continue to own CPU vectors for determinism and testability;
   - production `ExecutionPlan` resources may bind Vulkan buffers/images, AHardwareBuffer imports, staging memory, or other backend storage;
   - platform handles must not become part of the semantic definition of `RawFrame`, `SceneFrame`, or display renditions.
5. Conversions between host/reference containers and execution resources are explicit ingress/egress/binding operations, not implicit mutations of semantic frame types.
6. `ExecutionTrace` should connect semantic IDs/lineage to selected resource bindings and execution IDs so an agent can follow both “where this image came from” and “where/how it was stored and processed.”

## Consequences

Positive consequences:

- burst lineage can be represented without breaking single-frame IDs;
- reference and production backends can share semantics while using completely different storage;
- zero-copy/device-local optimizations cannot accidentally redefine frame contracts;
- resource lifetime/aliasing becomes compiler/executor data rather than application-visible image meaning;
- traces can separately explain semantic provenance and physical execution decisions;
- agents need not infer whether a field is “image meaning” or “where the bytes happen to live.”

Costs:

- future compiler work needs a resource-binding model in addition to semantic values;
- current convenience structs may require adapters/views when the universal control plane is introduced;
- provenance/lineage will need new types rather than continuing to pass a single integer through every layer;
- migration must preserve simple host/reference APIs for tests without making them the production storage abstraction.

## Validation

Conformance should eventually be provable by tests that:

- compile the same semantic graph to different storage/backends without changing semantic descriptors;
- show host/reference and Vulkan resources map to the same semantic value IDs;
- preserve explicit multi-frame lineage through burst -> scene -> rendition -> codec outputs;
- reject or avoid backend handles inside semantic descriptors;
- verify `ExecutionTrace` can correlate semantic lineage with plan resources and executed dispatches;
- keep deterministic reference tests independent of Vulkan/AHardwareBuffer availability.
