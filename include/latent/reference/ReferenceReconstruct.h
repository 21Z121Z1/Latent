#pragma once

#include "latent/imaging/RawFrame.h"
#include "latent/imaging/SceneFrame.h"
#include "latent/imaging/Types.h"
#include "latent/reference/Demosaic.h"
#include "latent/reference/DngColor.h"

#include <array>

namespace latent::reference {

enum class ColorPath : std::uint8_t {
    ExplicitMatrix,
    DngProfile,
};

struct ReconstructionConfig {
    DemosaicMethod demosaicMethod = DemosaicMethod::MalvarHeCutler2004;
    ColorPath colorPath = ColorPath::ExplicitMatrix;

    std::array<float, 4> whiteBalanceGains{1.0F, 1.0F, 1.0F, 1.0F};
    imaging::Matrix3f cameraToAcescg{};
    DngCameraProfile dngProfile{};
    imaging::ChromaticityXY whiteBalanceXy{imaging::kIlluminantD65};

    float sceneScaleEV = 0.0F;
    float whiteBalanceConfidence = 0.0F;
};

[[nodiscard]] imaging::SceneFrame reconstructSingleRaw(
    const imaging::RawFrame& raw,
    const ReconstructionConfig& config);

}  // namespace latent::reference
