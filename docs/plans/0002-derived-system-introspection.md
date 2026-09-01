# Plan 0002: Derived system introspection and canonical artifacts

Status: Proposed
Owner: repository architecture
Related: ADR-0001, ADR-0002, ADR-0003, ADR-0004, Plan 0001
Current step: Not started; depends first on Plan 0001 Step 1 canonical operation schemas.

## Goal

Make Latent self-describing enough that an agent can query one deterministic, machine-readable projection to discover implemented semantic operations, reference behavior, production lowerings, authority requirements, build features, diagnostics, and evidence relationships without scanning the whole repository.

This is a **cross-cutting P0 slice of the semantic-control-plane work**, not a competing control plane. The introspection surface is derived from the registries that compilation uses; it must never become a separately edited manifest.

## Acceptance criteria

The slice is complete when all of the following are true:

1. A backend-neutral system catalog is deterministically derived from canonical semantic/operation/lowering/diagnostic registrations and current build-feature facts.
2. Every canonical semantic operation appears exactly once with intrinsic schema facts, required authority classes, reference implementation availability, and registered production lowerings/capability predicates where applicable.
3. Static build capability is distinguishable from runtime `DeviceCaps`, observation/profile data, image intent/delegated policy, roadmap maturity, and live GitHub delivery state.
4. Deterministic catalog/request/graph/plan/trace representations have explicit format/schema versioning as those artifacts are introduced.
5. Runtime/scoped identities are not overloaded as deterministic content fingerprints.
6. Compiler/validation/fallback diagnostics expose stable machine-readable codes and structured fields; tests do not need to parse English error strings.
7. Canonical serialization is deterministic for logically identical inputs and suitable for golden comparison/fingerprinting.
8. A small inspection surface can emit the system catalog in a machine-readable form without requiring a Vulkan device or Android runtime.
9. Compiler/plan/trace artifacts eventually record the catalog/schema/compiler fingerprints needed to explain which system definition produced them.
10. CI contains completeness/versioning/determinism tests that fail when an operation/lowering/diagnostic is added inconsistently or becomes invisible to introspection.
11. No checked-in generated manifest duplicates registry truth, and catalog generation never depends on live GitHub state.

## Architectural constraints

- ADR-0001: semantic/control-plane contracts remain authoritative over execution backends.
- ADR-0002: semantic identity/lineage remains separate from physical resources.
- ADR-0003: semantic rules, observations, intent/policy, and capabilities remain distinct authority classes.
- ADR-0004: introspection is a derived projection, not an independently editable source of truth.
- Plan 0001 remains the owner of canonical operation schemas, authority-separated compiler contexts, `ExecutionPlan`, and `ExecutionTrace` migration.
- Keep the introspection model dependency-light and usable in no-Vulkan builds.
- Do not introduce a general runtime-reflection framework merely to expose a small deterministic catalog.
- Do not encode branch names, PR numbers, CI run IDs, source-line numbers, or other volatile delivery facts as semantic/catalog identity.

## Current facts

- `OperationKind` currently enumerates the prototype graph operations, but `OperationDescriptor` also lets callers provide intrinsic traits such as access, fusion, purity, temporal status, and domain-change flags.
- `GraphValidation` currently reports `std::vector<std::string> errors`.
- Vulkan ingress decisions currently include a free-form `std::string reason`.
- semantic descriptors, reference implementations, production Vulkan wrappers, optional codec integrations, CMake features, and tests are discoverable from source, but no single machine-readable registry/catalog ties them together.
- the current CMake/test surface has no introspection executable or catalog test target.
- `DeviceCaps` already provides a useful runtime-capability object and should remain separate from static build/system description.
- roadmap maturity and live GitHub state intentionally remain outside implementation semantics.

## Step 1 — Define identity, version, and fingerprint vocabulary

Before persisting or comparing machine-readable artifacts, define the minimal vocabulary that prevents different concepts from being conflated.

Tasks:

- distinguish scoped runtime identities from canonical content fingerprints;
- define which artifacts need explicit format/schema versions;
- define canonical ordering/normalization rules for deterministic representations;
- choose provisional fingerprint implementation only after the canonical representation is fixed;
- specify mismatch behavior: reject, explicitly migrate, or mark unsupported; never silently reinterpret incompatible semantics.

Validation:

- logically identical inputs canonicalize identically regardless of insertion/container iteration order;
- identities can differ while fingerprints match when the normalized content is equivalent;
- changing a semantic field changes the relevant fingerprint;
- version mismatch behavior is deterministic and explicit.

## Step 2 — Build introspection from Plan 0001 registries

Do not create an independent manifest before the canonical operation-schema work exists.

Tasks:

- expose read-only iteration/query over canonical semantic type/operation registrations;
- register reference implementation identifiers/availability through the same semantic operation definition or a directly linked registry;
- register production lowerings and their build/capability predicates without duplicating operation semantics;
- expose compiled external-integration/build-feature facts needed to explain which lowerings are present.

Validation:

- every operation schema is represented exactly once;
- duplicate/missing operation registrations fail tests or construction;
- every lowering resolves to a legal semantic operation/group;
- no-Vulkan builds produce a valid catalog with Vulkan lowerings correctly absent rather than failing catalog construction.

## Step 3 — Define `SystemCatalog`

Create a backend-neutral derived model that answers static implementation questions cheaply.

Minimum information:

```text
SystemCatalog
  catalog format/version
  semantic/operation registry version + fingerprint
  build feature fingerprint
  semantic type/schema entries
  operation schema entries
    intrinsic traits
    required authority classes
    reference implementation ID/availability
    production lowering entries
      build availability
      capability predicate/requirements
      evidence/equivalence class ID where registered
  diagnostic code registry summary
```

Do not place runtime device feature values in this object. A higher-level inspection view may show `SystemCatalog` beside a `CapabilityContext`, but the separation must remain explicit.

Validation:

- deterministic catalog equality/golden representation for fixed registrations/build options;
- runtime `DeviceCaps` variation does not change the static catalog;
- optional build integrations change only the expected build/lowering entries and fingerprints.

## Step 4 — Introduce structured diagnostics

Start with graph validation/compiler-facing diagnostics and migrate edge planners as useful.

Minimum conceptual fields:

```text
Diagnostic
  code
  severity
  category/stage
  semantic operation/value/resource IDs when applicable
  structured context fields
  human-readable message
```

Tasks:

- define a compact diagnostic-code namespace and uniqueness rule;
- preserve readable messages as views;
- avoid binding diagnostic identity to source file/line or English wording;
- provide compatibility adapters where current APIs expose strings.

Validation:

- tests assert codes/fields for semantic failures and fallback reasons;
- codes are unique;
- wording changes do not break machine-facing tests;
- current public behavior can be adapted without a flag-day rewrite.

## Step 5 — Canonical machine-readable serialization

Provide one deterministic serialization for catalog/debug/control artifacts that need external inspection or golden tests.

Tasks:

- select a simple representation with explicit format version;
- define canonical field order/set ordering/number representation rules rather than relying on incidental serializer behavior;
- keep human pretty-printing as a separate view where useful;
- make fingerprints derive from canonical logical content, not whitespace or pretty formatting.

Validation:

- repeated serialization is byte-stable for identical canonical content;
- round-trip tests preserve supported fields where parsing is implemented;
- pretty-print changes do not alter canonical fingerprints.

## Step 6 — Add a bounded inspection executable/API

Once the catalog exists, add a small dependency-light surface for agents and developers.

Target capabilities, exact command names provisional:

- dump static `SystemCatalog` as canonical/pretty machine-readable data;
- optionally show runtime capability facts as a separate section when a runtime is available;
- later dump/inspect normalized graph, `ExecutionPlan`, and `ExecutionTrace` through the same serialization/diagnostic conventions.

The tool must not become another orchestration API or policy engine.

Validation:

- catalog dump works in no-Vulkan CI;
- output identifies format/catalog/schema versions and fingerprints;
- runtime-unavailable cases remain explicit rather than inventing capability values.

## Step 7 — Link plans and traces to the system definition

As Plan 0001 lands `ExecutionPlan` and `ExecutionTrace`, include enough version/fingerprint context to know which semantic/compiler definition produced the artifact.

Tasks:

- include registry/catalog/compiler format fingerprints as appropriate;
- keep run IDs/semantic IDs distinct from content fingerprints;
- make trace explanations reference structured diagnostic/fallback codes;
- define compatibility checks before consuming persisted artifacts from another build/version.

Validation:

- two plans from identical normalized inputs/system definitions compare identically in deterministic fields;
- a changed schema/catalog is visible even when a request is textually similar;
- incompatible persisted artifacts fail with structured diagnostics.

## Step 8 — Make introspection part of change impact and CI

Adding a semantic operation or production lowering should automatically expand the system catalog and corresponding completeness tests.

Required checks should include:

- operation-schema catalog completeness;
- lowering-to-operation referential integrity;
- diagnostic-code uniqueness;
- deterministic catalog/canonical serialization;
- expected fingerprint changes for semantic registry changes;
- no-Vulkan catalog build/query;
- optional external integration visibility where enabled.

Prefer generated CI artifacts or on-demand catalog dumps over committing generated manifests to source control.

## Migration strategy

- Do not block Plan 0001 Step 1 on a polished CLI; get canonical schemas right first.
- Add queryability to the same registries rather than copying them into a second registry.
- Introduce diagnostic records behind compatibility adapters before deleting string-based fields.
- Add versions before treating serialized artifacts as durable interchange formats.
- Keep catalog serialization/testing backend-neutral; runtime probes remain separate tests.
- Integrate plan/trace fingerprints incrementally as those artifacts become real.

## Repository operating guardrails related to agent use

These are repository-process rules, not `SystemCatalog` data:

- start new feature/documentation work from current `main` unless an active plan explicitly documents an intentional stacked base;
- a branch that is fully merged (`ahead=0` relative to `main`) is historical evidence, not a base for new work;
- query current GitHub state before branch/PR decisions rather than reading a dated snapshot;
- merge only the reviewed latest PR head after the applicable latest-SHA CI succeeds;
- prefer machine-enforced repository rules/required checks when the repository owner authorizes them; until then the agent must enforce the gate explicitly in its workflow.

Historical branches need not be deleted merely to satisfy this plan; branch deletion is a separate repository-maintenance action.

## Validation matrix

This plan ultimately adds to the existing matrix rather than replacing it:

```text
default build/test
no-Vulkan build/test
optional codec integration where catalog build-features are involved
catalog completeness tests
canonical serialization/fingerprint tests
diagnostic-code/field tests
inspection executable smoke tests
```

Catalog/introspection tests should not require an Android device and should not require a Vulkan device unless explicitly querying a separate runtime capability view.

## Status ledger

- Completed: architectural problem identified from current graph/diagnostic/build surfaces.
- Completed: ADR-0004 records the derived-view/versioning boundary.
- Current: plan defined; no implementation API is claimed to exist.
- Remaining: Steps 1-8.

## Risks and open questions

- Exact C++ names (`SystemCatalog`, fingerprint types, diagnostic record types) are intentionally provisional until Plan 0001 schema work defines ownership cleanly.
- A catalog can become over-detailed and expensive; expose information that reduces agent/compiler ambiguity, not every source-level implementation detail.
- Evidence IDs should be stable enough for introspection without making test file paths part of semantic ABI.
- Persisted artifact compatibility policy should remain modest before 1.0; explicit version rejection is preferable to premature long-term compatibility machinery.
- Hash algorithm choice is secondary to canonical logical representation and should not be frozen prematurely.
