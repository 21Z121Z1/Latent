#pragma once

#include "latent/reference/DirectReconstruct.h"
#include "latent/reference/ReferenceReconstruct.h"

#include <span>

namespace latent::reference {

// Finish canonical green-balanced camera-linear RGB, WITHOUT another demosaic,
// RAW quantization, sensor correction, or display transform. The caller must
// establish that input G0/G1 balancing used the same ratio as the config gains.
// RGB white balance is [R, mean(G0,G1), B] and is applied exactly once here.
//
// Output arrays are interleaved RGB and must not alias inputs. Every input color
// must have positive finite coverage: SceneFrame cannot represent missing RGB.
// Non-finite input/output or arithmetic overflow fails closed; no clamp is used.
//
// If propagateNoise is true, varianceUpperBound has 3*N entries. Otherwise it
// must be empty. Cross-channel covariance is unavailable, so each scene-channel
// bound is (sum_c abs(M_sc * WB_c) * sqrt(cameraVariance_c) * sceneScale)^2.
// It is conditional on the reconstruction's frozen support/weights/geometry,
// NOT a calibrated posterior or an independent-channel marginal estimate.
void finishDirectSceneTile(std::span<const ReconstructedPixel> camera,
    const ReconstructionConfig& color, std::span<float> sceneRgb,
    std::span<float> varianceUpperBound);

}  // namespace latent::reference
