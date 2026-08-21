#include "latent/reference/SensorLinearOps.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace latent::reference {
namespace {

constexpr std::size_t kChannelCount = 4;

struct NeighborOffset {
    std::int32_t dx;
    std::int32_t dy;
};

// Same-CFA-channel neighbors of a Bayer sample: the four diagonal pixels at
// distance one and the four axial pixels at distance two.
constexpr std::array<NeighborOffset, 8> kSameChannelOffsets{{
    {-1, -1}, {1, -1}, {-1, 1}, {1, 1},
    {-2, 0}, {2, 0}, {0, -2}, {0, 2},
}};

float sampleAt(const SensorLinearFrameF32& frame, std::uint32_t x, std::uint32_t y) noexcept {
    return frame.samples[static_cast<std::size_t>(y) * frame.extent.width + x];
}

void validateFrame(const SensorLinearFrameF32& frame) {
    if (frame.extent.width == 0 || frame.extent.height == 0) {
        throw std::invalid_argument("sensor-linear operations require a non-empty frame");
    }
    if (frame.samples.size() !=
        static_cast<std::size_t>(frame.extent.pixelCount())) {
        throw std::invalid_argument("sensor-linear sample count must match its extent");
    }
}

std::vector<float> sameChannelNeighborValues(
    const SensorLinearFrameF32& frame,
    const imaging::DefectPixel& pixel,
    imaging::CfaChannel channel) {
    std::vector<float> values;
    for (const auto& offset : kSameChannelOffsets) {
        const auto nx = static_cast<std::int64_t>(pixel.x) + offset.dx;
        const auto ny = static_cast<std::int64_t>(pixel.y) + offset.dy;
        if (nx < 0 || ny < 0 ||
            nx >= static_cast<std::int64_t>(frame.extent.width) ||
            ny >= static_cast<std::int64_t>(frame.extent.height)) {
            continue;
        }
        const auto ux = static_cast<std::uint32_t>(nx);
        const auto uy = static_cast<std::uint32_t>(ny);
        if (imaging::cfaChannelAt(frame.cfa, ux, uy) != channel) {
            continue;
        }
        values.push_back(sampleAt(frame, ux, uy));
    }
    return values;
}

float medianOf(std::vector<float> values) {
    std::sort(values.begin(), values.end());
    const auto count = values.size();
    if (count % 2U == 1U) {
        return values[count / 2U];
    }
    return (values[count / 2U - 1U] + values[count / 2U]) * 0.5F;
}

}  // namespace

SensorLinearFrameF32 correctDefects(
    const SensorLinearFrameF32& frame,
    const std::vector<imaging::DefectPixel>& defects) {
    validateFrame(frame);

    SensorLinearFrameF32 result = frame;
    for (const auto& defect : defects) {
        if (defect.x >= frame.extent.width || defect.y >= frame.extent.height) {
            throw std::invalid_argument("defect coordinate lies outside the frame");
        }

        const auto channel = imaging::cfaChannelAt(frame.cfa, defect.x, defect.y);
        auto neighbors = sameChannelNeighborValues(frame, defect, channel);
        if (neighbors.empty()) {
            throw std::invalid_argument(
                "defect has no same-channel neighbor available for replacement");
        }

        result.samples[static_cast<std::size_t>(defect.y) * frame.extent.width +
                       defect.x] = medianOf(std::move(neighbors));
    }

    return result;
}

std::vector<imaging::DefectPixel> detectDefectCandidates(
    const SensorLinearFrameF32& frame,
    const DefectDetectionConfig& config,
    const imaging::NoiseModel* noise) {
    validateFrame(frame);
    if (!config.enabled) {
        return {};
    }
    if (!std::isfinite(config.sigmaMultiplier) || config.sigmaMultiplier <= 0.0F) {
        throw std::invalid_argument("sigmaMultiplier must be finite and positive");
    }
    if (!std::isfinite(config.minAbsoluteDelta) || config.minAbsoluteDelta < 0.0F) {
        throw std::invalid_argument("minAbsoluteDelta must be finite and non-negative");
    }

    std::vector<imaging::DefectPixel> candidates;
    for (std::uint32_t y = 0; y < frame.extent.height; ++y) {
        for (std::uint32_t x = 0; x < frame.extent.width; ++x) {
            const imaging::DefectPixel pixel{x, y};
            const auto channel = imaging::cfaChannelAt(frame.cfa, x, y);
            auto neighbors = sameChannelNeighborValues(frame, pixel, channel);
            if (neighbors.size() < 3U) {
                continue;
            }

            const float value = sampleAt(frame, x, y);
            const float median = medianOf(neighbors);
            const float delta = std::fabs(value - median);

            float sigma = 0.0F;
            if (noise != nullptr) {
                const auto index = static_cast<std::size_t>(channel);
                const float variance =
                    noise->shot[index] * value * value + noise->read[index];
                sigma = variance > 0.0F ? std::sqrt(variance) : 0.0F;
            }

            const float noiseThreshold = config.sigmaMultiplier * sigma;
            const bool exceedsNoise = delta > noiseThreshold;
            const bool exceedsAbsolute = delta > config.minAbsoluteDelta;
            if (exceedsNoise && exceedsAbsolute) {
                candidates.push_back(pixel);
            }
        }
    }

    return candidates;
}

float lensShadingGainAt(
    const imaging::LensShadingMap& map,
    const imaging::Extent& extent,
    imaging::CfaPattern cfa,
    std::uint32_t x,
    std::uint32_t y) {
    const auto validation = imaging::validateLensShadingMap(map);
    if (!validation.valid) {
        throw std::invalid_argument(validation.message);
    }
    if (extent.width == 0 || extent.height == 0 || x >= extent.width || y >= extent.height) {
        throw std::invalid_argument("position lies outside the frame extent");
    }

    const bool uniformX = map.gridColumns == 1U;
    const bool uniformY = map.gridRows == 1U;
    const float scaleX =
        uniformX ? 0.0F : static_cast<float>(map.gridColumns - 1U) /
                               static_cast<float>(extent.width - 1U);
    const float scaleY =
        uniformY ? 0.0F : static_cast<float>(map.gridRows - 1U) /
                               static_cast<float>(extent.height - 1U);

    const float gridY = uniformY ? 0.0F : static_cast<float>(y) * scaleY;
    const auto gy0 = static_cast<std::uint32_t>(gridY);
    const auto gy1 = std::min(gy0 + 1U, map.gridRows - 1U);
    const float fy = gridY - static_cast<float>(gy0);

    const float gridX = uniformX ? 0.0F : static_cast<float>(x) * scaleX;
    const auto gx0 = static_cast<std::uint32_t>(gridX);
    const auto gx1 = std::min(gx0 + 1U, map.gridColumns - 1U);
    const float fx = gridX - static_cast<float>(gx0);

    const auto channel = static_cast<std::size_t>(imaging::cfaChannelAt(cfa, x, y));

    const auto gainAt = [&](std::uint32_t gy, std::uint32_t gx) {
        const auto base = (static_cast<std::size_t>(gy) * map.gridColumns +
                           static_cast<std::size_t>(gx)) *
                          kChannelCount;
        return map.gains[base + channel];
    };

    const float g00 = gainAt(gy0, gx0);
    const float g10 = gainAt(gy0, gx1);
    const float g01 = gainAt(gy1, gx0);
    const float g11 = gainAt(gy1, gx1);

    const float top = g00 + (g10 - g00) * fx;
    const float bottom = g01 + (g11 - g01) * fx;
    return top + (bottom - top) * fy;
}

SensorLinearFrameF32 applyLensShading(
    const SensorLinearFrameF32& frame,
    const imaging::LensShadingMap& map) {
    validateFrame(frame);

    SensorLinearFrameF32 result = frame;
    for (std::uint32_t y = 0; y < frame.extent.height; ++y) {
        for (std::uint32_t x = 0; x < frame.extent.width; ++x) {
            const float gain =
                lensShadingGainAt(map, frame.extent, frame.cfa, x, y);
            const auto index =
                static_cast<std::size_t>(y) * frame.extent.width + x;
            result.samples[index] = frame.samples[index] * gain;
        }
    }

    return result;
}

}  // namespace latent::reference
