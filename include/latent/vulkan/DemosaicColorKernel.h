#pragma once

#include "latent/imaging/RawFrame.h"
#include "latent/imaging/Types.h"
#include "latent/vulkan/ComputeRunner.h"

#include <array>
#include <cstdint>
#include <vector>

namespace latent::vulkan {

struct DemosaicColorParams {
    imaging::Extent extent{};
    imaging::CfaPattern cfa = imaging::CfaPattern::RGGB;
    imaging::DemosaicMethod demosaicMethod =
        imaging::DemosaicMethod::MalvarHeCutler2004;

    // Camera-RGB -> scene RGB (e.g. the composed DNG profile matrix into
    // linear AP1). Row-major, matching imaging::Matrix3f.
    std::array<float, 9> cameraToScene{1.0F, 0.0F, 0.0F,
                                       0.0F, 1.0F, 0.0F,
                                       0.0F, 0.0F, 1.0F};
    float sceneScale = 1.0F;
};

// Fused kernel: gained FP16 Bayer (the output of SensorPreprocessKernel) ->
// demosaic -> color matrix -> scene scale -> packed FP16 RGBA16F.
//
// Input: `extent.pixelCount()` half-precision BIT PATTERNS in canonical
// sample order. Output: `extent.pixelCount() * 4` half-precision BIT
// PATTERNS (R, G, B, 0 per pixel); decode with imaging::halfBitsToFloat.
class DemosaicColorKernel {
public:
    explicit DemosaicColorKernel(ComputeRunner& runner);

    [[nodiscard]] std::vector<std::uint16_t> run(
        const DemosaicColorParams& params,
        const std::vector<std::uint16_t>& bayerHalfBits);

private:
    ComputeRunner& runner_;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace latent::vulkan