#pragma once

#include "latent/imaging/RawFrame.h"
#include "latent/imaging/Types.h"
#include "latent/reference/RawNormalize.h"

#include <array>
#include <cstddef>
#include <vector>

namespace latent::reference {

using imaging::DemosaicMethod;

struct RgbImageF32 {
    imaging::Extent extent{};
    std::vector<float> rgb;

    [[nodiscard]] std::uint64_t pixelCount() const noexcept {
        return extent.pixelCount();
    }
};

[[nodiscard]] constexpr std::size_t rgbChannelIndex(imaging::CfaChannel channel) noexcept {
    switch (channel) {
        case imaging::CfaChannel::R:
            return 0;
        case imaging::CfaChannel::G0:
        case imaging::CfaChannel::G1:
            return 1;
        case imaging::CfaChannel::B:
            return 2;
    }
    return 0;
}

struct DemosaicTap {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    imaging::CfaChannel channel = imaging::CfaChannel::R;
    float weight = 0.0F;
};

[[nodiscard]] RgbImageF32 demosaicSensorLinear(
    const SensorLinearFrameF32& frame,
    const std::array<float, 4>& whiteBalanceGains,
    DemosaicMethod method);

// Exact linear taps (position offsets, source CFA channels, weights) that the
// given demosaic method uses to produce one output channel at one pixel.
// Weights include white-balance gains; identity taps carry weight 1.
[[nodiscard]] std::vector<DemosaicTap> demosaicTapWeights(
    const imaging::Extent& extent,
    imaging::CfaPattern cfa,
    std::uint32_t x,
    std::uint32_t y,
    std::size_t rgbChannel,
    const std::array<float, 4>& whiteBalanceGains,
    DemosaicMethod method);

}  // namespace latent::reference
