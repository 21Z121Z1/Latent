#pragma once

#include "latent/imaging/RawBurst.h"
#include "latent/reference/ReferenceReconstruct.h"
#include "latent/runtime/RawBindings.h"

#include <limits>
#include <memory>
#include <optional>
#include <span>

namespace latent::reference {

// Latent-owned delegated policy. These values are not sensor observations.
struct TemporalPolicy {
    std::uint32_t tileSize = 32U;       // Reference sensor pixels.
    std::uint32_t maximumDisplacement = 64U;
    float residualCutoffSigma = 6.0F;  // Tukey rejection, not a vendor tuning value.
    float minimumAlignmentConfidence = 0.15F;
    float varianceFloor = 1.0e-10F;
    float missingNoiseVariance = 1.0e-4F;
    bool allowNominalIsoGainEstimate = false;
};

struct ReferenceCandidate {
    imaging::FrameId frame{};
    float clippingFraction = 0.0F;
    float sharpness = 0.0F;
    float meanVariance = 0.0F;
    bool noiseEstimated = false;
};
struct ReferenceSelection {
    imaging::FrameId frame{};
    float confidence = 0.0F;
    std::vector<ReferenceCandidate> candidates;
    bool explicitlyRequested = false;
};

// Host execution representations, not semantic ownership requirements. The
// fourth float keeps the sample ABI suitable for an FP32 Vulkan vec4 buffer.
struct TemporalSample {
    float value = 0.0F;
    float variance = 0.0F;
    float usable = 1.0F;  // Input clipping/defect validity, never a display clamp.
    float invalidReason = 1.0F; // TemporalRejection::Clipped; meaningful only when unusable.
};
static_assert(sizeof(TemporalSample) == 16U);
struct NormalizedRaw {
    imaging::Extent extent{};
    imaging::CfaPattern cfa{};
    SelectedRawLevels levels{};
    std::vector<TemporalSample> samples;
    std::array<float, 4> radiometricScale{1, 1, 1, 1};
    float radiometricConfidence = 1.0F;
    bool gainEstimated = false;
    bool noiseEstimated = false;
};

// sourcePosition = referencePosition + displacement, in sensor pixels.
struct MotionTile {
    float dx = 0.0F, dy = 0.0F;
    float confidence = 0.0F;
    float residual = 0.0F;
};
static_assert(sizeof(MotionTile) == 16U);
enum class GeometryStatus : std::uint32_t { Reference, Estimated, Unobservable, Ambiguous, Inconsistent };
struct RegistrationEvidence {
    // Conditional linearized guide-noise scale, not an unconditional calibrated
    // displacement posterior. Includes a conservative cubic-stencil reuse factor.
    float localizationStdDevPixels = std::numeric_limits<float>::infinity();
    float cycleErrorPixels = std::numeric_limits<float>::infinity();
    std::uint32_t supportedGuideSamples = 0;
    GeometryStatus status = GeometryStatus::Unobservable;
};
struct AlignmentField {
    imaging::FrameId source{}, reference{};
    imaging::Extent extent{};
    std::uint32_t tileSize = 0U, columns = 0U, rows = 0U;
    MotionTile global{};
    std::vector<MotionTile> tiles;
    RegistrationEvidence globalEvidence;
    std::vector<RegistrationEvidence> evidence;
};

struct TemporalUncertainty {
    // Conditional marginal aleatoric estimate. Adaptive weights, estimated
    // geometry, and resampling covariance are NOT an unconditional model.
    std::vector<float> marginalVariance;
    std::vector<float> alignmentConfidence;
    std::vector<float> robustnessConfidence;
    std::vector<float> effectiveSampleCount;
};
struct FusedRaw {
    SensorLinearFrameF32 sensor;
    TemporalUncertainty uncertainty;
    imaging::Lineage lineage;
};

struct FusionAccumulator {
    float weightedValue = 0, weight = 0, weightSquared = 0;
    float weightedVariance = 0, alignment = 0, robustness = 0;
    float attempts = 0, reserved = 0;
};
static_assert(sizeof(FusionAccumulator) == 32U);

struct SampleDecision {
    float weight = 0, robustness = 0;
    std::optional<imaging::TemporalRejection> rejection;
};

void validateTemporalPolicy(const TemporalPolicy& policy);
[[nodiscard]] NormalizedRaw normalizeTemporalRaw(
    runtime::RawFrameView source, const imaging::RawFrameMetadata& reference,
    const TemporalPolicy& policy, bool applyLensShading);
[[nodiscard]] ReferenceSelection selectBurstReference(
    const imaging::RawBurst& burst, const runtime::HostRawBindings& bindings,
    const TemporalPolicy& policy, std::optional<imaging::FrameId> requested = {});
[[nodiscard]] AlignmentField alignTemporalRaw(
    const NormalizedRaw& reference, const NormalizedRaw& source,
    imaging::FrameId referenceId, imaging::FrameId sourceId,
    const TemporalPolicy& policy);

// Same-parity planes preserve R/G0/G1/B identity, including fractional warps.
[[nodiscard]] std::optional<TemporalSample> sampleCfa(
    const NormalizedRaw& source, std::uint32_t x, std::uint32_t y, float dx, float dy);
[[nodiscard]] SampleDecision temporalWeight(
    TemporalSample reference, TemporalSample source, float alignmentConfidence,
    const TemporalPolicy& policy, bool isReference);
void accumulateTemporalSample(FusionAccumulator& accumulator, TemporalSample sample,
                              SampleDecision decision, float alignmentConfidence);

// Physical executor interface for ONE canonical fusion operation. Alignment,
// normalization, reference choice and semantic policy are not backend decisions.
class TemporalFusionSession {
public:
    virtual ~TemporalFusionSession() = default;
    virtual imaging::FrameContribution add(
        const NormalizedRaw& source, const AlignmentField& field, bool isReference) = 0;
    virtual std::vector<FusionAccumulator> finish() = 0;
};
[[nodiscard]] std::unique_ptr<TemporalFusionSession> makeReferenceFusionSession(
    const NormalizedRaw& reference, const TemporalPolicy& policy);
[[nodiscard]] FusedRaw finishTemporalFusion(
    const NormalizedRaw& reference, std::span<const FusionAccumulator> accumulation,
    imaging::Lineage lineage);

}  // namespace latent::reference
