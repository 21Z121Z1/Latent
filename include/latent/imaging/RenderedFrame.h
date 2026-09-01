#pragma once

#include "latent/imaging/SceneFrame.h"

#include <cstdint>
#include <vector>

namespace latent::imaging {

enum class RenderIntent : std::uint8_t {
    SDR,
    HDR,
};

struct RenderedImageF32 {
    Extent extent{};
    std::vector<float> rgb;

    [[nodiscard]] std::uint64_t pixelCount() const noexcept {
        return extent.pixelCount();
    }
};

struct RenderedFrame {
    std::uint64_t sourceRawId = 0;
    RenderedImageF32 image{};

    RenderIntent intent = RenderIntent::SDR;
    Primaries primaries = Primaries::SRGBRec709;
    WhitePoint whitePoint = WhitePoint::D65;
    TransferFunction transfer = TransferFunction::SRGB;
    ReferenceDomain reference = ReferenceDomain::Display;
    RangeSemantics range = RangeSemantics::EncodedDisplay;
    bool allowNegative = false;

    float renderExposureEV = 0.0F;
    float nominalWhiteNits = 100.0F;
    float peakTargetNits = 100.0F;
    float hdrHeadroom = 1.0F;
};

}  // namespace latent::imaging
