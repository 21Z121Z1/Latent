#pragma once

#include "latent/imaging/SceneFrame.h"

#include <cstdint>

namespace latent::imaging {

enum class RenderIntent : std::uint8_t {
    SDR,
    HDR,
};

struct RenderedFrame {
    std::uint64_t sourceRawId = 0;
    RenderIntent intent = RenderIntent::SDR;
    Primaries primaries = Primaries::SRGBRec709;
    WhitePoint whitePoint = WhitePoint::D65;
    TransferFunction transfer = TransferFunction::SRGB;

    float renderExposureEV = 0.0F;
    float nominalWhiteNits = 100.0F;
    float peakTargetNits = 100.0F;
    float hdrHeadroom = 1.0F;
};

}  // namespace latent::imaging
