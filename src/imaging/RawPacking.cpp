#include "latent/imaging/RawPacking.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace latent::imaging {
namespace {

constexpr std::uint32_t kRaw10GroupSamples = 4U;
constexpr std::uint32_t kRaw10GroupBytes = 5U;
constexpr std::uint32_t kRaw12GroupSamples = 2U;
constexpr std::uint32_t kRaw12GroupBytes = 3U;

void unpackRaw10Row(const std::uint8_t* row, std::uint32_t width, std::uint16_t* out) {
    std::uint32_t x = 0U;
    while (x + kRaw10GroupSamples <= width) {
        const auto b0 = static_cast<std::uint32_t>(row[0]);
        const auto b1 = static_cast<std::uint32_t>(row[1]);
        const auto b2 = static_cast<std::uint32_t>(row[2]);
        const auto b3 = static_cast<std::uint32_t>(row[3]);
        const auto b4 = static_cast<std::uint32_t>(row[4]);

        out[x + 0U] = static_cast<std::uint16_t>((b0 << 2U) | (b4 & 0x3U));
        out[x + 1U] = static_cast<std::uint16_t>((b1 << 2U) | ((b4 >> 2U) & 0x3U));
        out[x + 2U] = static_cast<std::uint16_t>((b2 << 2U) | ((b4 >> 4U) & 0x3U));
        out[x + 3U] = static_cast<std::uint16_t>((b3 << 2U) | ((b4 >> 6U) & 0x3U));

        row += kRaw10GroupBytes;
        x += kRaw10GroupSamples;
    }

    const std::uint32_t remaining = width - x;
    if (remaining == 0U) {
        return;
    }
    // Trailing partial group: `remaining` high bytes plus one shared LSB byte.
    for (std::uint32_t k = 0; k < remaining; ++k) {
        out[x + k] = static_cast<std::uint16_t>(
            (static_cast<std::uint32_t>(row[k]) << 2U) |
            ((static_cast<std::uint32_t>(row[remaining]) >> (2U * k)) & 0x3U));
    }
}

void unpackRaw12Row(const std::uint8_t* row, std::uint32_t width, std::uint16_t* out) {
    std::uint32_t x = 0U;
    while (x + kRaw12GroupSamples <= width) {
        const auto b0 = static_cast<std::uint32_t>(row[0]);
        const auto b1 = static_cast<std::uint32_t>(row[1]);
        const auto b2 = static_cast<std::uint32_t>(row[2]);

        out[x + 0U] = static_cast<std::uint16_t>((b0 << 4U) | (b2 & 0xFU));
        out[x + 1U] = static_cast<std::uint16_t>((b1 << 4U) | ((b2 >> 4U) & 0xFU));

        row += kRaw12GroupBytes;
        x += kRaw12GroupSamples;
    }

    if (width - x == 1U) {
        // Trailing single sample: one high byte plus one shared nibble byte.
        out[x] = static_cast<std::uint16_t>(
            (static_cast<std::uint32_t>(row[0]) << 4U) |
            (static_cast<std::uint32_t>(row[1]) & 0xFU));
    }
}

}  // namespace

RawValidation validatePackedRawLayout(
    const PackedRawLayout& layout,
    const std::uint8_t* data,
    std::uint64_t dataSize) {
    if (layout.extent.width == 0 || layout.extent.height == 0) {
        return {false, "packed RAW extent must be non-zero"};
    }
    if (layout.rowStrideBytes < minRowStrideBytes(layout.extent, layout.packing)) {
        return {false, "packed RAW row stride is smaller than the packed row width"};
    }
    if (data == nullptr && dataSize != 0U) {
        return {false, "packed RAW data pointer must not be null"};
    }
    const std::uint64_t required =
        static_cast<std::uint64_t>(layout.rowStrideBytes) *
            (layout.extent.height - 1U) +
        minRowStrideBytes(layout.extent, layout.packing);
    if (dataSize < required) {
        return {false, "packed RAW buffer is smaller than the layout requires"};
    }
    return {};
}

std::vector<std::uint16_t> unpackPackedRaw(
    const PackedRawLayout& layout,
    const std::uint8_t* data,
    std::uint64_t dataSize) {
    const auto validation = validatePackedRawLayout(layout, data, dataSize);
    if (!validation.valid) {
        throw std::invalid_argument(validation.message);
    }

    std::vector<std::uint16_t> result(
        static_cast<std::size_t>(layout.extent.pixelCount()));

    for (std::uint32_t y = 0; y < layout.extent.height; ++y) {
        const auto* row = data + static_cast<std::uint64_t>(y) * layout.rowStrideBytes;
        auto* out = result.data() + static_cast<std::size_t>(y) * layout.extent.width;
        switch (layout.packing) {
            case RawPacking::Raw10:
                unpackRaw10Row(row, layout.extent.width, out);
                break;
            case RawPacking::Raw12:
                unpackRaw12Row(row, layout.extent.width, out);
                break;
            case RawPacking::Raw16:
                for (std::uint32_t x = 0; x < layout.extent.width; ++x) {
                    const auto low = static_cast<std::uint16_t>(row[2U * x]);
                    const auto high = static_cast<std::uint16_t>(row[2U * x + 1U]);
                    out[x] = static_cast<std::uint16_t>(low | (high << 8U));
                }
                break;
        }
    }

    return result;
}

}  // namespace latent::imaging
