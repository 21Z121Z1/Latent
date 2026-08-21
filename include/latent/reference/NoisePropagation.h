#pragma once

#include "latent/imaging/Noise.h"
#include "latent/reference/Demosaic.h"
#include "latent/reference/RawNormalize.h"

namespace latent::reference {

struct SigmaQuery {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::size_t rgbChannel = 0;

    // Full scene-linear RGB triple at the queried pixel, taken from the
    // SceneFrame being characterized. All three components are required to
    // invert the camera-to-scene matrix for the signal estimate.
    std::array<float, 3> sceneValueRgb{0.0F, 0.0F, 0.0F};
};

// Standard deviation of the scene-linear value at one pixel for one output
// channel, propagated through normalize -> defect -> LSC -> WB -> demosaic ->
// camera-to-scene matrix -> scene scale. Deterministic; O(taps) per query.
[[nodiscard]] float propagatedSigma(
    const imaging::PropagatedNoise& noise,
    const SigmaQuery& query);

// Transforms raw-code-domain noise coefficients into the normalized
// sensor-linear domain selected by `levels`:
//   S' = S / (W - B_c),  O' = (S * B_c + O) / (W - B_c)^2
[[nodiscard]] imaging::NoiseModel normalizeNoiseModel(
    const imaging::NoiseModel& rawCodeModel,
    const SelectedRawLevels& levels);

// Assembles the lazy PropagatedNoise record for a reconstruction run.
[[nodiscard]] imaging::PropagatedNoise buildPropagatedNoise(
    const SensorLinearFrameF32& sensorFrame,
    const imaging::NoiseModel& normalizedModel,
    const std::array<float, 4>& whiteBalanceGains,
    bool lensShadingApplied,
    const imaging::LensShadingMap* lensShading,
    const imaging::Matrix3f& cameraToScene,
    float sceneScale,
    DemosaicMethod method);

}  // namespace latent::reference
