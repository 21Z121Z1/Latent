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

struct UltraHdrProbe {
    int imageWidth = 0;
    int imageHeight = 0;
    int gainMapWidth = 0;
    int gainMapHeight = 0;
    bool hasGainMapMetadata = false;
};

#ifdef LATENT_HAS_ULTRAHDR
// Encodes an explicit SDR/HDR rendition pair using libultrahdr. Latent does
// not implement gain-map math, metadata quantization, or container packing;
// those responsibilities stay inside the standards-oriented upstream codec.
[[nodiscard]] EncodedUltraHdr encodeUltraHdr(
    const UltraHdrRenditionPair& renditions,
    const UltraHdrEncodeOptions& options = {});

// Parses the encoded stream with libultrahdr's own decoder probe. This is
// intentionally metadata-only: it validates that the container exposes both
// the base rendition and a gain map without paying for a full image decode.
[[nodiscard]] UltraHdrProbe probeUltraHdr(const EncodedUltraHdr& encoded);
#endif

}  // namespace latent::codec
