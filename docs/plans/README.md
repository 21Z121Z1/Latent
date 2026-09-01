# Execution plans

This directory stores persistent implementation plans for work that spans multiple architectural steps, PRs, or validation environments.

A plan is not a substitute for an ADR or the roadmap:

- `docs/architecture.md` defines stable system structure;
- `docs/roadmap.md` defines priority and maturity;
- `docs/decisions/` records durable decisions;
- `docs/plans/` records how a specific multi-step change will be executed and verified.

Use a plan when an agent would otherwise need to reconstruct important work state from chat history, scratch notes, or several PR descriptions.

## Plan status and top metadata

Keep the first metadata lines cheap to parse. Every plan should expose at least:

```text
Status: Proposed | Active | Blocked | Completed | Superseded by Plan NNNN
Owner: ...
Related: ADR/plan identifiers
Current step: one concise factual line
```

Status meanings:

- **Proposed** — sequencing/acceptance criteria exist, but implementation has not started.
- **Active** — implementation is underway; `Current step` identifies the active dependency/step.
- **Blocked** — work intentionally cannot proceed; the blocking condition must be named.
- **Completed** — acceptance criteria are satisfied and durable outcomes have been reflected in architecture/roadmap/ADRs as needed.
- **Superseded** — another plan now owns the unfinished migration state.

Do not infer active work merely because a branch exists. Query GitHub for live delivery state and use the plan only for durable multi-step migration state.

## Required sections

Each active or proposed plan should contain:

1. **Goal** — one concrete outcome.
2. **Acceptance criteria** — observable completion conditions.
3. **Architectural constraints** — invariants/ADRs that must remain true.
4. **Current facts** — relevant existing APIs/tests and implementation facts, with no speculative claims.
5. **Steps** — ordered, dependency-aware increments small enough to validate independently.
6. **Validation** — exact test/CI evidence for each increment.
7. **Migration/compatibility** — how old APIs/data survive or are retired.
8. **Status ledger** — concise completed/current/remaining state.
9. **Risks/open questions** — only unresolved items that can materially change implementation.

## Maintenance rules

- Update the plan when the implementation changes direction; do not leave a known-stale plan as “documentation.”
- Keep `Status` and `Current step` synchronized with the status ledger so an agent can orient without reading the whole file.
- Keep plans factual. Mark proposed names/interfaces as proposed until code lands.
- Prefer links to architecture/ADRs/tests instead of copying long invariant lists.
- Do not copy current PR numbers, head SHAs, CI run IDs, branch counts, or review state into a plan unless the branch/stack itself is an active migration dependency; query GitHub for volatile state.
- Start new work from current `main` unless an Active plan explicitly documents an intentional stacked base.
- A fully merged branch (`ahead=0` relative to `main`) is historical evidence, not a base for new work.
- When the work completes, mark the plan complete and move durable outcomes into architecture/roadmap/ADRs as appropriate.
- A completed plan may remain as implementation history, but it must not become the authority for current architecture.
