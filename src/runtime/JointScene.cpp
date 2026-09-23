#include "latent/runtime/JointScene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace latent::runtime {
namespace {
class SceneSink final : public ReconstructionTileSink {
public:
    SceneSink(JointSceneResult& result, imaging::Extent extent,
        const reference::ReconstructionConfig& color) : result_(result), color_(color) {
        result_.scene.image.extent = extent;
        result_.scene.image.rgb.resize(static_cast<std::size_t>(extent.pixelCount()) * 3U);
        if (color.propagateNoise) result_.conditionalVarianceUpperBound.resize(result_.scene.image.rgb.size());
    }
    std::uint64_t residentBytes() const override {
        return (static_cast<std::uint64_t>(result_.scene.image.rgb.capacity()) +
            result_.conditionalVarianceUpperBound.capacity()) * sizeof(float);
    }
    void write(imaging::SensorRect rect, std::span<const reference::ReconstructedPixel> pixels) override {
        const auto e = result_.scene.image.extent;
        if (!rect.width || !rect.height || rect.x >= e.width || rect.y >= e.height ||
            rect.width > e.width - rect.x || rect.height > e.height - rect.y ||
            pixels.size() != static_cast<std::size_t>(rect.width) * rect.height) {
            throw std::invalid_argument("invalid scene sink tile");
        }
        // Row spans avoid retaining even a second tile-sized color payload.
        for (std::uint32_t y = 0; y < rect.height; ++y) {
            const auto offset = (static_cast<std::size_t>(rect.y + y) * e.width + rect.x) * 3U;
            auto rgb = std::span(result_.scene.image.rgb).subspan(offset, rect.width * 3U);
            auto variance = color_.propagateNoise ?
                std::span(result_.conditionalVarianceUpperBound).subspan(offset, rect.width * 3U) : std::span<float>{};
            reference::finishDirectSceneTile(pixels.subspan(static_cast<std::size_t>(y) * rect.width, rect.width),
                color_, rgb, variance);
        }
    }
private:
    JointSceneResult& result_;
    const reference::ReconstructionConfig& color_;
};
}

JointSceneResult reconstructRawScene(const imaging::RawBurst& burst, RawTileSource& source,
    const ReconstructionGrid& grid, imaging::FrameId referenceId,
    const reference::ReconstructionConfig& color, const TiledReconstructionPolicy& policy,
    const ReconstructionCapabilities& caps, std::span<const ReconstructionMotion> motion,
    std::function<bool()> continueExecution) {
    if (continueExecution && !continueExecution()) throw std::runtime_error("direct reconstruction cancelled");
    // Admission precedes image allocation, including checked size arithmetic.
    const std::uint64_t channels = color.propagateNoise ? 6U : 3U;
    if (grid.extent.pixelCount() > std::numeric_limits<std::size_t>::max() / (channels * sizeof(float))) {
        throw std::invalid_argument("direct scene output size overflow");
    }
    const auto bytes = grid.extent.pixelCount() * channels * sizeof(float);
    (void)planTiledReconstruction(burst, grid, policy, caps, source.residentBytes(), bytes);
    const auto member = std::find_if(burst.members.begin(), burst.members.end(),
        [&](const auto& m) { return m.id == referenceId; });
    if (member == burst.members.end()) throw std::invalid_argument("scene reference is not in burst");
    if (color.applyLensShading || color.defectCorrection != reference::DefectCorrectionMode::Disabled) {
        throw std::invalid_argument("direct scene sensor corrections belong to tiled policy");
    }
    (void)reference::cameraToSceneMatrix(color);
    (void)reference::sceneCoordinateScale(color.sceneScaleEV);
    const auto& gains = member->observations.colorCorrectionGains;
    const float g0 = gains.usable() ? (*gains.value)[1] : 1.0F;
    const float g1 = gains.usable() ? (*gains.value)[2] : 1.0F;
    const float mean = 0.5F * (g0 + g1);
    const float requestedMean = 0.5F * (color.whiteBalanceGains[1] + color.whiteBalanceGains[2]);
    if (!std::isfinite(mean) || mean <= 0.0F || !std::isfinite(requestedMean) || requestedMean <= 0.0F) {
        throw std::invalid_argument("invalid scene green calibration");
    }
    // Both ratios lie in [0,2]. Eight FP32 epsilons cover independent normalized
    // gain rounding, not a license to alter the reference green calibration.
    if (std::abs(g0 / mean - color.whiteBalanceGains[1] / requestedMean) >
        8.0F * std::numeric_limits<float>::epsilon()) {
        throw std::invalid_argument("scene white balance disagrees with reference green normalization");
    }
    JointSceneResult result{};
    SceneSink sink(result, grid.extent, color);
    result.trace = reconstructRawTiles(burst, source, grid, policy, caps, sink, motion,
        referenceId, std::move(continueExecution));
    auto lineage = std::make_shared<imaging::ImageLineage>();
    lineage->burst = burst.id;
    lineage->sequence = burst.sequence;
    lineage->calibration = burst.calibration;
    lineage->reference = result.trace.reference;
    lineage->inputs.reserve(burst.members.size());
    for (const auto& m : burst.members) lineage->inputs.push_back(m.id);
    result.scene.lineage = std::move(lineage);
    result.scene.sourceRawId = burst.members.size() == 1U ? referenceId.value : 0U;
    result.scene.sceneScaleEV = color.sceneScaleEV;
    result.scene.whiteBalanceConfidence = color.whiteBalanceConfidence;
    // Conditional bounds stay in their own evidence vector; the single-RAW lazy
    // shot/read/demosaic model is deliberately left invalid.
    return result;
}

}  // namespace latent::runtime
