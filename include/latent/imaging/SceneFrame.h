#pragma once

#include "latent/imaging/Noise.h"
#include "latent/imaging/Types.h"

#include <cstdint>
#include <vector>

namespace latent::imaging {

struct SceneImageF32 {
    Extent extent{};
    std::vector<float> rgb;

    [[nodiscard]] std::uint64_t pixelCount() const noexcept {
        return extent.pixelCount();
    }
};

struct SceneFrame {
    std::uint64_t sourceRawId = 0;
    SceneImageF32 image{};

    Primaries primaries = Primaries::ACEScgAP1;
    WhitePoint whitePoint = WhitePoint::D60;
    TransferFunction transfer = TransferFunction::Linear;
    ReferenceDomain reference = ReferenceDomain::Scene;
    RangeSemantics range = RangeSemantics::Unbounded;

    float sceneScaleEV = 0.0F;
    bool allowNegative = true;
    float whiteBalanceConfidence = 0.0F;

    PropagatedNoise propagatedNoise{};
};

}  // namespace latent::imaging
