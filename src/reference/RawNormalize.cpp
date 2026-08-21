#include "latent/reference/RawNormalize.h"

#include <algorithm>
#include <stdexcept>

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

RawValidation validateRawFrame(const RawFrame& frame) {
    if (frame.storage.extent.width == 0 || frame.storage.extent.height == 0) {
        return {false, "RAW extent must be non-zero"};
    }
    if (frame.storage.rowStridePixels < frame.storage.extent.width) {
        return {false, "RAW row stride cannot be smaller than width"};
    }
    const auto required = static_cast<std::uint64_t>(frame.storage.rowStridePixels) *
                          static_cast<std::uint64_t>(frame.storage.extent.height);
    if (frame.storage.pixels.size() < required) {
        return {false, "RAW storage is smaller than rowStride * height"};
    }
    if (frame.exposureTimeNs <= 0) {
        return {false, "exposure time must be positive"};
    }
    if (frame.sensitivityIso <= 0.0F) {
        return {false, "sensitivity ISO must be positive"};
    }
    return {};
}

}  // namespace latent::imaging

namespace latent::reference {

namespace {

const imaging::MetadataValue<imaging::BlackLevel>* selectBlackMetadata(const imaging::RawFrame& frame) {
    if (frame.opticalBlack.usable()) return &frame.opticalBlack;
    if (frame.dynamicBlack.usable()) return &frame.dynamicBlack;
    if (frame.staticBlack.usable()) return &frame.staticBlack;
    return nullptr;
}

const imaging::MetadataValue<float>* selectWhiteMetadata(const imaging::RawFrame& frame) {
    if (frame.dynamicWhite.usable()) return &frame.dynamicWhite;
    if (frame.staticWhite.usable()) return &frame.staticWhite;
    return nullptr;
}

std::size_t channelIndex(imaging::CfaChannel channel) {
    return static_cast<std::size_t>(channel);
}

}  // namespace

SelectedRawLevels selectRawLevels(const imaging::RawFrame& frame) {
    const auto* black = selectBlackMetadata(frame);
    const auto* white = selectWhiteMetadata(frame);
    if (black == nullptr || white == nullptr) {
        throw std::invalid_argument("RAW black/white metadata is incomplete");
    }

    SelectedRawLevels levels{};
    levels.black = *black->value;
    levels.white = *white->value;
    levels.blackSource = black->source;
    levels.whiteSource = white->source;

    const auto maxBlack = *std::max_element(levels.black.cfa.begin(), levels.black.cfa.end());
    if (!(levels.white > maxBlack)) {
        throw std::invalid_argument("RAW white level must be greater than every black level");
    }

    return levels;
}

float normalizeSensorCode(float code, float black, float white) {
    if (!(white > black)) {
        throw std::invalid_argument("white must be greater than black");
    }
    return (code - black) / (white - black);
}

SensorLinearFrameF32 normalizeRaw(const imaging::RawFrame& frame) {
    const auto validation = imaging::validateRawFrame(frame);
    if (!validation.valid) {
        throw std::invalid_argument(validation.message);
    }

    const auto levels = selectRawLevels(frame);
    SensorLinearFrameF32 result{};
    result.extent = frame.storage.extent;
    result.cfa = frame.cfa;
    result.levels = levels;
    result.samples.resize(static_cast<std::size_t>(result.extent.pixelCount()));

    for (std::uint32_t y = 0; y < result.extent.height; ++y) {
        for (std::uint32_t x = 0; x < result.extent.width; ++x) {
            const auto srcIndex = static_cast<std::size_t>(y) * frame.storage.rowStridePixels + x;
            const auto dstIndex = static_cast<std::size_t>(y) * result.extent.width + x;
            const auto channel = imaging::cfaChannelAt(frame.cfa, x, y);
            const auto black = levels.black.cfa[channelIndex(channel)];
            result.samples[dstIndex] = normalizeSensorCode(
                static_cast<float>(frame.storage.pixels[srcIndex]), black, levels.white);
        }
    }

    return result;
}

}  // namespace latent::reference
