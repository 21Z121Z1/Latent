# Plan 0003: Temporal RAW reconstruction and Android capture

Status: Active
Related: ADR-0001 through ADR-0004; Plans 0001 and 0002
Current step: Implement deterministic temporal reference stages after the contract slice.

## Scope and authority

Implement one clean-room temporal RAW semantic path. Retain the deterministic
C++ reference and the existing Vulkan backend. Do not create a second backend
solely to add Rust. Kotlin and Compose own Android UI and Camera2 integration.

The necessary P0 slice is typed burst identity, multi-source lineage, canonical
temporal operations, separate physical bindings, and a versioned temporal
execution plan and trace. This work does not claim to finish Plans 0001 or 0002.

## Incremental work

1. Verify baseline and establish reproducible build inputs.
2. Define the burst contract, lineage, authority-separated inputs, and ADR-0005.
3. Implement deterministic fixtures, selection, radiometry, and alignment.
4. Implement CFA-safe robust fusion, conditional uncertainty, and reconstruction.
5. Prove synthetic properties before adding Vulkan lowerings and differential tests.
6. Build the Android app, native replay, Camera2 capture, and transactional output.
7. Run applicable CI gates on the latest head and review the complete diff.

## Acceptance

Reference gates cover validation, lineage, N=1 equivalence, all Bayer layouts,
odd borders, sub-black values, radiometric scale and variance, translations,
local motion, clipping, deterministic replay, and Monte Carlo calibration.
Vulkan gates require real software-driver dispatch, explicit error budgets,
finite/sign preservation, and repeated execution. Android gates require build,
lint, unit tests, native loading, fixture reconstruction, and UI integration.
No camera or performance claim may be inferred from emulator or lavapipe tests.

## Verification ledger

| Requirement | Environment | Evidence | Status | Source |
| --- | --- | --- | --- | --- |
| Unchanged default strict build/tests | GitHub Ubuntu 24.04, GCC 13.3, Mesa | Run 33457999246, job 106054161632, 4/4 CTest; full log inspected | VERIFIED | 1939cb424deef1272602d2b565bd3ee3c6349f51 |
| Unchanged no-Vulkan build/tests | Same GitHub runner | Same job, 3/3 CTest | VERIFIED | Same baseline |
| Isolated libultrahdr integration | Same GitHub runner, libultrahdr 2.0.2 | Same job, 3/3 CTest | VERIFIED | Same baseline |
| Typed burst, borrowed normalization, lineage, noise units | Editing container, strict no-Vulkan build | 4/4 CTest, including temporal contract tests; CI pending | PARTIALLY VERIFIED | Contract increment |
| Temporal algorithms and Vulkan | Not implemented yet | No evidence yet | UNVERIFIED | Not applicable |
| Android integration | Not implemented yet | No evidence yet | UNVERIFIED | Not applicable |
| Real RAW capture, metadata, IMU, OEM behavior | Physical Android device required | No device connected | UNVERIFIED | Not applicable |
| Mobile latency, thermals, energy, memory traffic | Physical Android device required | No device connected | UNVERIFIED | Not applicable |

## Current constraints and clean-room record

The editing container cannot resolve GitHub and has no Android SDK or Vulkan
ICD. The GitHub connector supports writes and Actions executes native builds.
CI reproduction artifacts contain source archives and Git objects, not local
Git configuration or credentials. The first artifact was downloaded, its SHA-256
and internal checksums were verified, and the unchanged main no-Vulkan baseline
passed 3/3 CTest in the editing container. The first branch CI also passed all
existing gates with a required lavapipe probe. Native temporal gates are next.

The uploaded archive was inspected only at the directory-list level. Its private
reverse-engineering, disassembly, model, and tuning content is excluded from
algorithm specifications and implementation. Public papers/specifications and
independently generated experiments are the permitted algorithm evidence.
