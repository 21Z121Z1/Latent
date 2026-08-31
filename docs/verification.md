# Verification and evidence model

Latent permits optimized/fused/device-specific implementations only because correctness evidence is part of the architecture. This document defines the current validation ladder and what evidence a change is expected to provide.

## 1. Evidence hierarchy

Use the strongest applicable evidence, in this order:

1. **Semantic/type validation** — illegal image/reference-domain states are rejected.
2. **Golden vectors** — published standards, official reference implementations, or hand-verifiable cases pin constants and algorithms.
3. **Property tests** — invariants hold across generated/representative inputs.
4. **Reference composition tests** — independently implemented stages compose to the same result as a higher-level reference path.
5. **Differential tests** — Vulkan/optimized output matches the deterministic reference within an explicit numerical budget.
6. **Integration tests** — external libraries/platform boundaries produce valid real artifacts/queries.
7. **Lifetime/stress tests** — repeated execution proves resource ownership/reuse assumptions.
8. **Device measurements** — performance/zero-copy/power claims require actual measured device evidence, not successful API calls.

No single test class substitutes for the others. For example, a Vulkan shader matching itself on two drivers is not a semantic oracle; a reference algorithm with no production differential evidence is not proof of a Vulkan lowering.

## 2. Standard local validation ladder

### Default configuration

```bash
cmake -S . -B build \
  -DLATENT_BUILD_TESTS=ON \
  -DLATENT_STRICT_WARNINGS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure --verbose
```

### No-Vulkan configuration

This proves the semantic/reference core does not accidentally depend on the runtime backend:

```bash
cmake -S . -B build-novk \
  -DLATENT_BUILD_TESTS=ON \
  -DLATENT_ENABLE_VULKAN_RUNTIME=OFF
cmake --build build-novk --parallel
ctest --test-dir build-novk --output-on-failure
```

### Optional libultrahdr integration

Requires an external libultrahdr 2.x installation:

```bash
cmake -S . -B build-uhdr \
  -DLATENT_BUILD_TESTS=ON \
  -DLATENT_ENABLE_VULKAN_RUNTIME=OFF \
  -DLATENT_ENABLE_ULTRAHDR=ON \
  -DLATENT_ULTRAHDR_ROOT=/path/to/libultrahdr/install
cmake --build build-uhdr --parallel
ctest --test-dir build-uhdr --output-on-failure --verbose
```

The GitHub Actions workflow independently builds libultrahdr v2.0.2 for the integration configuration so its compiler flags remain outside the deterministic core target.

## 3. Current test executables

| Test executable | Main responsibility |
| --- | --- |
| `latent_tests` | imaging types, color science, RAW packing, reconstruction, noise/correction, graph validation/fusion, Vulkan runtime/kernel differential coverage when enabled |
| `latent_render_tests` | scene analysis, transfer functions, ACES-derived scalar tonescale, reference rendering semantics, scene-scale/render-exposure separation |
| `latent_codec_tests` | rendition validation/packing and, when enabled, real libultrahdr encode/probe integration |
| `latent_compute_runner_lifetime_tests` | repeated synchronous Vulkan dispatch/resource recycling beyond descriptor-pool limits |

CTest is the canonical invocation surface. Do not rely on manually running one binary when CI exercises several configurations.

## 4. Change-to-evidence routing

### Semantic types or domain rules

Required:

- construction/validation tests for legal and illegal states;
- tests for every changed reference-domain boundary;
- update architecture/ADR if the meaning itself changes.

### Camera metadata, calibration, color science, or reference reconstruction

Required:

- golden or analytically checkable vectors where possible;
- property tests for invariants/fallbacks;
- end-to-end reference composition test when stage ordering changes;
- provenance/fallback selection tests.

### Noise/uncertainty

Required:

- analytic special cases;
- property checks on positivity/finite behavior as applicable;
- Monte Carlo validation when closed-form propagation is not sufficient to expose composition mistakes.

### Graph/compiler work

Required:

- illegal graph/state rejection;
- deterministic plan/fusion behavior for a fixed graph/capability input;
- tests that semantic traits cannot be contradicted by caller annotations once schema work lands;
- explicit tests for external/temporal/reduction barriers;
- plan/trace round-trip or stable debug representation if serialization/debug output is introduced.

### Vulkan kernels or optimized lowerings

Required:

- a semantic-equivalent reference operation already exists or is added in the same change;
- test matrix spans relevant CFA/layout/extent/mode boundaries;
- zero NaN/Inf gates where the reference domain expects finite values;
- sign/unbounded preservation where required;
- numerical tolerance budget justified by precision/storage boundaries;
- differential test runs on a real Vulkan software/driver path in CI where feasible;
- no cross-driver bit-exact claim unless the contract truly requires it.

### Executor/resource lifetime work

Required:

- success path plus repeated/stress execution;
- failure cleanup paths for newly owned resources;
- bounds/null/zero-size checks where host transfers are exposed;
- CI against at least the existing lavapipe/runtime path;
- future asynchronous changes must test multi-frame lifetime and synchronization ordering, not just single-dispatch success.

### Rendering

Required:

- `sceneScaleEV` invariance;
- independent SDR/HDR intent;
- `renderExposureEV` separation;
- target primaries/white/transfer semantics;
- negative/out-of-gamut handling policy;
- explicit checks that the scene master is not mutated;
- production Vulkan rendering must be differentially tested against the reference renderer.

### Codec/export

Required:

- exact rendition semantic validation before packing;
- packed-layout tests;
- external library integration test for real output when the dependency is enabled;
- metadata/container probe, not only successful function return;
- HEIF/AVIF support is not considered validated until a real encode/probe path is exercised with the required platform/dependency stack.

## 5. Numerical evidence rules

A numerical gate must state what error source it permits.

Examples already present in the repository:

- FP16 storage boundaries permit small quantization differences while preserving sign/unbounded semantics;
- MHC/color cancellation-sensitive operations accumulate in FP32;
- reference builds disable GNU FP contraction so the oracle does not drift with compiler FMA choices;
- cross-driver Vulkan tests use tolerance budgets rather than assuming identical last bits.

When adding a tolerance:

1. identify the expected source of error;
2. measure observed distributions on representative cases;
3. choose a gate with justified headroom;
4. separately gate catastrophic failures such as NaN, sign inversion, or domain clipping;
5. document the precision boundary that would require revisiting the budget.

Do not loosen a gate just to obtain green CI without locating the root cause.

## 6. CI as a second environment

GitHub Actions is not merely a repeat of local testing. It provides a distinct compiler/runtime/dependency environment.

Current CI exercises:

- Ubuntu 24.04 build with strict warnings;
- default Vulkan-enabled configuration with Mesa lavapipe when installation succeeds;
- no-Vulkan configuration;
- isolated external libultrahdr 2.0.2 build;
- Ultra HDR integration configuration and tests.

When a PR changes code, inspect the workflow attached to the **latest head SHA**, then inspect failed jobs/steps rather than relying on an older green run.

For documentation-only changes, CI is still useful because it proves the branch remains buildable and catches accidental file/build edits. A documentation change should not claim code correctness beyond the unchanged head it is based on.

## 7. Evidence expected in PR descriptions

A good PR should make verification reconstructible without reading its whole diff:

- semantic layer(s) changed;
- invariant/contract changed or explicitly unchanged;
- reference evidence added/used;
- production lowering added/used;
- exact test/configuration commands or CI jobs run;
- known boundaries that remain intentionally unimplemented;
- ADR/roadmap updates when architectural maturity or ownership changes.

Avoid statements such as “100% correct.” State what was actually verified and what remains platform- or device-dependent.

## 8. Future execution-trace verification

When `ExecutionTrace` exists, tests should be able to assert not only pixel output but also control decisions:

- selected metadata fallback;
- selected lowering/backend;
- capability gate/fallback reason;
- precision/storage choice;
- resource lifetime class;
- device/profile version;
- measured timing/memory counters when present.

This will allow an agent to diagnose a bad result by querying one structured artifact rather than reconstructing hidden runtime decisions from source and logs.
