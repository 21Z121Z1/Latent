#pragma once

#include "latent/imaging/RawFrame.h"
#include "latent/reference/RawNormalize.h"

#include <vector>

namespace latent::reference {

struct DefectDetectionConfig {
    bool enabled = false;
    float sigmaMultiplier = 8.0F;
    float minAbsoluteDelta = 0.05F;
};

[[nodiscard]] SensorLinearFrameF32 correctDefects(
    const SensorLinearFrameF32& frame,
    const std::vector<imaging::DefectPixel>& defects);

// Detects defect candidates conservatively: a pixel is flagged only when its
// delta to the same-channel neighbor median exceeds BOTH a noise-aware
// threshold (sigmaMultiplier * sigma) and minAbsoluteDelta. When `noise` is
// provided it must be expressed in the same normalized sensor-linear domain
// as the frame samples; passing nullptr enables pure absolute-threshold mode.
[[nodiscard]] std::vector<imaging::DefectPixel> detectDefectCandidates(
    const SensorLinearFrameF32& frame,
    const DefectDetectionConfig& config,
    const imaging::NoiseModel* noise);

[[nodiscard]] SensorLinearFrameF32 applyLensShading(
    const SensorLinearFrameF32& frame,
    const imaging::LensShadingMap& map);

// Bilinearly interpolated per-channel gain at one pixel position, using the
// same grid mapping as applyLensShading.
[[nodiscard]] float lensShadingGainAt(
    const imaging::LensShadingMap& map,
    const imaging::Extent& extent,
    imaging::CfaPattern cfa,
    std::uint32_t x,
    std::uint32_t y);

}  // namespace latent::reference
