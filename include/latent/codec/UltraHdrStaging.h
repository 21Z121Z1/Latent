#pragma once

#include "latent/imaging/RenderedFrame.h"

#include <cstdint>
#include <vector>

namespace latent::codec {

enum class PackedPixelFormat : std::uint8_t {
    Rgba8888,
    Rgba1010102,
};

struct PackedRendition {
    imaging::Extent extent{};
    PackedPixelFormat format = PackedPixelFormat::Rgba8888;
    imaging::Primaries primaries = imaging::Primaries::SRGBRec709;
    imaging::TransferFunction transfer = imaging::TransferFunction::SRGB;
    std::uint32_t rowStridePixels = 0;
    // libultrahdr documents both packed formats as 32-bit little-endian
    // words. uint32_t storage also guarantees the alignment its implementation
    // may require when reading the packed plane.
    std::vector<std::uint32_t> pixels;
};

struct UltraHdrRenditionPair {
    std::uint64_t sourceRawId = 0;
    PackedRendition sdr{};
    PackedRendition hdr{};
    float hdrNominalWhiteNits = 203.0F;
    float hdrPeakTargetNits = 1000.0F;
};

// Converts Latent's explicit display renditions into the packed raw formats
// accepted by libultrahdr without changing either rendition's tone intent:
//   SDR Rec.709/sRGB -> little-endian RGBA8888
//   HDR BT.2020/PQ   -> little-endian RGBA1010102
//
// Gain-map math is deliberately not implemented here. The returned pair is a
// codec boundary object for libultrahdr's UHDR_SDR_IMG + UHDR_HDR_IMG mode.
[[nodiscard]] UltraHdrRenditionPair stageUltraHdrRenditions(
    const imaging::RenderedFrame& sdr,
    const imaging::RenderedFrame& hdr);

}  // namespace latent::codec
