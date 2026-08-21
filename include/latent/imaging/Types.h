#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace latent::imaging {

enum class ReferenceDomain : std::uint8_t {
    Sensor,
    Scene,
    Display,
};

enum class PixelLayout : std::uint8_t {
    Bayer,
    RGB,
    RGBA,
    Planar,
};

enum class NumericType : std::uint8_t {
    UInt16,
    Float16,
    Float32,
};

enum class ColorModel : std::uint8_t {
    SensorCfa,
    RGB,
    XYZ,
};

enum class Primaries : std::uint8_t {
    SensorNative,
    ACEScgAP1,
    SRGBRec709,
    DisplayP3,
    BT2020,
    XYZ1931,
};

enum class WhitePoint : std::uint8_t {
    SensorNative,
    D50,
    D60,
    D65,
};

enum class TransferFunction : std::uint8_t {
    Linear,
    SRGB,
    PQ,
    HLG,
};

enum class RangeSemantics : std::uint8_t {
    SensorReferenceNormalized,
    Unbounded,
    EncodedDisplay,
};

enum class PrecisionClass : std::uint8_t {
    ReferenceFP32,
    ProductionMixed,
};

struct Extent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] constexpr std::uint64_t pixelCount() const noexcept {
        return static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    }
};

struct ImageType {
    Extent extent{};
    PixelLayout layout = PixelLayout::RGB;
    NumericType storage = NumericType::Float32;
    PrecisionClass precision = PrecisionClass::ReferenceFP32;

    ColorModel colorModel = ColorModel::RGB;
    Primaries primaries = Primaries::ACEScgAP1;
    WhitePoint whitePoint = WhitePoint::D60;
    TransferFunction transfer = TransferFunction::Linear;

    ReferenceDomain reference = ReferenceDomain::Scene;
    float sceneScaleEV = 0.0F;
    RangeSemantics range = RangeSemantics::Unbounded;
    bool allowNegative = true;
};

struct TypeValidation {
    bool valid = true;
    std::string message;
};

[[nodiscard]] inline TypeValidation validateImageType(const ImageType& type) {
    if (type.extent.width == 0 || type.extent.height == 0) {
        return {false, "image extent must be non-zero"};
    }

    if (type.reference == ReferenceDomain::Scene) {
        if (type.transfer != TransferFunction::Linear) {
            return {false, "scene-referred images must be linear"};
        }
        if (type.range != RangeSemantics::Unbounded) {
            return {false, "scene-referred images must use unbounded range semantics"};
        }
        if (!type.allowNegative) {
            return {false, "scene-referred images must permit negative coordinates"};
        }
    }

    if (type.reference == ReferenceDomain::Sensor) {
        if (type.transfer != TransferFunction::Linear) {
            return {false, "sensor-referred images must be linear"};
        }
        if (type.range == RangeSemantics::EncodedDisplay) {
            return {false, "sensor-referred images cannot use display-encoded range semantics"};
        }
    }

    if (type.reference == ReferenceDomain::Display &&
        type.range != RangeSemantics::EncodedDisplay) {
        return {false, "display-referred images must explicitly use encoded-display range semantics"};
    }

    return {};
}

struct Matrix3f {
    std::array<float, 9> values{
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F,
    };

    [[nodiscard]] constexpr std::array<float, 3> apply(const std::array<float, 3>& v) const noexcept {
        return {
            values[0] * v[0] + values[1] * v[1] + values[2] * v[2],
            values[3] * v[0] + values[4] * v[1] + values[5] * v[2],
            values[6] * v[0] + values[7] * v[1] + values[8] * v[2],
        };
    }
};

}  // namespace latent::imaging
