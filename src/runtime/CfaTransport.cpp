#include "latent/runtime/CfaTransport.h"
#include <stdexcept>

namespace latent::runtime {
CfaTransportMap cfaTransportMap(imaging::CfaPattern sensorPattern,
    std::uint32_t originX, std::uint32_t originY) {
    using imaging::CfaChannel;
    using imaging::CfaPattern;
    if (static_cast<unsigned>(sensorPattern) > static_cast<unsigned>(CfaPattern::BGGR))
        throw std::invalid_argument("unsupported Bayer transport pattern");
    originX &= 1U; originY &= 1U;
    const auto color = [](CfaChannel c) { return c == CfaChannel::G1 ? CfaChannel::G0 : c; };
    CfaTransportMap map{};
    bool found = false;
    for (auto pattern : {CfaPattern::RGGB, CfaPattern::GRBG, CfaPattern::GBRG, CfaPattern::BGGR}) {
        bool matches = true;
        for (std::uint32_t y = 0; y < 2; ++y) for (std::uint32_t x = 0; x < 2; ++x) {
            if (color(imaging::cfaChannelAt(pattern, x, y)) !=
                color(imaging::cfaChannelAt(sensorPattern, x + originX, y + originY))) matches = false;
        }
        if (matches) { map.pattern = pattern; found = true; break; }
    }
    if (!found) throw std::invalid_argument("CFA view is not a Bayer phase");
    for (std::uint32_t y = 0; y < 2; ++y) for (std::uint32_t x = 0; x < 2; ++x) {
        const auto channel = static_cast<std::size_t>(imaging::cfaChannelAt(map.pattern, x, y));
        map.layoutIndex[channel] = ((y + originY) & 1U) * 2U + ((x + originX) & 1U);
        map.sensorChannel[channel] = static_cast<std::size_t>(imaging::cfaChannelAt(sensorPattern, x + originX, y + originY));
    }
    return map;
}
} // namespace latent::runtime
