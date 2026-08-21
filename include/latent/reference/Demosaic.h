#pragma once

#include "latent/imaging/RawFrame.h"
#include "latent/imaging/Types.h"
#include "latent/reference/RawNormalize.h"

#include <array>
#include <cstddef>
#include <vector>

namespace latent::reference {

enum class DemosaicMethod : std::uint8_t {
    BaselineBoxAverage,
    MalvarHeCutler2004,
};

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

[[nodiscard]] RgbImageF32 demosaicSensorLinear(
    const SensorLinearFrameF32& frame,
    const std::array<float, 4>& whiteBalanceGains,
    DemosaicMethod method);

}  // namespace latent::reference
