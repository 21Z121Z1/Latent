#pragma once

#include "latent/imaging/Types.h"

#include <array>

namespace latent::imaging {

struct ChromaticityXY {
    float x = 0.0F;
    float y = 0.0F;
};

inline constexpr ChromaticityXY kIlluminantD50{0.34567F, 0.35850F};
inline constexpr ChromaticityXY kIlluminantD65{0.31271F, 0.32902F};
inline constexpr ChromaticityXY kAcesWhite{0.32168F, 0.33767F};

inline constexpr ChromaticityXY kAp0Red{0.7347F, 0.2653F};
inline constexpr ChromaticityXY kAp0Green{0.0000F, 1.0000F};
inline constexpr ChromaticityXY kAp0Blue{0.0001F, -0.0770F};

inline constexpr ChromaticityXY kAp1Red{0.713F, 0.293F};
inline constexpr ChromaticityXY kAp1Green{0.165F, 0.830F};
inline constexpr ChromaticityXY kAp1Blue{0.128F, 0.044F};

inline constexpr Matrix3f kAp0ToAp1{{
     1.4514393161F, -0.2365107469F, -0.2149285693F,
    -0.0765537734F,  1.1762296998F, -0.0996759264F,
     0.0083161484F, -0.0060324498F,  0.9977163014F,
}};

inline constexpr Matrix3f kAp1ToAp0{{
     0.6954522414F,  0.1406786965F,  0.1638690622F,
     0.0447945634F,  0.8596711185F,  0.0955343182F,
    -0.0055256437F,  0.0040252103F,  1.0015003924F,
}};

using Xyz = std::array<float, 3>;

[[nodiscard]] constexpr Xyz xyToXyz(ChromaticityXY xy) noexcept {
    return {xy.x / xy.y, 1.0F, (1.0F - xy.x - xy.y) / xy.y};
}

[[nodiscard]] constexpr ChromaticityXY xyzToChromaticity(const Xyz& xyz) noexcept {
    const float sum = xyz[0] + xyz[1] + xyz[2];
    return {xyz[0] / sum, xyz[1] / sum};
}

[[nodiscard]] Matrix3f rgbPrimariesToXyzMatrix(
    ChromaticityXY red,
    ChromaticityXY green,
    ChromaticityXY blue,
    ChromaticityXY white);

[[nodiscard]] Matrix3f bradfordAdaptation(ChromaticityXY sourceWhite, ChromaticityXY targetWhite);

struct CorrelatedTemperature {
    float cctKelvin = 0.0F;
    float tint = 0.0F;
};

[[nodiscard]] CorrelatedTemperature xyToCorrelatedTemperature(ChromaticityXY xy);

[[nodiscard]] float xyToCctMcCamy(ChromaticityXY xy) noexcept;

}  // namespace latent::imaging
