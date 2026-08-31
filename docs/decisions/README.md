# Architecture decision records

This directory preserves durable architectural decisions that must outlive branch names and PR descriptions.

Use an ADR when a change alters a long-lived system boundary or invariant, including:

- reference-domain ownership or transitions;
- the definition of core semantic objects such as `SceneFrame` or future `RawBurst`;
- graph/compiler/executor responsibilities;
- precision and error-budget policy;
- metadata provenance precedence;
- ownership of render, gamut, gain-map, or codec behavior;
- Vulkan baseline/fast-path policy;
- external dependency isolation;
- device-profile or heuristic reproducibility rules.

Do not create ADRs for ordinary implementation details, refactors that preserve boundaries, or temporary branch sequencing.

## Lifecycle

Use these statuses:

- `Proposed`
- `Accepted`
- `Superseded by ADR-NNNN`
- `Rejected`

Accepted ADRs are historical records. Do not rewrite an accepted decision to make history look cleaner; create a new ADR that supersedes it and link both directions.

## Minimal format

```text
# ADR-NNNN: Title

Status: Proposed | Accepted | Superseded by ... | Rejected
Date: YYYY-MM-DD

## Context
What durable problem or ambiguity exists?

## Decision
What boundary/rule is being chosen?

## Consequences
What becomes easier/harder? What must downstream work obey?

## Validation
What evidence would prove implementations conform to this decision?
```

The architecture document should summarize accepted decisions that define the current system, while the ADR retains rationale and supersession history.
