#pragma once

#include "latent/imaging/RawFrame.h"
#include "latent/imaging/SceneFrame.h"
#include "latent/imaging/Types.h"

#include <array>

namespace latent::reference {

struct ReconstructionConfig {
    std::array<float, 4> whiteBalanceGains{1.0F, 1.0F, 1.0F, 1.0F};
    imaging::Matrix3f cameraToAcescg{};
    float sceneScaleEV = 0.0F;
    float whiteBalanceConfidence = 0.0F;
};

[[nodiscard]] imaging::SceneFrame reconstructSingleRaw(
    const imaging::RawFrame& raw,
    const ReconstructionConfig& config);

}  // namespace latent::reference
