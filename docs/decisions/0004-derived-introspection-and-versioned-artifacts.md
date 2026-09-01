# ADR-0004: Derived system introspection and versioned machine-readable artifacts

Status: Accepted
Date: 2026-09-01

## Context

Latent is being shaped so an agent can understand and control the whole imaging system without repeatedly reconstructing it from headers, enum values, kernel wrappers, CMake options, tests, and PR history.

`ExecutionPlan` and `ExecutionTrace` address one compiled request/run, but they do not answer a different high-value question: **what does this build of Latent know how to represent and execute at all?** Today that static capability picture is distributed across semantic types, `OperationKind`, caller-filled graph descriptors, reference functions, Vulkan wrappers, optional codec build features, tests, and documentation.

A hand-maintained YAML/JSON manifest would reduce search cost initially but would create a second source of truth that can drift from the implementation. Free-form diagnostic strings have a similar problem: they are useful to humans but fragile as control data for agents. Future plans/traces also need to remain comparable across schema/compiler evolution without conflating runtime identities with deterministic content fingerprints.

The architecture therefore needs a machine-readable introspection surface, but that surface must be a **derived projection of canonical registries**, not another independently editable architecture database.

## Decision

### 1. Canonical registries remain authoritative

The eventual canonical sources of implemented system truth are the registries/contracts that compilation itself consumes, including as they are introduced:

- canonical semantic type descriptors/constructors;
- the operation-schema registry;
- registered reference implementation identifiers;
- registered production lowering identifiers and capability predicates;
- structured diagnostic definitions;
- build-feature/dependency facts needed to know which integrations are compiled in.

No separate hand-edited manifest may redefine those facts.

### 2. `SystemCatalog` is a deterministic derived view

Latent will expose a backend-neutral, read-only system catalog (conceptually `SystemCatalog`; exact C++ naming may differ) derived from the canonical registries and build configuration.

The catalog should make at least these relations machine-queryable:

- semantic type/schema identifiers and relevant domain constraints;
- operation identifiers, arity/domain/type rules, intrinsic access/fusion/precision facts, and required authority-class inputs;
- deterministic reference implementation availability/identifier;
- production lowering identifiers, build availability, and capability requirements;
- external integration/build-feature availability;
- registered diagnostic codes/categories;
- evidence/equivalence class identifiers where the implementation has such registrations;
- catalog/schema/compiler format versions and deterministic registry fingerprints.

The catalog is a projection. If it disagrees with the registry used by the compiler, the implementation is wrong; the catalog does not win by being easier to read.

### 3. Static implementation capability, runtime capability, roadmap maturity, and delivery state stay separate

The catalog describes **what this build contains**. It must not absorb unrelated authorities:

- runtime/device facts remain in `CapabilityContext`/`DeviceCaps` or equivalent runtime snapshots;
- observations/capture/profile evidence remain observation inputs;
- fixed/delegated image intent remains request/policy input;
- roadmap maturity and future priorities remain documentation/planning facts;
- PR/branch/SHA/review/CI state remains live GitHub state.

A query tool may present these views together, but their data models and authority must remain distinguishable.

### 4. Machine-readable artifacts are versioned and canonically fingerprintable

Deterministic portions of the control plane must have explicit format/schema versioning and canonical representations suitable for comparison and hashing.

At minimum the design must distinguish versions/fingerprints for the relevant layers as they appear:

- semantic/operation registry schema;
- derived system catalog format;
- normalized semantic request/graph representation;
- compiler/plan format;
- trace format.

Canonicalization rules must make the same logical input produce the same canonical representation independent of incidental container iteration order or human formatting.

A semantic or format incompatibility must be explicit. Old plan/trace/catalog data must not be silently reinterpreted under changed semantics simply because field names still parse.

### 5. Runtime identities and content fingerprints are different concepts

Typed IDs answer questions such as “which value/resource/run is this inside this graph or execution?” Deterministic fingerprints answer “is this normalized content/configuration/schema the same as another one?”

Do not overload one mechanism for both jobs.

Examples:

- `SemanticValueId`, `PlanResourceId`, `RunId` may be scoped identities;
- `GraphFingerprint`, `CatalogFingerprint`, `PlanFingerprint`, or equivalent digests represent canonical content.

Exact names and hash algorithms are implementation details until the corresponding schema is defined, but the conceptual separation is architectural.

### 6. Diagnostics are structured data first

Compiler/validation/fallback diagnostics should evolve from free-form strings toward records with stable machine-readable identity, for example:

```text
Diagnostic
  code
  severity
  stage/category
  subject IDs where applicable
  structured context fields
  human message
```

Agents and tests should key on diagnostic code/fields, not parse English message text. Human-readable messages remain important views and may evolve more freely than the code/field contract.

Before 1.0, diagnostic/version compatibility may evolve, but changes must be intentional, versioned where necessary, and test-visible rather than accidental.

### 7. Introspection must be cheap to consume

The implementation should eventually provide both a library-level query and a deterministic machine-readable presentation. A small inspection executable is preferred once the registries exist; conceptual commands include catalog dump, plan dump, and trace explanation, but exact CLI spelling is not fixed by this ADR.

An agent should be able to answer “what operations/lowerings/evidence are available in this build?” without scanning the entire source tree.

## Consequences

Positive:

- repository-wide capability discovery becomes one bounded query rather than repeated source archaeology;
- agents can compare two builds/requests/plans/traces through explicit versions and fingerprints;
- change-impact reasoning becomes easier because operations, lowerings, authority requirements, and evidence classes are linked through canonical registrations;
- diagnostic handling becomes robust to human-message wording changes;
- CI can assert catalog completeness and prevent new operations/lowerings from becoming invisible to introspection;
- generated views can be emitted as CI artifacts without checking stale generated manifests into source control.

Costs/constraints:

- canonical registries need enough metadata to support introspection without becoming dependency-heavy reflection frameworks;
- canonical serialization and versioning require discipline before persisted plan/trace data is treated as stable;
- catalog completeness tests become a required part of adding semantic operations or production lowerings;
- source file paths, PR numbers, and other volatile delivery details should not be treated as stable catalog identity.

## Validation

Implementations conform to this decision when tests can demonstrate that:

1. every supported canonical operation schema appears exactly once in the derived catalog;
2. every registered production lowering references a legal semantic operation/group and explicit capability/build requirements;
3. catalog output is deterministic for an identical build/registry input;
4. changing a semantic schema or registered lowering changes the appropriate canonical fingerprint/version-visible data;
5. runtime `DeviceCaps` changes do not mutate the static catalog for the same build, though a combined query may show both views separately;
6. diagnostic codes are unique and tests assert codes/fields instead of parsing human messages;
7. incompatible artifact/schema versions are rejected or explicitly migrated rather than silently accepted;
8. no catalog generation step requires live GitHub state;
9. generated catalog/debug artifacts are reproducible and need not be committed as manually maintained truth.
