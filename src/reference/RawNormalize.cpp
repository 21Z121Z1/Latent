#include "latent/reference/RawNormalize.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <stdexcept>

namespace latent::reference {

namespace {

template <typename T>
const imaging::MetadataValue<T>* selectByTrust(
    std::initializer_list<const imaging::MetadataValue<T>*> candidates) {
    for (const auto* candidate : candidates) {
        if (candidate->value.has_value() && candidate->validity == imaging::MetadataValidity::Valid) {
            return candidate;
        }
    }
    for (const auto* candidate : candidates) {
        if (candidate->value.has_value() && candidate->validity == imaging::MetadataValidity::Suspect) {
            return candidate;
        }
    }
    return nullptr;
}

const imaging::MetadataValue<imaging::BlackLevel>* selectBlackMetadata(const imaging::RawFrameMetadata& frame) {
    return selectByTrust<imaging::BlackLevel>({&frame.opticalBlack, &frame.dynamicBlack, &frame.staticBlack});
}

const imaging::MetadataValue<float>* selectWhiteMetadata(const imaging::RawFrameMetadata& frame) {
    return selectByTrust<float>({&frame.dynamicWhite, &frame.staticWhite});
}

std::size_t channelIndex(imaging::CfaChannel channel) {
    return static_cast<std::size_t>(channel);
}

}  // namespace

SelectedRawLevels selectRawLevels(const imaging::RawFrameMetadata& frame) {
    const auto check = imaging::validateRawMetadata(frame);
    if (!check.valid) throw std::invalid_argument(check.message);
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
    if (!std::isfinite(code) || !std::isfinite(black) || !std::isfinite(white) || !(white > black)) {
        throw std::invalid_argument("white must be greater than black");
    }
    return (code - black) / (white - black);
}

SensorLinearFrameF32 normalizeRaw(const imaging::RawFrame& frame) {
    return normalizeRaw(runtime::viewRawFrame(frame));
}

SensorLinearFrameF32 normalizeRaw(const runtime::RawFrameView& frame) {
    const auto validation = runtime::validateRawView(frame);
    if (!validation.valid) {
        throw std::invalid_argument(validation.message);
    }

    const auto levels = selectRawLevels(*frame.metadata);
    SensorLinearFrameF32 result{};
    result.extent = frame.storage.extent;
    result.cfa = frame.metadata->cfa;
    result.levels = levels;
    result.samples.resize(static_cast<std::size_t>(result.extent.pixelCount()));

    for (std::uint32_t y = 0; y < result.extent.height; ++y) {
        for (std::uint32_t x = 0; x < result.extent.width; ++x) {
            const auto srcIndex = static_cast<std::size_t>(y) * frame.storage.rowStridePixels + x;
            const auto dstIndex = static_cast<std::size_t>(y) * result.extent.width + x;
            const auto channel = imaging::cfaChannelAt(frame.metadata->cfa, x, y);
            const auto black = levels.black.cfa[channelIndex(channel)];
            result.samples[dstIndex] = normalizeSensorCode(
                static_cast<float>(frame.storage.pixels[srcIndex]), black, levels.white);
        }
    }

    return result;
}

}  // namespace latent::reference
