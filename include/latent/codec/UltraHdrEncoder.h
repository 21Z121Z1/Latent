#pragma once

#include "latent/codec/UltraHdrStaging.h"

#include <cstdint>
#include <vector>

namespace latent::codec {

enum class UltraHdrContainer : std::uint8_t {
    Jpeg,
    Heif,
    Avif,
};

enum class UltraHdrPreset : std::uint8_t {
    BestQuality,
    Realtime,
};

struct UltraHdrEncodeOptions {
    UltraHdrContainer container = UltraHdrContainer::Jpeg;
    UltraHdrPreset preset = UltraHdrPreset::BestQuality;
    int baseQuality = 95;
    int gainMapQuality = 95;
    int gainMapScaleFactor = 1;
    bool multiChannelGainMap = true;
};

struct EncodedUltraHdr {
    UltraHdrContainer container = UltraHdrContainer::Jpeg;
    std::vector<std::uint8_t> bytes;
};

#ifdef LATENT_HAS_ULTRAHDR
// Encodes an explicit SDR/HDR rendition pair using libultrahdr. Latent does
// not implement gain-map math, metadata quantization, or container packing;
// those responsibilities stay inside the standards-oriented upstream codec.
[[nodiscard]] EncodedUltraHdr encodeUltraHdr(
    const UltraHdrRenditionPair& renditions,
    const UltraHdrEncodeOptions& options = {});
#endif

}  // namespace latent::codec
