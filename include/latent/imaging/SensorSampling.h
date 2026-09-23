#pragma once

#include "latent/imaging/Types.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace latent::imaging {
enum class CfaPattern : std::uint8_t { RGGB, GRBG, GBRG, BGGR };
enum class CfaChannel : std::uint8_t { R = 0, G0 = 1, G1 = 2, B = 3 };
[[nodiscard]] CfaChannel cfaChannelAt(CfaPattern pattern, std::uint32_t x, std::uint32_t y) noexcept;

// No vendor or megapixel enum: the topology describes a periodic sampling lattice.
struct CfaTopology {
    CfaPattern base = CfaPattern::RGGB;
    std::uint32_t groupX = 1, groupY = 1;
    bool operator==(const CfaTopology&) const = default;
};
enum class RawRepresentation : std::uint8_t { Unknown, Bayer, GroupedBayer, RemosaicedBayer };
enum class SensorPixelMode : std::uint8_t { Default, MaximumResolution };
enum class ProcessingState : std::uint8_t { Unknown, NotApplied, Applied };
struct SensorRect {
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    bool operator==(const SensorRect&) const = default;
};
// Maps pixel CENTERS, not edges. A unit crop maps (0,0) to (crop.x,crop.y).
// It is deliberately independent from CFA phase: binned sensor coordinates are
// not the integer coordinates of the RAW lattice.
struct SampleTransform {
    double scaleX = 1, scaleY = 1, translateX = 0, translateY = 0;
    bool operator==(const SampleTransform&) const = default;
};
struct SensorSampling {
    std::optional<CfaTopology> physical;
    CfaTopology buffer;
    RawRepresentation representation = RawRepresentation::Unknown;
    SensorPixelMode pixelMode = SensorPixelMode::Default;
    ProcessingState binning = ProcessingState::Unknown, remosaic = ProcessingState::Unknown;
    // Coordinates of buffer (0,0) in the RAW lattice, BEFORE this crop. Do not
    // reduce modulo two: a grouped lattice has period (2*groupX,2*groupY).
    std::uint32_t originX = 0, originY = 0;
    Extent pixelArray{};
    SensorRect active{}, preCorrectionActive{}, calibration{};
    // Field-of-view mapping and calibration mapping differ for Camera2 CROPPED_RAW.
    SampleTransform bufferToSensor{}, bufferToCalibration{};
    std::string coordinateSpace;  // mode/physical-camera calibration identity
    float zoomRatio = 1;
    bool operator==(const SensorSampling&) const = default;
};
struct SamplingValidation { bool valid = true; std::string message; };
[[nodiscard]] SamplingValidation validateSampling(const SensorSampling&, Extent bufferExtent);
[[nodiscard]] CfaChannel samplingChannelAt(const SensorSampling&, std::uint32_t x, std::uint32_t y);
[[nodiscard]] SensorSampling cropSampling(const SensorSampling&, Extent oldExtent, SensorRect crop);
[[nodiscard]] bool sameSamplingMode(const SensorSampling&, const SensorSampling&);
// Explicit fixture/legacy bridge. Unknown external metadata must NOT call this
// constructor merely because the dimensions resemble a known sensor.
[[nodiscard]] SensorSampling regularSampling(Extent, CfaPattern);
[[nodiscard]] bool legacyBayerCompatible(const SensorSampling&, CfaPattern);
[[nodiscard]] std::string samplingJson(const SensorSampling&);
}  // namespace latent::imaging
