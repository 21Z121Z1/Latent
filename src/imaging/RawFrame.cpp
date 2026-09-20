#include "latent/imaging/RawFrame.h"

#include <cmath>
#include <initializer_list>
#include <limits>

namespace latent::imaging {

CfaChannel cfaChannelAt(CfaPattern pattern, std::uint32_t x, std::uint32_t y) noexcept {
    const bool xOdd = (x & 1U) != 0U;
    const bool yOdd = (y & 1U) != 0U;

    switch (pattern) {
        case CfaPattern::RGGB:
            if (!yOdd) return xOdd ? CfaChannel::G0 : CfaChannel::R;
            return xOdd ? CfaChannel::B : CfaChannel::G1;
        case CfaPattern::GRBG:
            if (!yOdd) return xOdd ? CfaChannel::R : CfaChannel::G0;
            return xOdd ? CfaChannel::G1 : CfaChannel::B;
        case CfaPattern::GBRG:
            if (!yOdd) return xOdd ? CfaChannel::B : CfaChannel::G0;
            return xOdd ? CfaChannel::G1 : CfaChannel::R;
        case CfaPattern::BGGR:
            if (!yOdd) return xOdd ? CfaChannel::G0 : CfaChannel::B;
            return xOdd ? CfaChannel::R : CfaChannel::G1;
    }

    return CfaChannel::R;
}

RawValidation validateRawMetadata(const RawFrameMetadata& metadata) {
    if (static_cast<unsigned>(metadata.cfa) > static_cast<unsigned>(CfaPattern::BGGR)) {
        return {false, "RAW CFA pattern is unsupported"};
    }
    if (metadata.exposureTimeNs <= 0) return {false, "exposure time must be positive"};
    if (!std::isfinite(metadata.sensitivityIso) || metadata.sensitivityIso <= 0.0F) {
        return {false, "sensitivity ISO must be finite and positive"};
    }
    if (!std::isfinite(metadata.postRawSensitivityBoost) || metadata.postRawSensitivityBoost <= 0.0F) {
        return {false, "post-RAW sensitivity boost must be finite and positive"};
    }
    const auto confidenceValid = [](const auto& field) {
        return static_cast<unsigned>(field.source) <= static_cast<unsigned>(MetadataSource::EstimatedFromBurst) &&
               static_cast<unsigned>(field.validity) <= static_cast<unsigned>(MetadataValidity::Invalid) &&
               std::isfinite(field.confidence) && field.confidence >= 0.0F && field.confidence <= 1.0F;
    };
    if (!confidenceValid(metadata.staticBlack) || !confidenceValid(metadata.dynamicBlack) ||
        !confidenceValid(metadata.opticalBlack) || !confidenceValid(metadata.staticWhite) ||
        !confidenceValid(metadata.dynamicWhite) || !confidenceValid(metadata.noiseProfile) ||
        !confidenceValid(metadata.lensShading) || !confidenceValid(metadata.neutralColorPoint) ||
        !confidenceValid(metadata.colorCorrectionGains)) {
        return {false, "metadata source, validity, or confidence is invalid"};
    }
    for (const auto* black : {&metadata.staticBlack, &metadata.dynamicBlack, &metadata.opticalBlack}) {
        if (black->usable()) {
            for (float value : black->value->cfa) {
                if (!std::isfinite(value)) return {false, "black levels must be finite"};
            }
        }
    }
    for (const auto* white : {&metadata.staticWhite, &metadata.dynamicWhite}) {
        if (white->usable() && !std::isfinite(*white->value)) return {false, "white level must be finite"};
    }
    if (metadata.noiseProfile.usable()) {
        const auto check = validateNoiseModel(*metadata.noiseProfile.value);
        if (!check.valid) return check;
    }
    if (metadata.lensShading.usable()) {
        const auto check = validateLensShadingMap(*metadata.lensShading.value);
        if (!check.valid) return check;
    }
    if (!std::isfinite(metadata.exposureCalibration.gainUncertainty) ||
        metadata.exposureCalibration.gainUncertainty < 0.0F || metadata.exposureCalibration.gainUncertainty > 1.0F) {
        return {false, "gain uncertainty must be finite and within [0, 1]"};
    }
    for (float gain : metadata.exposureCalibration.effectiveGain) {
        if (!std::isfinite(gain) || gain <= 0.0F) return {false, "effective gain must be finite and positive"};
    }
    return {};
}

RawValidation validateRawFrame(const RawFrame& frame) {
    const auto metadata = validateRawMetadata(frame);
    if (!metadata.valid) return metadata;
    if (frame.storage.extent.width == 0U || frame.storage.extent.height == 0U) {
        return {false, "RAW extent must be non-zero"};
    }
    if (frame.storage.rowStridePixels < frame.storage.extent.width) {
        return {false, "RAW row stride cannot be smaller than width"};
    }
    const auto required = static_cast<std::uint64_t>(frame.storage.rowStridePixels) *
                          (frame.storage.extent.height - 1U) + frame.storage.extent.width;
    if (frame.storage.pixels.size() < required) {
        return {false, "RAW storage does not contain every addressed sample"};
    }
    return {};
}

RawValidation validateLensShadingMap(const LensShadingMap& map) {
    if (map.gridColumns == 0 || map.gridRows == 0) {
        return {false, "lens shading map grid must be non-empty"};
    }
    const auto cells = static_cast<std::uint64_t>(map.gridColumns) * map.gridRows;
    if (cells > std::numeric_limits<std::size_t>::max() / 4U) {
        return {false, "lens shading map extent exceeds addressable storage"};
    }
    const auto expected = static_cast<std::size_t>(cells) * 4U;
    if (map.gains.size() != expected) {
        return {false, "lens shading map gain count must equal gridColumns * gridRows * 4"};
    }
    for (const auto gain : map.gains) {
        if (!std::isfinite(gain)) {
            return {false, "lens shading map gains must be finite"};
        }
        if (gain < 1.0F) {
            return {false, "lens shading map gains must be >= 1.0"};
        }
    }
    return {};
}

RawValidation validateNoiseModel(const NoiseModel& model) {
    if (model.coordinate != NoiseCoordinate::RawCode &&
        model.coordinate != NoiseCoordinate::NormalizedBlackSubtracted) {
        return {false, "noise coordinate is unsupported"};
    }
    for (std::size_t c = 0; c < 4; ++c) {
        if (!std::isfinite(model.shot[c]) || !std::isfinite(model.read[c])) {
            return {false, "noise model coefficients must be finite"};
        }
        if (model.shot[c] < 0.0F || model.read[c] < 0.0F) {
            return {false, "noise model coefficients must be non-negative"};
        }
    }
    return {};
}

}  // namespace latent::imaging

