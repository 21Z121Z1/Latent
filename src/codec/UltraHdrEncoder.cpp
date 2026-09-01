#include "latent/codec/UltraHdrEncoder.h"

#include <ultrahdr_api.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#if UHDR_LIB_VER_MAJOR < 2
#error "Latent Ultra HDR integration requires libultrahdr 2.x or newer"
#endif

namespace latent::codec {
namespace {

constexpr float kUltraHdrSdrWhiteNits = 203.0F;
constexpr float kWhiteToleranceNits = 1.0e-3F;

void requireOk(const uhdr_error_info_t& status, const char* operation) {
    if (status.error_code == UHDR_CODEC_OK) {
        return;
    }

    std::string message(operation);
    if (status.has_detail != 0) {
        std::size_t detailLength = 0U;
        while (detailLength < sizeof(status.detail) &&
               status.detail[detailLength] != '\0') {
            ++detailLength;
        }
        if (detailLength != 0U) {
            message += ": ";
            message.append(status.detail, detailLength);
        }
    }
    throw std::runtime_error(message);
}

void validateOptions(const UltraHdrEncodeOptions& options) {
    if (options.baseQuality < 0 || options.baseQuality > 100 ||
        options.gainMapQuality < 0 || options.gainMapQuality > 100) {
        throw std::invalid_argument("Ultra HDR quality must be in [0, 100]");
    }
    if (options.gainMapScaleFactor < 1 || options.gainMapScaleFactor > 128) {
        throw std::invalid_argument(
            "Ultra HDR gain-map scale factor must be in [1, 128]");
    }
}

void validateRenditions(const UltraHdrRenditionPair& pair) {
    if (pair.sdr.format != PackedPixelFormat::Rgba8888 ||
        pair.sdr.primaries != imaging::Primaries::SRGBRec709 ||
        pair.sdr.transfer != imaging::TransferFunction::SRGB) {
        throw std::invalid_argument(
            "libultrahdr SDR input must be staged Rec.709/sRGB RGBA8888");
    }
    if (pair.hdr.format != PackedPixelFormat::Rgba1010102 ||
        pair.hdr.primaries != imaging::Primaries::BT2020 ||
        pair.hdr.transfer != imaging::TransferFunction::PQ) {
        throw std::invalid_argument(
            "libultrahdr HDR input must be staged BT.2020/PQ RGBA1010102");
    }
    if (pair.sdr.extent.width == 0U || pair.sdr.extent.height == 0U ||
        pair.sdr.extent.width != pair.hdr.extent.width ||
        pair.sdr.extent.height != pair.hdr.extent.height) {
        throw std::invalid_argument(
            "libultrahdr staged renditions require identical non-zero extents");
    }

    const std::uint64_t pixelCount = pair.sdr.extent.pixelCount();
    if (pixelCount >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::invalid_argument(
            "libultrahdr staged extent is too large for host indexing");
    }
    const std::size_t hostPixelCount = static_cast<std::size_t>(pixelCount);
    if (pair.sdr.pixels.size() != hostPixelCount ||
        pair.hdr.pixels.size() != hostPixelCount) {
        throw std::invalid_argument(
            "libultrahdr staged pixel payload does not match rendition extent");
    }
    if (pair.sdr.rowStridePixels < pair.sdr.extent.width ||
        pair.hdr.rowStridePixels < pair.hdr.extent.width) {
        throw std::invalid_argument("libultrahdr row stride is smaller than width");
    }
    if (!std::isfinite(pair.hdrNominalWhiteNits) ||
        std::fabs(pair.hdrNominalWhiteNits - kUltraHdrSdrWhiteNits) >
            kWhiteToleranceNits) {
        throw std::invalid_argument(
            "current libultrahdr gain-map semantics require 203-nit HDR nominal white");
    }
    if (!std::isfinite(pair.hdrPeakTargetNits) ||
        pair.hdrPeakTargetNits < kUltraHdrSdrWhiteNits ||
        pair.hdrPeakTargetNits > 10000.0F) {
        throw std::invalid_argument(
            "libultrahdr target display peak must be in [203, 10000] nits");
    }
}

uhdr_codec_t toCodec(UltraHdrContainer container) {
    switch (container) {
        case UltraHdrContainer::Jpeg:
            return UHDR_CODEC_JPG;
        case UltraHdrContainer::Heif:
            return UHDR_CODEC_HEIF;
        case UltraHdrContainer::Avif:
            return UHDR_CODEC_AVIF;
    }
    throw std::invalid_argument("unsupported Ultra HDR container");
}

uhdr_enc_preset_t toPreset(UltraHdrPreset preset) {
    switch (preset) {
        case UltraHdrPreset::BestQuality:
            return UHDR_USAGE_BEST_QUALITY;
        case UltraHdrPreset::Realtime:
            return UHDR_USAGE_REALTIME;
    }
    throw std::invalid_argument("unsupported Ultra HDR preset");
}

uhdr_raw_image_t makeSdrDescriptor(const PackedRendition& sdr) {
    uhdr_raw_image_t image{};
    image.fmt = UHDR_IMG_FMT_32bppRGBA8888;
    image.cg = UHDR_CG_BT_709;
    image.ct = UHDR_CT_SRGB;
    image.range = UHDR_CR_FULL_RANGE;
    image.w = static_cast<unsigned int>(sdr.extent.width);
    image.h = static_cast<unsigned int>(sdr.extent.height);
    // libultrahdr's C API is not const-correct for encoder input. The encoder
    // treats registered raw-image planes as input; Latent keeps ownership.
    image.planes[UHDR_PLANE_PACKED] =
        const_cast<std::uint32_t*>(sdr.pixels.data());
    image.planes[UHDR_PLANE_U] = nullptr;
    image.planes[UHDR_PLANE_V] = nullptr;
    image.stride[UHDR_PLANE_PACKED] =
        static_cast<unsigned int>(sdr.rowStridePixels);
    image.stride[UHDR_PLANE_U] = 0U;
    image.stride[UHDR_PLANE_V] = 0U;
    return image;
}

uhdr_raw_image_t makeHdrDescriptor(const PackedRendition& hdr) {
    uhdr_raw_image_t image{};
    image.fmt = UHDR_IMG_FMT_32bppRGBA1010102;
    image.cg = UHDR_CG_BT_2100;
    image.ct = UHDR_CT_PQ;
    image.range = UHDR_CR_FULL_RANGE;
    image.w = static_cast<unsigned int>(hdr.extent.width);
    image.h = static_cast<unsigned int>(hdr.extent.height);
    image.planes[UHDR_PLANE_PACKED] =
        const_cast<std::uint32_t*>(hdr.pixels.data());
    image.planes[UHDR_PLANE_U] = nullptr;
    image.planes[UHDR_PLANE_V] = nullptr;
    image.stride[UHDR_PLANE_PACKED] =
        static_cast<unsigned int>(hdr.rowStridePixels);
    image.stride[UHDR_PLANE_U] = 0U;
    image.stride[UHDR_PLANE_V] = 0U;
    return image;
}

uhdr_compressed_image_t makeCompressedDescriptor(
    const EncodedUltraHdr& encoded) {
    if (encoded.bytes.empty()) {
        throw std::invalid_argument("cannot probe an empty Ultra HDR stream");
    }
    uhdr_compressed_image_t image{};
    image.data = const_cast<std::uint8_t*>(encoded.bytes.data());
    image.data_sz = encoded.bytes.size();
    image.capacity = encoded.bytes.size();
    image.cg = UHDR_CG_UNSPECIFIED;
    image.ct = UHDR_CT_UNSPECIFIED;
    image.range = UHDR_CR_UNSPECIFIED;
    return image;
}

}  // namespace

EncodedUltraHdr encodeUltraHdr(
    const UltraHdrRenditionPair& renditions,
    const UltraHdrEncodeOptions& options) {
    validateOptions(options);
    validateRenditions(renditions);

    using Encoder = std::unique_ptr<uhdr_codec_private_t, void (*)(uhdr_codec_private_t*)>;
    Encoder encoder(uhdr_create_encoder(), &uhdr_release_encoder);
    if (!encoder) {
        throw std::runtime_error("libultrahdr failed to create encoder");
    }

    uhdr_raw_image_t sdr = makeSdrDescriptor(renditions.sdr);
    uhdr_raw_image_t hdr = makeHdrDescriptor(renditions.hdr);

    requireOk(
        uhdr_enc_set_raw_image(encoder.get(), &hdr, UHDR_HDR_IMG),
        "libultrahdr rejected HDR rendition");
    requireOk(
        uhdr_enc_set_raw_image(encoder.get(), &sdr, UHDR_SDR_IMG),
        "libultrahdr rejected SDR rendition");
    requireOk(
        uhdr_enc_set_quality(encoder.get(), options.baseQuality, UHDR_BASE_IMG),
        "libultrahdr rejected base-image quality");
    requireOk(
        uhdr_enc_set_quality(
            encoder.get(), options.gainMapQuality, UHDR_GAIN_MAP_IMG),
        "libultrahdr rejected gain-map quality");
    requireOk(
        uhdr_enc_set_gainmap_scale_factor(
            encoder.get(), options.gainMapScaleFactor),
        "libultrahdr rejected gain-map scale factor");
    requireOk(
        uhdr_enc_set_using_multi_channel_gainmap(
            encoder.get(), options.multiChannelGainMap ? 1 : 0),
        "libultrahdr rejected gain-map channel mode");
    requireOk(
        uhdr_enc_set_target_display_peak_brightness(
            encoder.get(), renditions.hdrPeakTargetNits),
        "libultrahdr rejected target display peak");
    requireOk(
        uhdr_enc_set_preset(encoder.get(), toPreset(options.preset)),
        "libultrahdr rejected encoder preset");
    requireOk(
        uhdr_enc_set_output_format(encoder.get(), toCodec(options.container)),
        "libultrahdr rejected output container");
    requireOk(uhdr_encode(encoder.get()), "libultrahdr encode failed");

    const uhdr_compressed_image_t* stream = uhdr_get_encoded_stream(encoder.get());
    if (stream == nullptr || stream->data == nullptr || stream->data_sz == 0U) {
        throw std::runtime_error("libultrahdr returned an empty encoded stream");
    }

    const auto* begin = static_cast<const std::uint8_t*>(stream->data);
    EncodedUltraHdr result{};
    result.container = options.container;
    result.bytes.assign(begin, begin + stream->data_sz);
    return result;
}

UltraHdrProbe probeUltraHdr(const EncodedUltraHdr& encoded) {
    uhdr_compressed_image_t image = makeCompressedDescriptor(encoded);

    using Decoder = std::unique_ptr<uhdr_codec_private_t, void (*)(uhdr_codec_private_t*)>;
    Decoder decoder(uhdr_create_decoder(), &uhdr_release_decoder);
    if (!decoder) {
        throw std::runtime_error("libultrahdr failed to create decoder probe");
    }

    requireOk(
        uhdr_dec_set_image(decoder.get(), &image),
        "libultrahdr rejected encoded stream for probing");
    requireOk(uhdr_dec_probe(decoder.get()), "libultrahdr probe failed");

    UltraHdrProbe result{};
    result.imageWidth = uhdr_dec_get_image_width(decoder.get());
    result.imageHeight = uhdr_dec_get_image_height(decoder.get());
    result.gainMapWidth = uhdr_dec_get_gainmap_width(decoder.get());
    result.gainMapHeight = uhdr_dec_get_gainmap_height(decoder.get());
    result.hasGainMapMetadata =
        uhdr_dec_get_gainmap_metadata(decoder.get()) != nullptr;

    if (result.imageWidth <= 0 || result.imageHeight <= 0 ||
        result.gainMapWidth <= 0 || result.gainMapHeight <= 0 ||
        !result.hasGainMapMetadata) {
        throw std::runtime_error(
            "libultrahdr probe did not find a complete base-image/gain-map container");
    }
    return result;
}

}  // namespace latent::codec
