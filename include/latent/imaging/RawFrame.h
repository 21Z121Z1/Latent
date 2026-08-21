#pragma once

#include "latent/imaging/Types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace latent::imaging {

enum class CfaPattern : std::uint8_t {
    RGGB,
    GRBG,
    GBRG,
    BGGR,
};

enum class CfaChannel : std::uint8_t {
    R = 0,
    G0 = 1,
    G1 = 2,
    B = 3,
};

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
        return value.has_value() && validity != MetadataValidity::Invalid;
    }
};

struct BlackLevel {
    std::array<float, 4> cfa{0.0F, 0.0F, 0.0F, 0.0F};
};

// Per-CFA-channel noise model in raw code units: sigma(x) = sqrt(S*x + O),
// matching the Android NOISE_PROFILE / DNG NoiseProfile convention.
struct NoiseModel {
    std::array<float, 4> shot{0.0F, 0.0F, 0.0F, 0.0F};
    std::array<float, 4> read{0.0F, 0.0F, 0.0F, 0.0F};
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
    std::array<float, 4> effectiveGain{1.0F, 1.0F, 1.0F, 1.0F};
    float gainUncertainty = 1.0F;
    MetadataSource source = MetadataSource::Unknown;
};

struct RawStorage {
    Extent extent{};
    std::uint32_t rowStridePixels = 0;
    std::vector<std::uint16_t> pixels;
};

struct RawFrame {
    std::uint64_t id = 0;
    std::int64_t sensorTimestampNs = 0;
    std::string cameraId;
    std::string sensorMode;

    RawStorage storage{};
    CfaPattern cfa = CfaPattern::RGGB;

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

struct RawValidation {
    bool valid = true;
    std::string message;
};

[[nodiscard]] CfaChannel cfaChannelAt(CfaPattern pattern, std::uint32_t x, std::uint32_t y) noexcept;
[[nodiscard]] RawValidation validateRawFrame(const RawFrame& frame);
[[nodiscard]] RawValidation validateLensShadingMap(const LensShadingMap& map);
[[nodiscard]] RawValidation validateNoiseModel(const NoiseModel& model);

}  // namespace latent::imaging
