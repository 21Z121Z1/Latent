#pragma once

#include "latent/imaging/RawFrame.h"
#include "latent/vulkan/ComputeRunner.h"

#include <cstdint>
#include <vector>

namespace latent::vulkan {

struct PreprocessParams {
    imaging::Extent extent{};
    imaging::CfaPattern cfa = imaging::CfaPattern::RGGB;
    float whiteLevel = 1023.0F;
    std::array<float, 4> black{64.0F, 64.0F, 64.0F, 64.0F};
    std::array<float, 4> wbGains{1.0F, 1.0F, 1.0F, 1.0F};

    // Empty gains disable lens shading inside the kernel.
    imaging::LensShadingMap lensShading{};
};

// Fused pointwise kernel: canonical uint16 RAW -> normalize by per-channel
// black and white level -> bilinear lens-shading gain -> white-balance gain,
// computed in FP32 and stored as IEEE binary16.
//
// Input: `extent.pixelCount()` canonical uint16 samples.
// Output: same count of half-precision BIT PATTERNS (decode with
// imaging::halfBitsToFloat).
class SensorPreprocessKernel {
public:
    explicit SensorPreprocessKernel(ComputeRunner& runner);

    [[nodiscard]] std::vector<std::uint16_t> run(
        const PreprocessParams& params,
        const std::vector<std::uint16_t>& canonicalRaw);

private:
    ComputeRunner& runner_;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace latent::vulkan