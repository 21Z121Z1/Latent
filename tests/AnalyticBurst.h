#pragma once
#include "TemporalFixture.h"
#include <array>
#include <numbers>
#include <random>

namespace latent::test::analytic {
// Independent continuous irradiance; integrating each sinusoid over a unit
// square photosite is exact. No image warp, demosaic or alignment code generates
// the truth. Frequencies are radians per native sensor pixel.
inline float apertureSignal(float x, float y, std::size_t channel) {
    constexpr std::array<std::array<float, 4>, 7> waves{{
        {0.181F, 0.037F, 0.043F, 0.7F}, {0.057F, -0.193F, 0.037F, 1.3F},
        {0.113F, 0.127F, 0.028F, -0.4F}, {0.029F, 0.071F, 0.031F, 2.1F},
        {-0.149F, 0.083F, 0.019F, 0.2F}, {0.077F, 0.013F, 0.017F, -1.7F},
        {0.019F, -0.101F, 0.021F, 0.9F}}};
    float value = 0.29F;
    for (const auto& w : waves) {
        const float aperture = (std::sin(w[0] * 0.5F) / (w[0] * 0.5F)) *
                               (std::sin(w[1] * 0.5F) / (w[1] * 0.5F));
        value += w[2] * aperture * std::sin(w[0] * x + w[1] * y + w[3]);
    }
    constexpr std::array<float, 4> response{0.81F, 1.0F, 0.98F, 0.73F};
    return value * response.at(channel);
}
inline reference::NormalizedRaw observation(float dx, float dy,
    imaging::CfaPattern cfa = imaging::CfaPattern::RGGB,
    imaging::Extent extent = {129, 113}, float brightness = 1.0F,
    float shot = 0, float read = 0, std::uint32_t seed = 1U) {
    reference::NormalizedRaw out{}; out.extent = extent; out.cfa = cfa;
    out.samples.resize(extent.pixelCount());
    std::mt19937 engine(seed);
    std::normal_distribution<float> gaussian(0, 1);
    for (std::uint32_t y = 0; y < extent.height; ++y) for (std::uint32_t x = 0; x < extent.width; ++x) {
        const auto channel = static_cast<std::size_t>(imaging::cfaChannelAt(cfa, x, y));
        const float mean = brightness * apertureSignal(static_cast<float>(x) - dx, static_cast<float>(y) - dy, channel);
        float value = mean;
        if (shot > 0) value = static_cast<float>(std::poisson_distribution<int>(static_cast<double>(mean / shot))(engine)) * shot;
        if (read > 0) value += std::sqrt(read) * gaussian(engine);
        out.samples[static_cast<std::size_t>(y) * extent.width + x] = {value, shot * mean + read, 1, 1};
    }
    return out;
}
} // namespace latent::test::analytic
