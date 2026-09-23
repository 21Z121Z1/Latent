# Plan 0005: independently implemented joint RAW reconstruction

Status: Active
Owner: Latent reconstruction engineering
Related: ADR 0005; Plan 0004
Current step: implementation complete; integration and merge remain gated by the live exact-head PR checks, not a copied CI snapshot.

## Goal

Extend the canonical bounded direct-CFA executor with independently implemented,
phase-admitted joint RAW regression and measurable multiframe super-resolution,
without importing JSR implementation assets or replacing Latent's semantics.

## Active migration dependency

Work began by querying live `main`, instructions, open/merged PRs, branches and
Actions. The branch was created from `main` at
`3bab36689c5978ffc57b83fced040168238ca322`. PR #20 already contains that main
ancestry, the reconciled continuous solver, grouped-CFA semantics and the direct
tiled CPU/Vulkan executor. Its head
`7af2f243d395d802831de25fc72245746fe601a1` was fast-forward reconciled before
implementation. This increment intentionally depends on that canonical work;
it must not be cherry-picked onto old main without the prerequisite. The
existing branches and main are not rewritten. Query GitHub for current stack,
head, review and merge state; these SHAs identify the migration dependency, not
an assertion of current repository HEAD.

## Acceptance criteria

A real CPU and Vulkan joint estimator must run through the existing tiled RAW
API; same-grid controlled SR must improve over its Gaussian and single-frame
baselines. Gain/variance scaling, insufficient-phase fallback, signed covariance,
correlated bounds, grouped/cropped tiling and fail-closed geometry must have
executable tests. Existing difficult tests must remain intact. Memory admission
and Android's existing file schema must remain correct. Latest applicable native,
no-Vulkan, sanitizer, external-codec, Android and installed-APK CI must converge.
No physical device or JSR numerical-parity result may be inferred from those gates.

## Architectural constraints and current facts

The [operation specification](../joint-raw-reconstruction.md) owns this
increment's mathematical/provenance/compatibility detail. `SensorSampling`
remains the CFA authority; the continuous guide remains the registration solver.
Observations, delegated kernel policy and capabilities remain distinct. Output
is explicitly camera-linear, not a falsely labelled `SceneFrame`. Scene scale,
WB/color conversion, rendering and encoding are not folded into fusion. A separate
`reconstructRawScene` adapter now composes the camera-linear output with the
canonical scene/color boundary and existing SDR/HDR rendering.

Implemented: scale-homogeneous robust weights; correct spatial-noise debiasing;
six-basis quadratic moments; phase/SNR/leverage admission; Gaussian fallback;
full signed noise covariance and separate correlated bounds; QR-root frame
support; affine-plus-row inverse Jacobians; matching GLSL; actual-size arena
admission; versioned trace/diagnostics; explicit high-detail policy; N=1
specialization; V1 Android wire packing; reproducible matched-grid benchmark.
Also implemented: direct camera-to-scene FP32 finishing; shared DNG/matrix color
resolution; calibrated common-green validation; fail-closed missing RGB coverage;
conditional cross-color variance bounds; pre-allocation scene residency admission;
complete captured-member lineage and rendering composition. Per-frame regional
contribution counts remain unavailable from direct aggregate moments; no counts
are invented. The existing FusedRaw audit is unchanged.
JSR source/weights/binaries are neither consumed nor redistributed.

## Steps and validation

1. Reconcile live source and prove the inherited baseline locally.
2. Derive and implement the independent operation, keeping the Gaussian zeroth
   moment as the ablation/fallback in the same accumulator.
3. Add independent irradiance, support, gain and covariance oracles. Debug
   observed alias rejection, scatter-noise subtraction and border cancellation
   rather than loosening comparison gates.
4. Lower to Vulkan, validate actual execution against the CPU oracle and add the
   same SR/gain/phase property fixtures to required-GPU CI.
5. Compose the direct camera-linear output with the existing scene boundary;
   prove DNG/matrix, green calibration, bound propagation, lineage, cancellation
   and output admission on CPU and actual Vulkan. Fix Android Kotlin long-valued
   Camera2 use cases and explicit native semantic ID construction.
6. Publish commits and PR, inspect latest checks/artifacts, fix failures and
   rerun until the software acceptance gates converge.

Local strict Release no-Vulkan and software-Vulkan runs are evidence for the
working source only. Exact-head Actions and installed-APK reports are attached
to delivery, not inferred from the prerequisite's green checks. Required suites
are enumerated in [the specification](../joint-raw-reconstruction.md#validation-and-reproduction)
and [verification rules](../verification.md).

## Migration / compatibility

The direct kernel policy replaces an absolute variance floor with a clearly
named relative floor and adds explicit fitting policy. Internal CPU/GPU layouts
change together. Arena sizes are derived. New per-pixel diagnostic fields do not
silently alter Android's 16-float V1 file. The native trace is versioned and owns
the algorithm identifier. No automatic production capture-mode promotion occurs.

## Status ledger

Implemented: live-state reconciliation, provenance boundary, joint CPU/Vulkan
reconstruction, scene/render composition, independent property/differential tests
and Android source compatibility corrections. Local strict no-Vulkan and actual
software-Vulkan suites pass; that is not a claim about an arbitrary newer commit.
Integration status is the live PR's exact-head native, sanitizer, high-resolution
and Android build/unit/lint/installed-APK checks. Merge requires those gates and
the stacked prerequisite; this plan stays Active until integration is merged.

## Risks / deliberately unclaimed capabilities

The phase score is not calibrated quality/resolution evidence. Uncertainty is
conditional, and remosaic correlation uses a bound. Larger FP32 state has real
bandwidth/latency cost; bounded admission is not a mobile-speed claim. Real RAW
quality, channel LCA, a trained controller and physical-device thermal/quality
validation remain product/research work, not hidden completion claims for this
software increment.
