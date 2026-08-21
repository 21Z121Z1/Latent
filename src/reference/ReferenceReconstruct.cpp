#include "latent/reference/ReferenceReconstruct.h"

#include "latent/reference/RawNormalize.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace latent::reference {
namespace {

void validateConfig(const ReconstructionConfig& config) {
    if (!std::isfinite(config.sceneScaleEV)) {
        throw std::invalid_argument("sceneScaleEV must be finite");
    }
    if (!std::isfinite(config.whiteBalanceConfidence) ||
        config.whiteBalanceConfidence < 0.0F || config.whiteBalanceConfidence > 1.0F) {
        throw std::invalid_argument("whiteBalanceConfidence must be finite and within [0, 1]");
    }
    for (const auto gain : config.whiteBalanceGains) {
        if (!std::isfinite(gain) || gain <= 0.0F) {
            throw std::invalid_argument("white-balance gains must be finite and positive");
        }
    }
    if (config.colorPath == ColorPath::ExplicitMatrix) {
        for (const auto coefficient : config.cameraToAcescg.values) {
            if (!std::isfinite(coefficient)) {
                throw std::invalid_argument("cameraToAcescg matrix must contain only finite values");
            }
        }
    }
}

}  // namespace

imaging::SceneFrame reconstructSingleRaw(
    const imaging::RawFrame& raw,
    const ReconstructionConfig& config) {
    validateConfig(config);
    const auto sensor = normalizeRaw(raw);
    if (sensor.extent.width < 2U || sensor.extent.height < 2U) {
        throw std::invalid_argument("Bayer demosaic requires at least a 2x2 RAW extent");
    }

    imaging::Matrix3f cameraToScene = imaging::Matrix3f::identity();
    if (config.colorPath == ColorPath::DngProfile) {
        const auto profileCheck = validateDngProfile(config.dngProfile);
        if (!profileCheck.valid) {
            throw std::invalid_argument(profileCheck.message);
        }
        const auto cameraToXyzD50 =
            cameraToXyzD50Matrix(config.whiteBalanceXy, config.dngProfile);
        cameraToScene = xyzD50ToAcescgMatrix().multiplied(cameraToXyzD50.matrix);
    } else {
        cameraToScene = config.cameraToAcescg;
    }

    const auto rgb = demosaicSensorLinear(
        sensor, config.whiteBalanceGains, config.demosaicMethod);

    imaging::SceneFrame scene{};
    scene.sourceRawId = raw.id;
    scene.image.extent = rgb.extent;
    scene.image.rgb.resize(static_cast<std::size_t>(rgb.extent.pixelCount()) * 3U);
    scene.sceneScaleEV = config.sceneScaleEV;
    const float sceneScale = std::exp2(config.sceneScaleEV);
    if (!std::isfinite(sceneScale)) {
        throw std::invalid_argument("sceneScaleEV is outside the finite FP32 coordinate range");
    }
    scene.whiteBalanceConfidence = config.whiteBalanceConfidence;

    for (std::size_t i = 0; i < scene.image.rgb.size(); i += 3U) {
        const std::array<float, 3> cameraRgb{rgb.rgb[i], rgb.rgb[i + 1U], rgb.rgb[i + 2U]};
        const auto sceneRgb = cameraToScene.apply(cameraRgb);
        scene.image.rgb[i] = sceneRgb[0] * sceneScale;
        scene.image.rgb[i + 1U] = sceneRgb[1] * sceneScale;
        scene.image.rgb[i + 2U] = sceneRgb[2] * sceneScale;
    }

    return scene;
}

}  // namespace latent::reference
