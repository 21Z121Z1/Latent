#include "latent/reference/ReferenceReconstruct.h"

#include "latent/reference/NoisePropagation.h"
#include "latent/reference/RawNormalize.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

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

std::vector<imaging::DefectPixel> collectDefects(
    const imaging::RawFrame& raw,
    const SensorLinearFrameF32& sensor,
    const ReconstructionConfig& config,
    const imaging::NoiseModel* normalizedNoise) {
    std::vector<imaging::DefectPixel> defects;
    if (config.defectCorrection == DefectCorrectionMode::FromMetadataMap ||
        config.defectCorrection == DefectCorrectionMode::DetectAndCorrect) {
        defects = raw.defects;
    }

    if (config.defectCorrection == DefectCorrectionMode::DetectAndCorrect &&
        config.defectDetection.enabled) {
        auto detected = detectDefectCandidates(sensor, config.defectDetection, normalizedNoise);
        defects.insert(defects.end(), detected.begin(), detected.end());
    }

    if (defects.size() > 1U) {
        std::sort(defects.begin(), defects.end(), [](const auto& a, const auto& b) {
            return a.y != b.y ? a.y < b.y : a.x < b.x;
        });
        defects.erase(
            std::unique(defects.begin(), defects.end(), [](const auto& a, const auto& b) {
                return a.x == b.x && a.y == b.y;
            }),
            defects.end());
    }

    return defects;
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

    std::optional<imaging::NoiseModel> normalizedNoise;
    if (raw.noiseProfile.usable()) {
        const auto noiseCheck = imaging::validateNoiseModel(*raw.noiseProfile.value);
        if (!noiseCheck.valid) {
            throw std::invalid_argument(noiseCheck.message);
        }
        normalizedNoise = normalizeNoiseModel(*raw.noiseProfile.value, sensor.levels);
    }

    auto working = sensor;
    if (config.defectCorrection != DefectCorrectionMode::Disabled) {
        const auto defects =
            collectDefects(raw, working, config, normalizedNoise ? &*normalizedNoise : nullptr);
        if (!defects.empty()) {
            working = correctDefects(working, defects);
        }
    }

    bool lensShadingApplied = false;
    if (config.applyLensShading && raw.lensShading.usable()) {
        working = applyLensShading(working, *raw.lensShading.value);
        lensShadingApplied = true;
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
        working, config.whiteBalanceGains, config.demosaicMethod);

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

    if (config.propagateNoise && normalizedNoise.has_value()) {
        scene.propagatedNoise = buildPropagatedNoise(
            sensor,
            *normalizedNoise,
            config.whiteBalanceGains,
            lensShadingApplied,
            lensShadingApplied ? &*raw.lensShading.value : nullptr,
            cameraToScene,
            sceneScale,
            config.demosaicMethod);
    }

    return scene;
}

}  // namespace latent::reference
