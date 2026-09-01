# Execution plans

This directory stores persistent implementation plans for work that spans multiple architectural steps, PRs, or validation environments.

A plan is not a substitute for an ADR or the roadmap:

- `docs/architecture.md` defines stable system structure;
- `docs/roadmap.md` defines priority and maturity;
- `docs/decisions/` records durable decisions;
- `docs/plans/` records how a specific multi-step change will be executed and verified.

Use a plan when an agent would otherwise need to reconstruct important work state from chat history, scratch notes, or several PR descriptions.

## Required sections

Each active plan should contain:

1. **Goal** — one concrete outcome.
2. **Acceptance criteria** — observable completion conditions.
3. **Architectural constraints** — invariants/ADRs that must remain true.
4. **Current facts** — relevant existing APIs/tests/branches, with no speculative claims.
5. **Steps** — ordered, dependency-aware increments small enough to validate independently.
6. **Validation** — exact test/CI evidence for each increment.
7. **Migration/compatibility** — how old APIs/data survive or are retired.
8. **Status ledger** — concise completed/current/remaining state.
9. **Risks/open questions** — only unresolved items that can materially change implementation.

## Maintenance rules

- Update the plan when the implementation changes direction; do not leave a known-stale plan as “documentation.”
- Keep plans factual. Mark proposed names/interfaces as proposed until code lands.
- Prefer links to architecture/ADRs/tests instead of copying long invariant lists.
- When the work completes, mark the plan complete and move durable outcomes into architecture/roadmap/ADRs as appropriate.
- A completed plan may remain as implementation history, but it must not become the authority for current architecture.
