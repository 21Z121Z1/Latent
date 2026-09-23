#pragma once

#include "latent/imaging/SensorSampling.h"

#include "latent/imaging/Types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace latent::imaging {

enum class MetadataSource : std::uint8_t {
    Unknown,
    StaticCharacteristic,
    DynamicCaptureResult,
    OpticalBlackEstimate,
    DeviceProfile,
    MeasuredCalibration,
    EstimatedFromBurst,
};

enum class MetadataValidity : std::uint8_t {
    Missing,
    Valid,
    Suspect,
    Invalid,
};

template <typename T>
struct MetadataValue {
    std::optional<T> value;
    MetadataSource source = MetadataSource::Unknown;
    MetadataValidity validity = MetadataValidity::Missing;
    float confidence = 0.0F;

    [[nodiscard]] bool usable() const noexcept {
        return value.has_value() &&
               (validity == MetadataValidity::Valid || validity == MetadataValidity::Suspect);
    }
};

struct BlackLevel {
    std::array<float, 4> cfa{0.0F, 0.0F, 0.0F, 0.0F};
};

enum class NoiseCoordinate : std::uint8_t {
    RawCode,
    NormalizedBlackSubtracted,
};

// sigma(x) = sqrt(S*x + O). The coordinate is explicit: Android/DNG profiles
// use normalized, black-subtracted signal, not the legacy RawCode coordinate.
struct NoiseModel {
    std::array<float, 4> shot{0.0F, 0.0F, 0.0F, 0.0F};
    std::array<float, 4> read{0.0F, 0.0F, 0.0F, 0.0F};
    NoiseCoordinate coordinate = NoiseCoordinate::RawCode;
};

// Android-convention lens shading correction map: a rows x columns grid of
// per-channel gains [R, Geven, Godd, B], all >= 1.0, row-major, spanning the
// frame extent in normalized coordinates.
struct LensShadingMap {
    std::uint32_t gridColumns = 0;
    std::uint32_t gridRows = 0;
    std::vector<float> gains;

    [[nodiscard]] std::uint64_t gainCount() const noexcept {
        return static_cast<std::uint64_t>(gridColumns) *
               static_cast<std::uint64_t>(gridRows) * 4U;
    }
};

struct DefectPixel {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
};

struct ExposureCalibration {
    float nominalIso = 0.0F;
    // Relative gain in black-subtracted sensor CODE units. Unknown source
    // does not establish that this default is a measured physical gain.
    std::array<float, 4> effectiveGain{1.0F, 1.0F, 1.0F, 1.0F};
    float gainUncertainty = 1.0F;
    MetadataSource source = MetadataSource::Unknown;
};

struct RawStorage {
    Extent extent{};
    std::uint32_t rowStridePixels = 0;
    std::vector<std::uint16_t> pixels;
};

// Capture observations contain no pixel ownership or backend handle.
struct RawFrameMetadata {
    std::int64_t sensorTimestampNs = 0;
    std::string cameraId;
    std::string sensorMode;

    CfaPattern cfa = CfaPattern::RGGB;
    // Absent only for the pre-existing, explicitly regular-Bayer contract.
    std::optional<SensorSampling> sampling;

    std::int64_t exposureTimeNs = 0;
    float sensitivityIso = 0.0F;
    float postRawSensitivityBoost = 1.0F;

    MetadataValue<BlackLevel> staticBlack;
    MetadataValue<BlackLevel> dynamicBlack;
    MetadataValue<BlackLevel> opticalBlack;
    MetadataValue<float> staticWhite;
    MetadataValue<float> dynamicWhite;

    MetadataValue<std::array<float, 4>> neutralColorPoint;
    MetadataValue<std::array<float, 4>> colorCorrectionGains;

    MetadataValue<LensShadingMap> lensShading;
    MetadataValue<NoiseModel> noiseProfile;
    std::vector<DefectPixel> defects;

    ExposureCalibration exposureCalibration{};
};

// Owning reference/fixture convenience container. Production can bind the
// same metadata to borrowed host storage or other physical resources.
struct RawFrame : RawFrameMetadata {
    std::uint64_t id = 0;  // Legacy single-frame API; burst boundaries use FrameId.
    RawStorage storage{};
};

struct RawValidation {
    bool valid = true;
    std::string message;
};

[[nodiscard]] RawValidation validateRawMetadata(const RawFrameMetadata& metadata);
[[nodiscard]] RawValidation validateRawFrame(const RawFrame& frame);
[[nodiscard]] RawValidation validateLensShadingMap(const LensShadingMap& map);
[[nodiscard]] RawValidation validateNoiseModel(const NoiseModel& model);

}  // namespace latent::imaging
