#pragma once

#include "latent/imaging/RawFrame.h"

namespace latent::runtime {

// Transport channel mapping for a view whose origin differs from sensor origin.
// The indices map each canonical channel in the view to the original metadata.
struct CfaTransportMap {
    imaging::CfaPattern pattern{};
    std::array<std::size_t, 4> layoutIndex{};  // row-major 2x2 sensor layout
    std::array<std::size_t, 4> sensorChannel{}; // R, green-even, green-odd, B
};
[[nodiscard]] CfaTransportMap cfaTransportMap(imaging::CfaPattern sensorPattern,
                                             std::uint32_t originX, std::uint32_t originY);

} // namespace latent::runtime
