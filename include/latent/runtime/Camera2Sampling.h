#pragma once
#include "latent/imaging/SensorSampling.h"

namespace latent::runtime {
// Projection of public Camera2 observations. No device/vendor name heuristics.
// Values belong to the ACTUAL pixel mode; absence is not a zero/false value.
struct Camera2SamplingEvidence {
    imaging::CfaPattern base = imaging::CfaPattern::RGGB;
    std::optional<imaging::Extent> physicalGroup;
    std::optional<bool> rawGroupingUsed;
    bool ultraHighResolution = false, remosaicReprocessing = false;
    bool pixelModeRequestAvailable = false;
    imaging::SensorPixelMode requestedMode = imaging::SensorPixelMode::Default;
    std::optional<imaging::SensorPixelMode> actualMode;
    imaging::Extent pixelArray{}, rawExtent{};
    imaging::SensorRect active{}, preCorrection{}, deliveredCrop{};
    bool croppedRawStream = false;
    std::optional<imaging::SensorRect> rawCropRegion;
    float zoomRatio = 1;
    std::string coordinateSpace;
};
// Fails closed on inconsistent/underspecified topology and mode transitions.
[[nodiscard]] imaging::SensorSampling interpretCamera2Sampling(const Camera2SamplingEvidence&);
}  // namespace latent::runtime
