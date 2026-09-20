# ADR-0005: Temporal RAW reconstruction

Status: Accepted

## Context

A burst has many observed sensor frames and one requested scene reconstruction.
Frame identity must survive alignment, local rejection, rendering, and export.
The owning single-RAW fixture type must not determine production storage.

## Decision

`RawBurst` is an ordered metadata snapshot. It identifies the burst, capture
sequence, members, and common calibration relation. Each member owns its
capture observations. Physical bindings identify members by typed frame IDs.
They are not part of the burst descriptor. A calibration relation identifies a
common sensor coordinate system; it does not certify a nominal ISO as gain.
`RawFrame` remains an owning reference/fixture convenience container.

Capture planning consumes observations, image intent, delegated policy, and
capture capabilities separately. Reconstruction consumes completed observations,
not exposure requests disguised as captured facts. Processing batch size,
working-set limits, backend choice, and resource release belong to the execution
plan. They do not change burst membership or image intent.

Reference selection produces an explicit, deterministic result with candidate
metrics and reasons. Alignment maps an output position in reference geometry
to a source sampling position. Its field and confidence are separate from the
warped values. Alignment proxies and pyramids are execution artifacts, not new
image domains. Noise, exposure, gain, and lens-shading observations retain
source and confidence. Noise coordinates are explicit. Radiometric scale must
propagate variance by the square of that scale.

The first generation uses CFA-separated sampling and robust RAW fusion. It
must never bilinearly interpolate unrelated Bayer sites. Its fused samples are
FP32 sensor-linear values in the selected reference radiometric coordinate.
Input sensor clipping is tracked before correction or normalization. It is not
inferred from a display clamp. Unsupported local evidence falls back to the
reference or another explicitly trusted sample. An all-clipped burst cannot
supply missing highlight information.

Temporal uncertainty separates conditional marginal aleatoric variance,
alignment confidence, robustness confidence, and effective support. The fixed-
weight independent-input formula is not an unconditional variance claim for
adaptive weighting and estimated warps. Spatial covariance omitted by resampling
or demosaic propagation must remain an explicit approximation. A fused variance
map must not be silently fitted back to a shot/read model.

The legal first-generation transition is `RawBurst -> FusedRaw -> SceneFrame`.
A future joint `RawBurst -> SceneFrame` reconstruction is also legal. Callers
request a scene reconstruction; they do not require FusedRaw as its universal
implementation. Scene output remains linear AP1/D60, unbounded, and negative-
preserving. Scene-coordinate scale is distinct from capture and render exposure.

Lineage retains all captured members, the reference, and regional contribution
counts. A partially rejected region must not be reported as uniformly accepted
or rejected. Render and codec outputs retain this immutable lineage. The legacy
scalar `sourceRawId` is not burst provenance.

Canonical temporal operations and the deterministic FP32 reference define the
meaning. A versioned execution plan selects legal lowerings. A structured trace
records actual execution and fallbacks. Vulkan is a lowering, not a second image
algorithm. Optional capabilities cannot change fixed semantics. Vulkan 1.1 has
a correctness path. Differential and lifetime evidence must support optimized
execution claims.

## Clean-room boundary

The algorithms are independently written from public principles and tested on
independent fixtures. Hasinoff et al., SIGGRAPH Asia 2016, motivates constant-
exposure RAW capture and separation of alignment and merging. Android Camera2
specifies capture-result, CFA, noise-profile, and lens-shading units. These are
not permission to copy a third-party HDR+ implementation. Private disassembly,
models, tuning tables, and vendor-specific algorithm parameters are excluded.

## Consequences

The existing C++ oracle and Vulkan infrastructure remain the single compute
implementation. Kotlin owns the Android UI and capture boundary. A language
migration needs separate benefit and differential evidence; language preference
alone does not justify a duplicate backend. Recorded or synthetic replay must
use the same reconstruction API as live capture. Emulator and software Vulkan
evidence do not establish physical-camera behavior or mobile performance.
