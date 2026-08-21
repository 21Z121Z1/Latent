#pragma once

#include "latent/imaging/RawFrame.h"
#include "latent/imaging/Types.h"

#include <cstdint>
#include <vector>

namespace latent::imaging {

enum class RawPacking : std::uint8_t {
    Raw16,
    Raw10,
    Raw12,
};

[[nodiscard]] constexpr std::uint32_t bitsPerSample(RawPacking packing) noexcept {
    switch (packing) {
        case RawPacking::Raw10:
            return 10U;
        case RawPacking::Raw12:
            return 12U;
        case RawPacking::Raw16:
            return 16U;
    }
    return 16U;
}

[[nodiscard]] constexpr std::uint32_t minRowStrideBytes(Extent extent, RawPacking packing) noexcept {
    const std::uint64_t bits = static_cast<std::uint64_t>(extent.width) *
                               bitsPerSample(packing);
    return static_cast<std::uint32_t>((bits + 7U) / 8U);
}

// Packed RAW frame geometry following the Android/MIPI conventions: each row
// begins on its own rowStride boundary; trailing rows may be short. Row
// padding bytes beyond ceil(width * bpp / 8) are skipped.
struct PackedRawLayout {
    Extent extent{};
    RawPacking packing = RawPacking::Raw16;
    std::uint32_t rowStrideBytes = 0;
};

[[nodiscard]] RawValidation validatePackedRawLayout(
    const PackedRawLayout& layout,
    const std::uint8_t* data,
    std::uint64_t dataSize);

// Unpacks a packed RAW frame into canonical little-endian uint16 samples that
// hold native-depth values (RAW10 samples span 0..1023, RAW12 0..4095); no
// left-justification is applied, scaling is normalization's job.
[[nodiscard]] std::vector<std::uint16_t> unpackPackedRaw(
    const PackedRawLayout& layout,
    const std::uint8_t* data,
    std::uint64_t dataSize);

}  // namespace latent::imaging
