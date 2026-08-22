#include "latent/codec/UltraHdrStaging.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace latent::codec {
namespace {

void validateHostEndian() {
    if constexpr (std::endian::native != std::endian::little) {
        throw std::runtime_error(
            "libultrahdr packed RGBA staging currently requires a little-endian host");
    }
}

void validatePayload(const imaging::RenderedFrame& frame) {
    if (frame.reference != imaging::ReferenceDomain::Display ||
        frame.range != imaging::RangeSemantics::EncodedDisplay ||
        frame.allowNegative) {
        throw std::invalid_argument(
            "Ultra HDR staging requires encoded display-referred renditions");
    }
    if (frame.whitePoint != imaging::WhitePoint::D65) {
        throw std::invalid_argument("Ultra HDR staging currently requires D65 renditions");
    }
    if (frame.image.extent.width == 0U || frame.image.extent.height == 0U) {
        throw std::invalid_argument("Ultra HDR staging requires a non-empty rendition");
    }

    const std::uint64_t pixelCount = frame.image.extent.pixelCount();
    if (pixelCount >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / 3U)) {
        throw std::invalid_argument("rendition is too large for host indexing");
    }
    const std::size_t expected = static_cast<std::size_t>(pixelCount) * 3U;
    if (frame.image.rgb.size() != expected) {
        throw std::invalid_argument(
            "rendered RGB payload does not match its declared extent");
    }

    for (const float sample : frame.image.rgb) {
        if (!std::isfinite(sample) || sample < 0.0F || sample > 1.0F) {
            throw std::invalid_argument(
                "Ultra HDR staging requires finite normalized encoded samples in [0, 1]");
        }
    }
}

void validateSdr(const imaging::RenderedFrame& sdr) {
    validatePayload(sdr);
    if (sdr.intent != imaging::RenderIntent::SDR ||
        sdr.primaries != imaging::Primaries::SRGBRec709 ||
        sdr.transfer != imaging::TransferFunction::SRGB) {
        throw std::invalid_argument(
            "SDR Ultra HDR staging requires a Rec.709/sRGB SDR rendition");
    }
}

void validateHdr(const imaging::RenderedFrame& hdr) {
    validatePayload(hdr);
    if (hdr.intent != imaging::RenderIntent::HDR ||
        hdr.primaries != imaging::Primaries::BT2020 ||
        hdr.transfer != imaging::TransferFunction::PQ) {
        throw std::invalid_argument(
            "HDR Ultra HDR staging requires a BT.2020/PQ HDR rendition");
    }
    if (!std::isfinite(hdr.nominalWhiteNits) ||
        !std::isfinite(hdr.peakTargetNits) ||
        hdr.nominalWhiteNits <= 0.0F ||
        hdr.peakTargetNits < 203.0F ||
        hdr.peakTargetNits > 10000.0F ||
        hdr.nominalWhiteNits > hdr.peakTargetNits) {
        throw std::invalid_argument(
            "HDR Ultra HDR staging requires 0 < nominalWhite <= peak in [203, 10000] nits");
    }
}

std::uint32_t quantizeUnorm(float value, std::uint32_t maximum) {
    const double scaled =
        static_cast<double>(value) * static_cast<double>(maximum);
    const double rounded = std::floor(scaled + 0.5);
    return static_cast<std::uint32_t>(
        std::clamp(rounded, 0.0, static_cast<double>(maximum)));
}

PackedRendition packSdr(const imaging::RenderedFrame& sdr) {
    PackedRendition packed{};
    packed.extent = sdr.image.extent;
    packed.format = PackedPixelFormat::Rgba8888;
    packed.primaries = imaging::Primaries::SRGBRec709;
    packed.transfer = imaging::TransferFunction::SRGB;
    packed.rowStridePixels = sdr.image.extent.width;

    const std::uint64_t pixelCount64 = sdr.image.extent.pixelCount();
    if (pixelCount64 >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::invalid_argument("SDR rendition is too large to pack");
    }
    packed.pixels.reserve(static_cast<std::size_t>(pixelCount64));

    for (std::size_t base = 0U; base < sdr.image.rgb.size(); base += 3U) {
        const std::uint32_t r = quantizeUnorm(sdr.image.rgb[base], 255U);
        const std::uint32_t g = quantizeUnorm(sdr.image.rgb[base + 1U], 255U);
        const std::uint32_t b = quantizeUnorm(sdr.image.rgb[base + 2U], 255U);
        packed.pixels.push_back(
            r | (g << 8U) | (b << 16U) | (255U << 24U));
    }
    return packed;
}

PackedRendition packHdr(const imaging::RenderedFrame& hdr) {
    PackedRendition packed{};
    packed.extent = hdr.image.extent;
    packed.format = PackedPixelFormat::Rgba1010102;
    packed.primaries = imaging::Primaries::BT2020;
    packed.transfer = imaging::TransferFunction::PQ;
    packed.rowStridePixels = hdr.image.extent.width;

    const std::uint64_t pixelCount64 = hdr.image.extent.pixelCount();
    if (pixelCount64 >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::invalid_argument("HDR rendition is too large to pack");
    }
    packed.pixels.reserve(static_cast<std::size_t>(pixelCount64));

    for (std::size_t base = 0U; base < hdr.image.rgb.size(); base += 3U) {
        const std::uint32_t r = quantizeUnorm(hdr.image.rgb[base], 1023U);
        const std::uint32_t g = quantizeUnorm(hdr.image.rgb[base + 1U], 1023U);
        const std::uint32_t b = quantizeUnorm(hdr.image.rgb[base + 2U], 1023U);
        packed.pixels.push_back(
            r | (g << 10U) | (b << 20U) | (3U << 30U));
    }
    return packed;
}

}  // namespace

UltraHdrRenditionPair stageUltraHdrRenditions(
    const imaging::RenderedFrame& sdr,
    const imaging::RenderedFrame& hdr) {
    validateHostEndian();
    validateSdr(sdr);
    validateHdr(hdr);

    if (sdr.sourceRawId != hdr.sourceRawId) {
        throw std::invalid_argument(
            "Ultra HDR renditions must originate from the same scene source");
    }
    if (sdr.image.extent.width != hdr.image.extent.width ||
        sdr.image.extent.height != hdr.image.extent.height) {
        throw std::invalid_argument(
            "Ultra HDR SDR and HDR renditions must have identical extents");
    }

    UltraHdrRenditionPair pair{};
    pair.sourceRawId = sdr.sourceRawId;
    pair.sdr = packSdr(sdr);
    pair.hdr = packHdr(hdr);
    pair.hdrNominalWhiteNits = hdr.nominalWhiteNits;
    pair.hdrPeakTargetNits = hdr.peakTargetNits;
    return pair;
}

}  // namespace latent::codec
