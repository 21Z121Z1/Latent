#include "latent/reference/TemporalReconstruct.h"
#include "latent/reference/NoisePropagation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace latent::reference {
namespace {
using imaging::TemporalRejection;
std::size_t index(imaging::Extent e, std::uint32_t x, std::uint32_t y) {
    return static_cast<std::size_t>(y) * e.width + x;
}
bool calibrated(const imaging::ExposureCalibration& c) {
    return c.source == imaging::MetadataSource::MeasuredCalibration ||
           c.source == imaging::MetadataSource::DeviceProfile;
}

class ReferenceFusionSession final : public TemporalFusionSession {
public:
    ReferenceFusionSession(const NormalizedRaw& reference, const TemporalPolicy& policy)
        : reference_(reference), policy_(policy), accum_(reference.samples.size()) {}
    imaging::FrameContribution add(const NormalizedRaw& source,
        const AlignmentField& field, bool isReference) override {
        if ((source.extent.width != reference_.extent.width || source.extent.height != reference_.extent.height) || source.cfa != reference_.cfa ||
            source.samples.size() != reference_.samples.size() ||
            (field.extent.width != source.extent.width || field.extent.height != source.extent.height) || field.tileSize != policy_.tileSize ||
            field.columns != (source.extent.width + field.tileSize - 1U) / field.tileSize ||
            field.rows != (source.extent.height + field.tileSize - 1U) / field.tileSize ||
            field.tiles.size() != static_cast<std::size_t>(field.columns) * field.rows) {
            throw std::invalid_argument("fusion binding/field mismatch");
        }
        imaging::FrameContribution contribution{};
        contribution.frame = field.source;
        contribution.regions.resize(field.tiles.size());
        for (std::uint32_t ty = 0; ty < field.rows; ++ty) {
            for (std::uint32_t tx = 0; tx < field.columns; ++tx) {
                auto& region = contribution.regions[static_cast<std::size_t>(ty) * field.columns + tx];
                region.x = tx * field.tileSize; region.y = ty * field.tileSize;
                region.width = std::min(field.tileSize, source.extent.width - region.x);
                region.height = std::min(field.tileSize, source.extent.height - region.y);
            }
        }
        for (std::uint32_t y = 0; y < source.extent.height; ++y) {
            for (std::uint32_t x = 0; x < source.extent.width; ++x) {
                const auto i = index(source.extent, x, y);
                const auto ti = static_cast<std::size_t>(y / field.tileSize) * field.columns + x / field.tileSize;
                const auto& tile = field.tiles[ti];
                auto& region = contribution.regions[ti];
                const auto sample = isReference ? std::optional<TemporalSample>{source.samples[i]} :
                    sampleCfa(source, x, y, tile.dx, tile.dy);
                if (!sample) {
                    ++region.rejected[static_cast<std::size_t>(TemporalRejection::Border)];
                    accumulateTemporalSample(accum_[i], {}, {0, 0, TemporalRejection::Border}, 0);
                    continue;
                }
                const float confidence = isReference ? 1.0F : tile.confidence * source.radiometricConfidence;
                const auto decision = temporalWeight(reference_.samples[i], *sample, confidence, policy_, isReference);
                accumulateTemporalSample(accum_[i], *sample, decision, confidence);
                if (decision.rejection) {
                    ++region.rejected[static_cast<std::size_t>(*decision.rejection)];
                } else {
                    ++region.accepted;
                    ++contribution.acceptedSamples;
                }
            }
        }
        return contribution;
    }
    std::vector<FusionAccumulator> finish() override { return std::move(accum_); }
private:
    const NormalizedRaw& reference_;
    TemporalPolicy policy_;
    std::vector<FusionAccumulator> accum_;
};
}  // namespace

void validateTemporalPolicy(const TemporalPolicy& p) {
    if (p.tileSize < 8U || p.tileSize > 256U || (p.tileSize & 1U) != 0U ||
        p.maximumDisplacement > 1024U || !std::isfinite(p.residualCutoffSigma) ||
        p.residualCutoffSigma < 1.0F || p.residualCutoffSigma > 100.0F ||
        !std::isfinite(p.minimumAlignmentConfidence) || p.minimumAlignmentConfidence < 0.0F ||
        p.minimumAlignmentConfidence > 1.0F || !std::isfinite(p.varianceFloor) ||
        p.varianceFloor < 1.0e-12F || p.varianceFloor > 1.0F ||
        !std::isfinite(p.missingNoiseVariance) || p.missingNoiseVariance < p.varianceFloor ||
        p.missingNoiseVariance > 1.0F) {
        throw std::invalid_argument("invalid temporal policy");
    }
}

NormalizedRaw normalizeTemporalRaw(runtime::RawFrameView source,
    const imaging::RawFrameMetadata& reference, const TemporalPolicy& policy, bool applyLsc) {
    validateTemporalPolicy(policy);
    const auto valid = runtime::validateRawView(source);
    if (!valid.valid) throw std::invalid_argument(valid.message);
    const auto& metadata = *source.metadata;
    const auto levels = selectRawLevels(metadata);
    const auto refLevels = selectRawLevels(reference);
    auto sensor = normalizeRaw(source);
    NormalizedRaw out{};
    out.extent = sensor.extent; out.cfa = sensor.cfa; out.levels = refLevels;
    out.samples.resize(sensor.samples.size());
    imaging::NoiseModel noise{};
    out.noiseEstimated = !metadata.noiseProfile.usable();
    if (!out.noiseEstimated) noise = normalizeNoiseModel(*metadata.noiseProfile.value, levels);
    else noise.read.fill(policy.missingNoiseVariance);
    const bool measured = calibrated(metadata.exposureCalibration) && calibrated(reference.exposureCalibration);
    const bool equalIso = metadata.sensitivityIso == reference.sensitivityIso;
    if (!measured) {
        out.gainEstimated = true;
        out.radiometricConfidence = equalIso ? 0.75F : 0.0F;
        if (!equalIso && policy.allowNominalIsoGainEstimate) out.radiometricConfidence = 0.35F;
    } else {
        out.radiometricConfidence = 1.0F - std::max(metadata.exposureCalibration.gainUncertainty,
                                                    reference.exposureCalibration.gainUncertainty);
    }
    for (std::size_t c = 0; c < 4; ++c) {
        const float gainRatio = measured ? reference.exposureCalibration.effectiveGain[c] /
            metadata.exposureCalibration.effectiveGain[c] :
            (equalIso || !policy.allowNominalIsoGainEstimate ? 1.0F : reference.sensitivityIso / metadata.sensitivityIso);
        out.radiometricScale[c] = static_cast<float>(reference.exposureTimeNs) /
            static_cast<float>(metadata.exposureTimeNs) * gainRatio *
            (levels.white - levels.black.cfa[c]) / (refLevels.white - refLevels.black.cfa[c]);
        if (!std::isfinite(out.radiometricScale[c]) || out.radiometricScale[c] <= 0.0F) {
            throw std::invalid_argument("non-finite radiometric transform");
        }
    }
    for (std::uint32_t y = 0; y < out.extent.height; ++y) {
        for (std::uint32_t x = 0; x < out.extent.width; ++x) {
            const auto i = index(out.extent, x, y);
            const auto channel = imaging::cfaChannelAt(out.cfa, x, y);
            const auto c = static_cast<std::size_t>(channel);
            const float raw = static_cast<float>(source.storage.pixels[static_cast<std::size_t>(y) * source.storage.rowStridePixels + x]);
            const float lsc = applyLsc && metadata.lensShading.usable() ?
                lensShadingGainAt(*metadata.lensShading.value, out.extent, out.cfa, x, y) : 1.0F;
            const float a = out.radiometricScale[c] * lsc;
            auto& s = out.samples[i];
            s.value = sensor.samples[i] * a;
            s.variance = (noise.shot[c] * std::max(sensor.samples[i], 0.0F) + noise.read[c]) * a * a;
            s.usable = raw < levels.white ? 1.0F : 0.0F;
            if (!std::isfinite(s.value) || !std::isfinite(s.variance) || s.variance < 0.0F) {
                throw std::invalid_argument("non-finite normalized temporal evidence");
            }
        }
    }
    // Defective samples cannot become temporal evidence. The reference fallback
    // retains the observation; no unmodelled corrected value is assigned a variance.
    for (const auto& d : metadata.defects) {
        if (d.x >= out.extent.width || d.y >= out.extent.height) throw std::invalid_argument("defect outside RAW extent");
        out.samples[index(out.extent, d.x, d.y)].usable = 0.0F;
        out.samples[index(out.extent, d.x, d.y)].invalidReason = static_cast<float>(TemporalRejection::Metadata);
    }
    return out;
}

ReferenceSelection selectBurstReference(const imaging::RawBurst& burst,
    const runtime::HostRawBindings& bindings, const TemporalPolicy& policy,
    std::optional<imaging::FrameId> requested) {
    const auto valid = bindings.validate(burst);
    if (!valid.valid) throw std::invalid_argument(valid.message);
    ReferenceSelection selection{};
    for (const auto& member : burst.members) {
        const auto frame = normalizeTemporalRaw(bindings.view(burst, member.id), member.observations, policy, false);
        ReferenceCandidate metric{}; metric.frame = member.id; metric.noiseEstimated = frame.noiseEstimated;
        float sharpness = 0, variance = 0, clipped = 0, pairs = 0;
        for (std::uint32_t y = 0; y < frame.extent.height; ++y) {
            for (std::uint32_t x = 0; x < frame.extent.width; ++x) {
                const auto& s = frame.samples[index(frame.extent, x, y)];
                variance += s.variance; clipped += s.usable == 0.0F ? 1.0F : 0.0F;
                if (x >= 2U && s.usable != 0.0F) {
                    const auto& t = frame.samples[index(frame.extent, x - 2U, y)];
                    if (t.usable != 0.0F) {
                        const float delta = s.value - t.value;
                        sharpness += std::max(0.0F, delta * delta - s.variance - t.variance);
                        pairs += 1.0F;
                    }
                }
            }
        }
        const float n = static_cast<float>(frame.samples.size());
        metric.clippingFraction = clipped / n;
        metric.meanVariance = variance / n;
        metric.sharpness = sharpness / std::max(1.0F, pairs);
        selection.candidates.push_back(metric);
    }
    // Lexicographic, no learned coefficients. Lowest clipped area, then useful
    // same-channel structure, then variance. Capture order breaks exact ties.
    const auto better = [](const auto& a, const auto& b) {
        return std::tuple{a.clippingFraction, -a.sharpness, a.meanVariance} <
               std::tuple{b.clippingFraction, -b.sharpness, b.meanVariance};
    };
    auto selected = std::min_element(selection.candidates.begin(), selection.candidates.end(), better);
    if (requested) {
        selected = std::find_if(selection.candidates.begin(), selection.candidates.end(),
            [&](const auto& c) { return c.frame == *requested; });
        if (selected == selection.candidates.end()) throw std::invalid_argument("requested reference is not a burst member");
        selection.explicitlyRequested = true;
    }
    selection.frame = selected->frame;
    selection.confidence = (1.0F - selected->clippingFraction) * (selected->noiseEstimated ? 0.5F : 1.0F);
    return selection;
}

std::optional<TemporalSample> sampleCfa(const NormalizedRaw& source,
    std::uint32_t x, std::uint32_t y, float dx, float dy) {
    if (!std::isfinite(dx) || !std::isfinite(dy) || x >= source.extent.width || y >= source.extent.height) return {};
    const auto px = x & 1U, py = y & 1U;
    const float fx = (static_cast<float>(x - px) + dx) * 0.5F;
    const float fy = (static_cast<float>(y - py) + dy) * 0.5F;
    const float maxX = static_cast<float>((source.extent.width - 1U - px) / 2U);
    const float maxY = static_cast<float>((source.extent.height - 1U - py) / 2U);
    if (fx < 0 || fy < 0 || fx > maxX || fy > maxY) return {};
    const auto ix = static_cast<std::uint32_t>(fx), iy = static_cast<std::uint32_t>(fy);
    const float tx = fx - static_cast<float>(ix), ty = fy - static_cast<float>(iy);
    TemporalSample result{}; result.usable = 1.0F;
    for (std::uint32_t j = 0; j < 2; ++j) for (std::uint32_t i = 0; i < 2; ++i) {
        const float w = (i == 0U ? 1.0F - tx : tx) * (j == 0U ? 1.0F - ty : ty);
        if (w == 0.0F) continue; // Do not fetch an unused out-of-bounds edge tap.
        const auto sx = (ix + i) * 2U + px, sy = (iy + j) * 2U + py;
        const auto& s = source.samples[index(source.extent, sx, sy)];
        result.value += w * s.value;
        result.variance += w * w * s.variance;
        result.usable = std::min(result.usable, s.usable);
        if (s.usable == 0.0F) result.invalidReason = s.invalidReason;
    }
    return result;
}

SampleDecision temporalWeight(TemporalSample ref, TemporalSample src, float confidence,
    const TemporalPolicy& policy, bool isReference) {
    if (!std::isfinite(src.value) || !std::isfinite(src.variance) || src.variance < 0.0F ||
        !std::isfinite(confidence)) return {0, 0, TemporalRejection::Metadata};
    if (src.usable == 0.0F) return {0, 0, src.invalidReason == static_cast<float>(TemporalRejection::Metadata) ?
        TemporalRejection::Metadata : TemporalRejection::Clipped};
    if (!isReference && confidence < policy.minimumAlignmentConfidence) return {0, 0, TemporalRejection::Alignment};
    float robust = 1.0F;
    if (!isReference && ref.usable != 0.0F) {
        const float delta = src.value - ref.value;
        const float z2 = delta * delta / (src.variance + ref.variance + policy.varianceFloor);
        const float u = z2 / (policy.residualCutoffSigma * policy.residualCutoffSigma);
        if (u >= 1.0F) return {0, 0, TemporalRejection::Motion};
        robust = (1.0F - u) * (1.0F - u);
    }
    // A common reference scale keeps accumulator weights well-conditioned.
    // It cancels in the mean, N_eff and conditional variance formulas.
    const float weight = (ref.variance + policy.varianceFloor) /
        (src.variance + policy.varianceFloor) * confidence * robust;
    if (!std::isfinite(weight) || weight <= 0.0F) return {0, 0, TemporalRejection::Metadata};
    return {weight, robust, {}};
}
void accumulateTemporalSample(FusionAccumulator& a, TemporalSample s, SampleDecision d, float confidence) {
    a.attempts += 1.0F;
    a.alignment += std::clamp(confidence, 0.0F, 1.0F);
    a.robustness += d.robustness;
    if (d.rejection) return;
    a.weightedValue += d.weight * s.value;
    a.weight += d.weight;
    a.weightSquared += d.weight * d.weight;
    a.weightedVariance += d.weight * d.weight * s.variance;
}
std::unique_ptr<TemporalFusionSession> makeReferenceFusionSession(const NormalizedRaw& reference, const TemporalPolicy& policy) {
    validateTemporalPolicy(policy);
    return std::make_unique<ReferenceFusionSession>(reference, policy);
}
FusedRaw finishTemporalFusion(const NormalizedRaw& reference,
    std::span<const FusionAccumulator> accumulation, imaging::Lineage lineage) {
    if (accumulation.size() != reference.samples.size()) throw std::invalid_argument("fusion accumulator extent mismatch");
    FusedRaw fused{}; fused.sensor.extent = reference.extent; fused.sensor.cfa = reference.cfa;
    fused.sensor.levels = reference.levels; fused.lineage = std::move(lineage);
    const auto n = accumulation.size();
    fused.sensor.samples.resize(n);
    fused.uncertainty.marginalVariance.resize(n);
    fused.uncertainty.alignmentConfidence.resize(n);
    fused.uncertainty.robustnessConfidence.resize(n);
    fused.uncertainty.effectiveSampleCount.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& a = accumulation[i]; const auto& r = reference.samples[i];
        const bool supported = a.weight > 0 && a.weightSquared > 0;
        fused.sensor.samples[i] = supported ? a.weightedValue / a.weight : r.value;
        fused.uncertainty.marginalVariance[i] = supported ? a.weightedVariance / (a.weight * a.weight) : r.variance;
        fused.uncertainty.alignmentConfidence[i] = a.attempts > 0 ? a.alignment / a.attempts : 0.0F;
        fused.uncertainty.robustnessConfidence[i] = a.attempts > 0 ? a.robustness / a.attempts : 0.0F;
        fused.uncertainty.effectiveSampleCount[i] = supported ? a.weight * a.weight / a.weightSquared : 0.0F;
        if (!std::isfinite(fused.sensor.samples[i]) || !std::isfinite(fused.uncertainty.marginalVariance[i])) {
            throw std::runtime_error("non-finite fusion result");
        }
    }
    return fused;
}
}  // namespace latent::reference
