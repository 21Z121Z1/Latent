#include "latent/reference/Demosaic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace latent::reference {
namespace {

constexpr std::size_t kWidth = 5;

constexpr std::size_t kernelIndex(std::size_t ky, std::size_t kx) noexcept {
    return ky * kWidth + kx;
}

float clampedSample(
    const std::vector<float>& samples,
    imaging::Extent extent,
    std::int64_t x,
    std::int64_t y) noexcept {
    const auto clampedX = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(x, 0, static_cast<std::int64_t>(extent.width) - 1));
    const auto clampedY = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(y, 0, static_cast<std::int64_t>(extent.height) - 1));
    return samples[static_cast<std::size_t>(clampedY) * extent.width + clampedX];
}

float applyKernel(
    const std::vector<float>& samples,
    imaging::Extent extent,
    std::uint32_t x,
    std::uint32_t y,
    const std::array<float, 25>& kernel) noexcept {
    float sum = 0.0F;
    for (std::size_t ky = 0; ky < kWidth; ++ky) {
        for (std::size_t kx = 0; kx < kWidth; ++kx) {
            const auto coefficient = kernel[kernelIndex(ky, kx)];
            if (coefficient == 0.0F) {
                continue;
            }
            const auto sample = clampedSample(
                samples,
                extent,
                static_cast<std::int64_t>(x) + static_cast<std::int64_t>(kx) - 2,
                static_cast<std::int64_t>(y) + static_cast<std::int64_t>(ky) - 2);
            sum += coefficient * sample;
        }
    }
    return sum;
}

// Malvar-He-Cutler (2004) 5x5 filters, coefficients scaled by 1/8.
constexpr std::array<float, 25> kGreenAtRedBlue{{
    0.0F, 0.0F, -0.125F, 0.0F, 0.0F,
    0.0F, 0.0F, 0.25F, 0.0F, 0.0F,
    -0.125F, 0.25F, 0.5F, 0.25F, -0.125F,
    0.0F, 0.0F, 0.25F, 0.0F, 0.0F,
    0.0F, 0.0F, -0.125F, 0.0F, 0.0F,
}};

constexpr std::array<float, 25> kTargetAtGreenRowAligned{{
    0.0F, 0.0F, 0.0625F, 0.0F, 0.0F,
    0.0F, -0.125F, 0.0F, -0.125F, 0.0F,
    -0.125F, 0.5F, 0.625F, 0.5F, -0.125F,
    0.0F, -0.125F, 0.0F, -0.125F, 0.0F,
    0.0F, 0.0F, 0.0625F, 0.0F, 0.0F,
}};

constexpr std::array<float, 25> kTargetAtGreenColumnAligned{{
    0.0F, 0.0F, -0.125F, 0.0F, 0.0F,
    0.0F, -0.125F, 0.5F, -0.125F, 0.0F,
    0.0625F, 0.0F, 0.625F, 0.0F, 0.0625F,
    0.0F, -0.125F, 0.5F, -0.125F, 0.0F,
    0.0F, 0.0F, -0.125F, 0.0F, 0.0F,
}};

constexpr std::array<float, 25> kTargetAtOppositeColor{{
    0.0F, 0.0F, -0.1875F, 0.0F, 0.0F,
    0.0F, 0.25F, 0.0F, 0.25F, 0.0F,
    -0.1875F, 0.0F, 0.75F, 0.0F, -0.1875F,
    0.0F, 0.25F, 0.0F, 0.25F, 0.0F,
    0.0F, 0.0F, -0.1875F, 0.0F, 0.0F,
}};

bool isGreen(imaging::CfaChannel channel) noexcept {
    return channel == imaging::CfaChannel::G0 || channel == imaging::CfaChannel::G1;
}

std::vector<float> applyWhiteBalanceGains(
    const SensorLinearFrameF32& frame,
    const std::array<float, 4>& whiteBalanceGains) {
    std::vector<float> gained(frame.samples.size());
    for (std::uint32_t y = 0; y < frame.extent.height; ++y) {
        for (std::uint32_t x = 0; x < frame.extent.width; ++x) {
            const auto index = static_cast<std::size_t>(y) * frame.extent.width + x;
            const auto channel = imaging::cfaChannelAt(frame.cfa, x, y);
            gained[index] =
                frame.samples[index] * whiteBalanceGains[static_cast<std::size_t>(channel)];
        }
    }
    return gained;
}

RgbImageF32 demosaicMalvarHeCutler(
    const SensorLinearFrameF32& frame,
    const std::array<float, 4>& whiteBalanceGains) {
    const auto bayer = applyWhiteBalanceGains(frame, whiteBalanceGains);

    RgbImageF32 result{};
    result.extent = frame.extent;
    result.rgb.resize(static_cast<std::size_t>(frame.extent.pixelCount()) * 3U);

    for (std::uint32_t y = 0; y < frame.extent.height; ++y) {
        for (std::uint32_t x = 0; x < frame.extent.width; ++x) {
            const auto index = static_cast<std::size_t>(y) * frame.extent.width + x;
            const auto center = imaging::cfaChannelAt(frame.cfa, x, y);
            const auto centerRgb = rgbChannelIndex(center);

            std::array<float, 3> pixel{};
            pixel[centerRgb] = bayer[index];

            for (std::size_t target = 0; target < 3; ++target) {
                if (target == centerRgb) {
                    continue;
                }

                if (isGreen(center)) {
                    const auto neighborX =
                        x + 1 < frame.extent.width ? x + 1 : (x > 0 ? x - 1 : x);
                    const auto horizontalChannel =
                        imaging::cfaChannelAt(frame.cfa, neighborX, y);
                    const bool rowAligned = rgbChannelIndex(horizontalChannel) == target;
                    pixel[target] = applyKernel(
                        bayer,
                        frame.extent,
                        x,
                        y,
                        rowAligned ? kTargetAtGreenRowAligned
                                   : kTargetAtGreenColumnAligned);
                    continue;
                }

                if (target == 1) {
                    pixel[target] =
                        applyKernel(bayer, frame.extent, x, y, kGreenAtRedBlue);
                    continue;
                }

                pixel[target] =
                    applyKernel(bayer, frame.extent, x, y, kTargetAtOppositeColor);
            }

            const auto base = index * 3U;
            result.rgb[base] = pixel[0];
            result.rgb[base + 1U] = pixel[1];
            result.rgb[base + 2U] = pixel[2];
        }
    }

    return result;
}

RgbImageF32 demosaicBaselineBoxAverage(
    const SensorLinearFrameF32& frame,
    const std::array<float, 4>& whiteBalanceGains) {
    RgbImageF32 result{};
    result.extent = frame.extent;
    result.rgb.resize(static_cast<std::size_t>(frame.extent.pixelCount()) * 3U);

    for (std::uint32_t y = 0; y < frame.extent.height; ++y) {
        for (std::uint32_t x = 0; x < frame.extent.width; ++x) {
            std::array<float, 3> sum{0.0F, 0.0F, 0.0F};
            std::array<std::uint32_t, 3> count{0U, 0U, 0U};

            const auto x0 = x == 0 ? 0 : x - 1;
            const auto y0 = y == 0 ? 0 : y - 1;
            const auto x1 = x + 1 < frame.extent.width ? x + 1 : x;
            const auto y1 = y + 1 < frame.extent.height ? y + 1 : y;

            for (std::uint32_t yy = y0; yy <= y1; ++yy) {
                for (std::uint32_t xx = x0; xx <= x1; ++xx) {
                    const auto channel = imaging::cfaChannelAt(frame.cfa, xx, yy);
                    const auto target = rgbChannelIndex(channel);
                    const auto sampleIndex =
                        static_cast<std::size_t>(yy) * frame.extent.width + xx;
                    sum[target] +=
                        frame.samples[sampleIndex] *
                        whiteBalanceGains[static_cast<std::size_t>(channel)];
                    ++count[target];
                }
            }

            const auto centerChannel = imaging::cfaChannelAt(frame.cfa, x, y);
            const auto centerRgb = rgbChannelIndex(centerChannel);
            const auto centerIndex =
                static_cast<std::size_t>(y) * frame.extent.width + x;
            sum[centerRgb] =
                frame.samples[centerIndex] *
                whiteBalanceGains[static_cast<std::size_t>(centerChannel)];
            count[centerRgb] = 1U;

            const auto base = centerIndex * 3U;
            for (std::size_t c = 0; c < 3; ++c) {
                result.rgb[base + c] = sum[c] / static_cast<float>(count[c]);
            }
        }
    }

    return result;
}

}  // namespace

RgbImageF32 demosaicSensorLinear(
    const SensorLinearFrameF32& frame,
    const std::array<float, 4>& whiteBalanceGains,
    DemosaicMethod method) {
    if (frame.extent.width == 0 || frame.extent.height == 0) {
        throw std::invalid_argument("demosaic requires a non-empty sensor-linear frame");
    }
    if (frame.samples.size() !=
        static_cast<std::size_t>(frame.extent.pixelCount())) {
        throw std::invalid_argument("sensor-linear sample count must match its extent");
    }
    for (const auto gain : whiteBalanceGains) {
        if (!std::isfinite(gain) || gain <= 0.0F) {
            throw std::invalid_argument("white-balance gains must be finite and positive");
        }
    }

    switch (method) {
        case DemosaicMethod::BaselineBoxAverage:
            return demosaicBaselineBoxAverage(frame, whiteBalanceGains);
        case DemosaicMethod::MalvarHeCutler2004:
            return demosaicMalvarHeCutler(frame, whiteBalanceGains);
    }
    throw std::invalid_argument("unknown demosaic method");
}

}  // namespace latent::reference
