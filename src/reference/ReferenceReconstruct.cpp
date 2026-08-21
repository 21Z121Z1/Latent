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
    for (const auto coefficient : config.cameraToAcescg.values) {
        if (!std::isfinite(coefficient)) {
            throw std::invalid_argument("cameraToAcescg matrix must contain only finite values");
        }
    }
}

std::size_t cfaIndex(imaging::CfaChannel channel) {
    return static_cast<std::size_t>(channel);
}

std::size_t rgbIndex(imaging::CfaChannel channel) {
    switch (channel) {
        case imaging::CfaChannel::R: return 0;
        case imaging::CfaChannel::G0:
        case imaging::CfaChannel::G1: return 1;
        case imaging::CfaChannel::B: return 2;
    }
    return 0;
}

float weightedSample(
    const SensorLinearFrameF32& frame,
    const ReconstructionConfig& config,
    std::uint32_t x,
    std::uint32_t y) {
    const auto index = static_cast<std::size_t>(y) * frame.extent.width + x;
    const auto channel = imaging::cfaChannelAt(frame.cfa, x, y);
    return frame.samples[index] * config.whiteBalanceGains[cfaIndex(channel)];
}

std::array<float, 3> demosaicPixel(
    const SensorLinearFrameF32& frame,
    const ReconstructionConfig& config,
    std::uint32_t x,
    std::uint32_t y) {
    std::array<float, 3> sum{0.0F, 0.0F, 0.0F};
    std::array<std::uint32_t, 3> count{0U, 0U, 0U};

    const auto x0 = x == 0 ? 0 : x - 1;
    const auto y0 = y == 0 ? 0 : y - 1;
    const auto x1 = x + 1 < frame.extent.width ? x + 1 : x;
    const auto y1 = y + 1 < frame.extent.height ? y + 1 : y;

    for (std::uint32_t yy = y0; yy <= y1; ++yy) {
        for (std::uint32_t xx = x0; xx <= x1; ++xx) {
            const auto channel = imaging::cfaChannelAt(frame.cfa, xx, yy);
            const auto target = rgbIndex(channel);
            sum[target] += weightedSample(frame, config, xx, yy);
            ++count[target];
        }
    }

    const auto centerChannel = imaging::cfaChannelAt(frame.cfa, x, y);
    const auto centerRgb = rgbIndex(centerChannel);
    sum[centerRgb] = weightedSample(frame, config, x, y);
    count[centerRgb] = 1U;

    std::array<float, 3> rgb{};
    for (std::size_t c = 0; c < rgb.size(); ++c) {
        if (count[c] == 0U) {
            throw std::runtime_error("demosaic neighborhood contains no sample for a required channel");
        }
        rgb[c] = sum[c] / static_cast<float>(count[c]);
    }
    return rgb;
}

}  // namespace

imaging::SceneFrame reconstructSingleRaw(
    const imaging::RawFrame& raw,
    const ReconstructionConfig& config) {
    validateConfig(config);
    const auto sensor = normalizeRaw(raw);
    if (sensor.extent.width < 2U || sensor.extent.height < 2U) {
        throw std::invalid_argument("baseline Bayer demosaic requires at least a 2x2 RAW extent");
    }

    imaging::SceneFrame scene{};
    scene.sourceRawId = raw.id;
    scene.image.extent = sensor.extent;
    scene.image.rgb.resize(static_cast<std::size_t>(sensor.extent.pixelCount()) * 3U);
    scene.sceneScaleEV = config.sceneScaleEV;
    const float sceneScale = std::exp2(config.sceneScaleEV);
    if (!std::isfinite(sceneScale)) {
        throw std::invalid_argument("sceneScaleEV is outside the finite FP32 coordinate range");
    }
    scene.whiteBalanceConfidence = config.whiteBalanceConfidence;

    for (std::uint32_t y = 0; y < sensor.extent.height; ++y) {
        for (std::uint32_t x = 0; x < sensor.extent.width; ++x) {
            const auto cameraRgb = demosaicPixel(sensor, config, x, y);
            const auto sceneRgb = config.cameraToAcescg.apply(cameraRgb);
            const auto base = (static_cast<std::size_t>(y) * sensor.extent.width + x) * 3U;
            scene.image.rgb[base] = sceneRgb[0] * sceneScale;
            scene.image.rgb[base + 1U] = sceneRgb[1] * sceneScale;
            scene.image.rgb[base + 2U] = sceneRgb[2] * sceneScale;
        }
    }

    return scene;
}

}  // namespace latent::reference
