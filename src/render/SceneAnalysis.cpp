#include "latent/render/SceneAnalysis.h"

#include "latent/imaging/ColorScience.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

namespace latent::render {
namespace {

float percentile(const std::vector<float>& sortedValues, double quantile) {
    if (sortedValues.empty()) {
        throw std::invalid_argument("percentile requires at least one value");
    }

    const double position =
        quantile * static_cast<double>(sortedValues.size() - 1U);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    const float fraction = static_cast<float>(
        position - static_cast<double>(lower));
    return sortedValues[lower] +
           (sortedValues[upper] - sortedValues[lower]) * fraction;
}

void validateSceneForAnalysis(const imaging::SceneFrame& scene) {
    if (scene.reference != imaging::ReferenceDomain::Scene ||
        scene.transfer != imaging::TransferFunction::Linear ||
        scene.range != imaging::RangeSemantics::Unbounded ||
        !scene.allowNegative) {
        throw std::invalid_argument(
            "scene analysis requires the linear unbounded SceneFrame contract");
    }
    if (scene.primaries != imaging::Primaries::ACEScgAP1 ||
        scene.whitePoint != imaging::WhitePoint::D60) {
        throw std::invalid_argument(
            "scene analysis currently requires ACEScg/AP1 with D60 white");
    }
    if (!std::isfinite(scene.sceneScaleEV)) {
        throw std::invalid_argument("sceneScaleEV must be finite");
    }
    if (scene.image.extent.width == 0U || scene.image.extent.height == 0U) {
        throw std::invalid_argument("scene analysis requires a non-empty image");
    }

    const auto pixelCount = scene.image.extent.pixelCount();
    if (pixelCount >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / 3U)) {
        throw std::invalid_argument("scene image is too large for host indexing");
    }
    const auto expected = static_cast<std::size_t>(pixelCount) * 3U;
    if (scene.image.rgb.size() != expected) {
        throw std::invalid_argument(
            "SceneFrame RGB payload does not match its declared extent");
    }
}

}  // namespace

SceneLuminanceStats analyzeSceneLuminance(const imaging::SceneFrame& scene) {
    validateSceneForAnalysis(scene);

    const imaging::Matrix3f ap1ToXyz = imaging::rgbPrimariesToXyzMatrix(
        imaging::kAp1Red,
        imaging::kAp1Green,
        imaging::kAp1Blue,
        imaging::kAcesWhite);

    // Row 1 is XYZ Y. SceneFrame pixels already contain the explicit
    // coordinate factor 2^sceneScaleEV, so subtract sceneScaleEV in log2
    // space to recover exposure-relative luminance without risking exp2
    // overflow/underflow.
    const float yR = ap1ToXyz.values[3];
    const float yG = ap1ToXyz.values[4];
    const float yB = ap1ToXyz.values[5];

    std::vector<float> luminanceEV;
    luminanceEV.reserve(
        static_cast<std::size_t>(scene.image.extent.pixelCount()));

    SceneLuminanceStats stats{};
    stats.pixelCount =
        static_cast<std::size_t>(scene.image.extent.pixelCount());

    for (std::size_t pixel = 0U; pixel < stats.pixelCount; ++pixel) {
        const std::size_t base = pixel * 3U;
        const float r = scene.image.rgb[base];
        const float g = scene.image.rgb[base + 1U];
        const float b = scene.image.rgb[base + 2U];
        if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) {
            throw std::invalid_argument(
                "scene analysis rejects NaN/Inf SceneFrame samples");
        }

        const float storedLuminance = yR * r + yG * g + yB * b;
        if (!(storedLuminance > 0.0F) || !std::isfinite(storedLuminance)) {
            ++stats.nonPositiveLuminanceCount;
            continue;
        }

        const float ev = std::log2(storedLuminance) - scene.sceneScaleEV;
        if (!std::isfinite(ev)) {
            throw std::invalid_argument(
                "scene luminance produced a non-finite exposure value");
        }
        luminanceEV.push_back(ev);
    }

    stats.positiveLuminanceCount = luminanceEV.size();
    if (luminanceEV.empty()) {
        throw std::invalid_argument(
            "scene analysis requires at least one positive-luminance pixel");
    }

    std::sort(luminanceEV.begin(), luminanceEV.end());
    stats.minimumEV = luminanceEV.front();
    stats.p01EV = percentile(luminanceEV, 0.01);
    stats.medianEV = percentile(luminanceEV, 0.50);
    stats.p99EV = percentile(luminanceEV, 0.99);
    stats.maximumEV = luminanceEV.back();
    return stats;
}

float suggestRenderExposureEV(
    const SceneLuminanceStats& stats,
    float targetMedianLinear) {
    if (stats.positiveLuminanceCount == 0U ||
        !std::isfinite(stats.medianEV)) {
        throw std::invalid_argument(
            "render exposure suggestion requires valid scene statistics");
    }
    if (!(targetMedianLinear > 0.0F) || !std::isfinite(targetMedianLinear)) {
        throw std::invalid_argument(
            "target median luminance must be finite and positive");
    }
    return std::log2(targetMedianLinear) - stats.medianEV;
}

}  // namespace latent::render
