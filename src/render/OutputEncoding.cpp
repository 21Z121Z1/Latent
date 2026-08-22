#include "latent/render/OutputEncoding.h"

#include <cmath>
#include <stdexcept>

namespace latent::render {
namespace {

void requireNormalized(float value, const char* name) {
    if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
        throw std::invalid_argument(
            std::string(name) + " must be finite and in [0, 1]");
    }
}

// SMPTE ST 2084 constants. The same values are used by the Apache-2.0 ACES 2
// reference DisplayEncoding library (Lib.Academy.DisplayEncoding.ctl).
constexpr double kPqM1 = 0.1593017578125;  // 2610 / 16384
constexpr double kPqM2 = 78.84375;         // 2523 / 32
constexpr double kPqC1 = 0.8359375;        // 3424 / 4096
constexpr double kPqC2 = 18.8515625;       // 2413 / 128
constexpr double kPqC3 = 18.6875;          // 2392 / 128
constexpr double kPqPeakNits = 10000.0;

}  // namespace

float encodeSrgb(float linear) {
    requireNormalized(linear, "sRGB linear input");
    if (linear <= 0.0031308F) {
        return 12.92F * linear;
    }
    return 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
}

float decodeSrgb(float encoded) {
    requireNormalized(encoded, "sRGB encoded input");
    if (encoded <= 0.04045F) {
        return encoded / 12.92F;
    }
    return std::pow((encoded + 0.055F) / 1.055F, 2.4F);
}

float encodePqFromNits(float luminanceNits) {
    if (!std::isfinite(luminanceNits) || luminanceNits < 0.0F ||
        luminanceNits > static_cast<float>(kPqPeakNits)) {
        throw std::invalid_argument(
            "PQ luminance input must be finite and in [0, 10000] nits");
    }

    const double normalized =
        static_cast<double>(luminanceNits) / kPqPeakNits;
    const double powered = std::pow(normalized, kPqM1);
    const double ratio =
        (kPqC1 + kPqC2 * powered) / (1.0 + kPqC3 * powered);
    return static_cast<float>(std::pow(ratio, kPqM2));
}

float decodePqToNits(float encoded) {
    requireNormalized(encoded, "PQ encoded input");

    const double powered =
        std::pow(static_cast<double>(encoded), 1.0 / kPqM2);
    const double numerator = std::max(powered - kPqC1, 0.0);
    const double denominator = kPqC2 - kPqC3 * powered;
    if (!(denominator > 0.0)) {
        throw std::invalid_argument("PQ input is outside the decodable domain");
    }
    const double normalized =
        std::pow(numerator / denominator, 1.0 / kPqM1);
    return static_cast<float>(normalized * kPqPeakNits);
}

}  // namespace latent::render
