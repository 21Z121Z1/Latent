#include "latent/reference/NoisePropagation.h"

#include "latent/reference/SensorLinearOps.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace latent::reference {
namespace {

constexpr std::size_t kChannelCount = 4;

}  // namespace

imaging::NoiseModel normalizeNoiseModel(
    const imaging::NoiseModel& rawCodeModel,
    const SelectedRawLevels& levels) {
    imaging::NoiseModel result{};

    for (std::size_t c = 0; c < kChannelCount; ++c) {
        const float black = levels.black.cfa[c];
        const float range = levels.white - black;
        if (!(range > 0.0F)) {
            throw std::invalid_argument("noise normalization requires white > black per channel");
        }

        result.shot[c] = rawCodeModel.shot[c] / range;
        result.read[c] =
            (rawCodeModel.shot[c] * black + rawCodeModel.read[c]) / (range * range);

        if (!std::isfinite(result.shot[c]) || !std::isfinite(result.read[c])) {
            throw std::invalid_argument("noise normalization produced non-finite coefficients");
        }
    }

    return result;
}

imaging::PropagatedNoise buildPropagatedNoise(
    const SensorLinearFrameF32& sensorFrame,
    const imaging::NoiseModel& normalizedModel,
    const std::array<float, 4>& whiteBalanceGains,
    bool lensShadingApplied,
    const imaging::LensShadingMap* lensShading,
    const imaging::Matrix3f& cameraToScene,
    float sceneScale,
    DemosaicMethod method) {
    if (sensorFrame.extent.width == 0 || sensorFrame.extent.height == 0) {
        throw std::invalid_argument("propagated noise requires a non-empty frame");
    }
    if (!(sceneScale > 0.0F) || !std::isfinite(sceneScale)) {
        throw std::invalid_argument("scene scale must be finite and positive");
    }
    for (std::size_t c = 0; c < kChannelCount; ++c) {
        if (!std::isfinite(normalizedModel.shot[c]) || normalizedModel.shot[c] < 0.0F ||
            !std::isfinite(normalizedModel.read[c]) || normalizedModel.read[c] < 0.0F) {
            throw std::invalid_argument("normalized noise coefficients must be finite and non-negative");
        }
        if (!std::isfinite(whiteBalanceGains[c]) || whiteBalanceGains[c] <= 0.0F) {
            throw std::invalid_argument("white-balance gains must be finite and positive");
        }
    }
    if (lensShadingApplied && lensShading == nullptr) {
        throw std::invalid_argument("lens shading map is required when it was applied");
    }

    const auto inverseCameraToScene = imaging::inverted(cameraToScene);
    if (!inverseCameraToScene.has_value()) {
        throw std::invalid_argument("camera-to-scene matrix must be invertible for noise propagation");
    }

    imaging::PropagatedNoise noise{};
    noise.valid = true;
    noise.extent = sensorFrame.extent;
    noise.cfa = sensorFrame.cfa;
    noise.demosaicMethod = method;
    noise.shot = normalizedModel.shot;
    noise.read = normalizedModel.read;
    noise.whiteBalanceGains = whiteBalanceGains;
    noise.lensShadingApplied = lensShadingApplied;
    if (lensShadingApplied) {
        noise.lensShading = *lensShading;
    }
    noise.sceneToCamera = *inverseCameraToScene;
    noise.sceneScale = sceneScale;

    return noise;
}

float propagatedSigma(
    const imaging::PropagatedNoise& noise,
    const SigmaQuery& query) {
    if (!noise.valid) {
        throw std::invalid_argument("propagated noise record is not valid");
    }
    if (query.x >= noise.extent.width || query.y >= noise.extent.height) {
        throw std::invalid_argument("sigma query position lies outside the extent");
    }
    if (query.rgbChannel > 2U) {
        throw std::invalid_argument("rgb channel must be 0, 1, or 2");
    }
    for (const auto value : query.sceneValueRgb) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("sigma query requires finite scene values");
        }
    }

    const auto taps = demosaicTapWeights(
        noise.extent, noise.cfa, query.x, query.y, query.rgbChannel,
        {1.0F, 1.0F, 1.0F, 1.0F}, noise.demosaicMethod);

    std::array<float, 3> scaledScene{};
    for (std::size_t c = 0; c < 3; ++c) {
        scaledScene[c] = query.sceneValueRgb[c] / noise.sceneScale;
    }
    const auto cameraEstimate = noise.sceneToCamera.apply(scaledScene);

    float variance = 0.0F;
    for (const auto& tap : taps) {
        const auto channel = static_cast<std::size_t>(tap.channel);
        const auto rgbOfTap = rgbChannelIndex(tap.channel);

        float gain = 1.0F;
        if (noise.lensShadingApplied) {
            gain = lensShadingGainAt(
                noise.lensShading, noise.extent, noise.cfa, tap.x, tap.y);
        }

        const float wbGain = noise.whiteBalanceGains[channel];
        const float effectiveGain = wbGain * gain;

        float signalEstimate = cameraEstimate[rgbOfTap] / effectiveGain;
        if (!std::isfinite(signalEstimate)) {
            signalEstimate = 0.0F;
        }

        const float shotVariance =
            std::max(noise.shot[channel] * signalEstimate, 0.0F);
        variance += tap.weight * tap.weight * effectiveGain * effectiveGain *
                    (shotVariance + noise.read[channel]);
    }

    if (!std::isfinite(variance) || variance < 0.0F) {
        throw std::runtime_error("propagated variance is degenerate");
    }
    return noise.sceneScale * std::sqrt(variance);
}

}  // namespace latent::reference
