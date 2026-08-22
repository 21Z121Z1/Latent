#include "latent/render/AcesToneScale.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace latent::render {

// Formula transcription based on:
//   ACES Project, Lib.Academy.Tonescale.ctl (ACES 2)
//   SPDX-License-Identifier: Apache-2.0
//   Copyright Contributors to the ACES Project.
//
// The implementation is kept isolated as a scalar primitive so Latent does
// not mislabel a partial RGB application as the complete ACES Output Transform.
AcesToneScaleParams makeAcesToneScaleParams(float peakLuminanceNits) {
    if (!std::isfinite(peakLuminanceNits) || peakLuminanceNits < 100.0F ||
        peakLuminanceNits > 10000.0F) {
        throw std::invalid_argument(
            "ACES tonescale peak luminance must be in [100, 10000] nits");
    }

    constexpr float normalizedWhite = 100.0F;
    constexpr float contrast = 1.15F;
    constexpr float middleGrey = 0.18F;
    constexpr float middleGreyOutputNits = 10.013F;
    constexpr float greyChangePerStop = 0.14F;
    constexpr float toe = 0.04F;
    constexpr float rHitMin = 128.0F;
    constexpr float rHitMax = 896.0F;

    const float normalizedPeak = peakLuminanceNits / normalizedWhite;
    const float rHit =
        rHitMin + (rHitMax - rHitMin) *
                      (std::log(normalizedPeak) /
                       std::log(10000.0F / normalizedWhite));
    const float m0 = normalizedPeak;
    const float m1 =
        0.5F * (m0 + std::sqrt(m0 * (m0 + 4.0F * toe)));
    const float rOverM1 = rHit / m1;
    const float u = std::pow(rOverM1 / (rOverM1 + 1.0F), contrast);
    const float m = m1 / u;
    const float whiteStops = std::log(normalizedPeak) / std::log(2.0F);
    const float greyTarget =
        (middleGreyOutputNits / normalizedWhite) *
        (1.0F + whiteStops * greyChangePerStop);
    const float greyPreFlare =
        0.5F * (greyTarget +
                std::sqrt(greyTarget * (greyTarget + 4.0F * toe)));
    const float greyPower =
        std::pow(greyPreFlare / m, 1.0F / contrast);
    const float greyIntersection =
        -(m1 * greyPower) / (greyPower - 1.0F);
    const float w2 = middleGrey / greyIntersection;
    const float s2 = w2 * m1;
    const float u2 =
        std::pow(rOverM1 / (rOverM1 + w2), contrast);
    const float m2 = m1 / u2;

    if (!std::isfinite(s2) || !std::isfinite(u2) || !std::isfinite(m2) ||
        !(s2 > 0.0F) || !(u2 > 0.0F) || !(m2 > 0.0F)) {
        throw std::runtime_error("ACES tonescale parameterization failed");
    }

    AcesToneScaleParams params{};
    params.peakLuminanceNits = peakLuminanceNits;
    params.normalizedWhiteNits = normalizedWhite;
    params.contrast = contrast;
    params.toe = toe;
    params.s2 = s2;
    params.u2 = u2;
    params.m2 = m2;
    params.forwardLimit = 8.0F * rHit;
    params.inverseLimit =
        peakLuminanceNits / (u2 * normalizedWhite);
    params.logPeak = std::log10(normalizedPeak);
    return params;
}

float acesToneScaleForward(
    float sceneValue,
    const AcesToneScaleParams& params) {
    if (!std::isfinite(sceneValue) || sceneValue < 0.0F) {
        throw std::invalid_argument(
            "ACES tonescale input must be finite and non-negative");
    }
    if (!std::isfinite(params.m2) || !std::isfinite(params.s2) ||
        !std::isfinite(params.contrast) || !std::isfinite(params.toe) ||
        !(params.m2 > 0.0F) || !(params.s2 > 0.0F) ||
        !(params.contrast > 0.0F) || params.toe < 0.0F) {
        throw std::invalid_argument("invalid ACES tonescale parameters");
    }

    const float f =
        params.m2 *
        std::pow(sceneValue / (sceneValue + params.s2), params.contrast);
    const float h = std::max(0.0F, f * f / (f + params.toe));
    return h * params.normalizedWhiteNits;
}

}  // namespace latent::render
