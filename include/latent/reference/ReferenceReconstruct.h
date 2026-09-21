#pragma once

#include "latent/imaging/RawFrame.h"
#include "latent/imaging/SceneFrame.h"
#include "latent/imaging/Types.h"
#include "latent/reference/Demosaic.h"
#include "latent/reference/DngColor.h"
#include "latent/reference/SensorLinearOps.h"

#include <array>

namespace latent::reference {

enum class ColorPath : std::uint8_t {
    ExplicitMatrix,
    DngProfile,
};

enum class DefectCorrectionMode : std::uint8_t {
    Disabled,
    FromMetadataMap,
    DetectAndCorrect,
};

struct ReconstructionConfig {
    DemosaicMethod demosaicMethod = DemosaicMethod::MalvarHeCutler2004;
    ColorPath colorPath = ColorPath::ExplicitMatrix;

    DefectCorrectionMode defectCorrection = DefectCorrectionMode::Disabled;
    DefectDetectionConfig defectDetection{};
    bool applyLensShading = false;
    bool propagateNoise = true;

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

// Finish an already normalized/corrected FP32 CFA image without quantization.
// No affine noise model is inferred for fused data. The caller owns its lineage
// and conditional variance. Sensor correction flags must be disabled here.
[[nodiscard]] imaging::SceneFrame reconstructSensorLinear(
    const SensorLinearFrameF32& sensor, const ReconstructionConfig& config);

}  // namespace latent::reference
