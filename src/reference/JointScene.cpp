#include "latent/reference/JointScene.h"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace latent::reference {

void finishDirectSceneTile(std::span<const ReconstructedPixel> camera,
    const ReconstructionConfig& color, std::span<float> sceneRgb,
    std::span<float> varianceUpperBound) {
    if (color.applyLensShading || color.defectCorrection != DefectCorrectionMode::Disabled) {
        throw std::invalid_argument("direct scene finishing must not repeat sensor corrections");
    }
    if (camera.empty() || camera.size() > std::numeric_limits<std::size_t>::max() / 3U ||
        sceneRgb.size() != camera.size() * 3U ||
        varianceUpperBound.size() != (color.propagateNoise ? sceneRgb.size() : 0U)) {
        throw std::invalid_argument("invalid direct scene tile payload");
    }
    const auto matrix = cameraToSceneMatrix(color);
    const float scale = sceneCoordinateScale(color.sceneScaleEV);
    const std::array<float, 3> wb{color.whiteBalanceGains[0],
        0.5F * (color.whiteBalanceGains[1] + color.whiteBalanceGains[2]),
        color.whiteBalanceGains[3]};
    if (!std::isfinite(wb[1]) || wb[1] <= 0.0F) {
        throw std::invalid_argument("invalid common green white balance");
    }
    for (std::size_t i = 0; i < camera.size(); ++i) {
        std::array<float, 3> rgb{}, sigma{};
        for (std::size_t c = 0; c < 3U; ++c) {
            const auto& p = camera[i];
            if (!std::isfinite(p.rgb[c]) || !std::isfinite(p.variance[c]) || p.variance[c] < 0.0F ||
                !std::isfinite(p.confidence[c]) || p.confidence[c] <= 0.0F || p.confidence[c] > 1.0F) {
                throw std::invalid_argument("direct scene requires finite, covered camera RGB and variance");
            }
            rgb[c] = p.rgb[c] * wb[c];
            if (color.propagateNoise) sigma[c] = std::sqrt(p.variance[c]) * wb[c];
        }
        const auto scene = matrix.apply(rgb);
        for (std::size_t c = 0; c < 3U; ++c) {
            const float value = scene[c] * scale;
            if (!std::isfinite(value)) throw std::overflow_error("non-finite direct scene color result");
            sceneRgb[i * 3U + c] = value;
            if (color.propagateNoise) {
                float boundSigma = 0.0F;
                for (std::size_t k = 0; k < 3U; ++k) {
                    boundSigma += std::abs(matrix.values[c * 3U + k]) * sigma[k];
                }
                boundSigma *= scale;
                const float bound = boundSigma * boundSigma;
                if (!std::isfinite(bound)) throw std::overflow_error("non-finite direct scene variance bound");
                varianceUpperBound[i * 3U + c] = bound;
            }
        }
    }
}

}  // namespace latent::reference
