# Plan 0003: Temporal RAW reconstruction and Android capture

Status: Active
Related: ADR-0001 through ADR-0005; Plans 0001 and 0002
Current step: Verify Vulkan lowering in Actions; implement Android integration.

## Scope and authority

Implement one clean-room temporal RAW path. Keep the deterministic C++ oracle
and existing Vulkan backend. Kotlin and Compose own Android UI and Camera2.
Do not create a second backend solely to add Rust.

This work adds the required P0 slice: typed burst identity, multi-source lineage,
physical bindings, canonical temporal stages, and a versioned plan and trace.
It does not complete the general graph compiler or Plans 0001 and 0002.

## Implemented increments

- Start from main `28535ff41ead853a219ad4003a1300a7dbf6ac07`.
- Commit `75a25cb02d9e25379a755b53b5037faa41b9b87e` implements deterministic
  selection, gain-aware radiometry, multiscale/local registration, same-parity
  CFA sampling, Tukey fusion, conditional uncertainty, and scene reconstruction.
- The Vulkan increment lowers sampling, weights, persistent accumulation, and
  regional support to FP32 compute. Normalization and registration remain CPU
  stages. No FP16 arithmetic or storage is used. The executor reuses one source
  buffer. It reads small regional counters per frame and final accumulators once.
- A host shader compiler is required for cross builds. Target binaries are never
  executed as build tools. Native CI adds SPIR-V validation and sanitizers.

## Remaining acceptance work

1. Run real Vulkan differential, validation-layer, and lifetime tests in Actions.
2. Extend reference quality/calibration and bounded-working-set experiments.
3. Add the Android app, native replay, capture policy, Camera2, and MediaStore.
4. Prove Android assembly, lint, unit tests, native loading, and fixture UI flow.
5. Record host/software-driver benchmarks and review the complete final diff.

Reference gates cover validation, lineage, N=1 image equivalence, all Bayer
layouts, odd borders, sub-black, radiometric variance scaling, translations,
motion rejection, clipping, replay, and Monte Carlo calibration. Fixed-weight
variance evidence does not establish unconditional calibration of adaptive
weights. The current high-frequency fractional-motion fixture fails closed at
low registration confidence; its displacement gate alone is not a quality claim.

## Verification ledger

| Requirement | Environment | Evidence | Status | Latest SHA |
| --- | --- | --- | --- | --- |
| Fresh unchanged baseline | GitHub Ubuntu 24.04, GCC 13.3, lavapipe | Run 35520430992, job 106210609008; native 5/5, no-Vulkan 4/4, Ultra HDR 4/4; complete log inspected | VERIFIED | 28535ff41ead853a219ad4003a1300a7dbf6ac07 |
| CPU temporal properties and regressions | GitHub Ubuntu 24.04 | Run 35561432556, job 106214904806; native 6/6, no-Vulkan 5/5, Ultra HDR 5/5; complete log inspected | VERIFIED | 75a25cb02d9e25379a755b53b5037faa41b9b87e |
| Vulkan increment compilation | Editing container, GCC strict | All shader/C++ targets build; native CTest 7/7 but Vulkan execution visibly skips without ICD | PARTIALLY VERIFIED | Current Vulkan increment |
| ASan, UBSan, leaks | Editing container, Clang | All 5 no-Vulkan CTest groups pass with halt-on-error and leak detection | VERIFIED | Current Vulkan increment |
| Temporal Vulkan dispatch/differential/lifetime | Actions required | Tests added; no execution evidence yet | UNVERIFIED | Current Vulkan increment |
| Android integration | Not implemented yet | No evidence | UNVERIFIED | Not applicable |
| Real RAW, metadata, IMU, OEM, external import | Physical Android device | No device connected | UNVERIFIED | Not applicable |
| Mobile latency, thermals, energy, memory traffic | Physical Android device | No device connected | UNVERIFIED | Not applicable |

## Environment and clean-room record

The editing container cannot resolve GitHub and has no Android SDK or Vulkan
ICD. The connector supports repository writes and Actions. Reproduction artifacts
supply pinned sources and Git bundles. Artifact digests and internal checksums
are verified. The committed CPU tree equals the locally tested tree.

The earlier contract iteration reported directory-only inspection of the uploaded
archive. This implementation iteration has not opened the archive. No private
code, parameters, tuning, topology, or weights inform the implementation.
The Google HDR+ public project and paper abstract, Android metadata definitions,
and Khronos synchronization specifications are public references. The paper PDF
could not be retrieved; no full-paper review is claimed. All algorithm code and
synthetic fixtures are independently written. No third-party HDR+ code is copied.
