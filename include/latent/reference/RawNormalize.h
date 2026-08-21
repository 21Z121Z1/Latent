#pragma once

#include "latent/imaging/RawFrame.h"

#include <array>
#include <vector>

namespace latent::reference {

struct SelectedRawLevels {
    imaging::BlackLevel black{};
    float white = 0.0F;
    imaging::MetadataSource blackSource = imaging::MetadataSource::Unknown;
    imaging::MetadataSource whiteSource = imaging::MetadataSource::Unknown;
};

struct SensorLinearFrameF32 {
    imaging::Extent extent{};
    imaging::CfaPattern cfa = imaging::CfaPattern::RGGB;
    std::vector<float> samples;
    SelectedRawLevels levels{};
};

[[nodiscard]] SelectedRawLevels selectRawLevels(const imaging::RawFrame& frame);
[[nodiscard]] float normalizeSensorCode(float code, float black, float white);
[[nodiscard]] SensorLinearFrameF32 normalizeRaw(const imaging::RawFrame& frame);

}  // namespace latent::reference
