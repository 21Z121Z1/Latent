#include "latent/codec/UltraHdrEncoder.h"
#include "latent/codec/UltraHdrStaging.h"
#include "latent/render/ReferenceRenderer.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void checkInvalidArgument(
    const std::function<void()>& operation,
    const std::string& message) {
    bool threw = false;
    try {
        operation();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, message);
}

latent::imaging::RenderedFrame makeSdr() {
    latent::imaging::RenderedFrame frame{};
    frame.sourceRawId = 7U;
    frame.intent = latent::imaging::RenderIntent::SDR;
    frame.primaries = latent::imaging::Primaries::SRGBRec709;
    frame.whitePoint = latent::imaging::WhitePoint::D65;
    frame.transfer = latent::imaging::TransferFunction::SRGB;
    frame.reference = latent::imaging::ReferenceDomain::Display;
    frame.range = latent::imaging::RangeSemantics::EncodedDisplay;
    frame.allowNegative = false;
    frame.nominalWhiteNits = 100.0F;
    frame.peakTargetNits = 100.0F;
    frame.image.extent = {2U, 1U};
    frame.image.rgb = {
        0.0F, 0.5F, 1.0F,
        1.0F, 0.0F, 0.25F,
    };
    return frame;
}

latent::imaging::RenderedFrame makeHdr() {
    latent::imaging::RenderedFrame frame{};
    frame.sourceRawId = 7U;
    frame.intent = latent::imaging::RenderIntent::HDR;
    frame.primaries = latent::imaging::Primaries::BT2020;
    frame.whitePoint = latent::imaging::WhitePoint::D65;
    frame.transfer = latent::imaging::TransferFunction::PQ;
    frame.reference = latent::imaging::ReferenceDomain::Display;
    frame.range = latent::imaging::RangeSemantics::EncodedDisplay;
    frame.allowNegative = false;
    frame.nominalWhiteNits = 203.0F;
    frame.peakTargetNits = 1000.0F;
    frame.hdrHeadroom = 1000.0F / 203.0F;
    frame.image.extent = {2U, 1U};
    frame.image.rgb = {
        0.0F, 0.5F, 1.0F,
        1.0F, 0.0F, 0.25F,
    };
    return frame;
}

void testDeterministicPacking() {
    using namespace latent::codec;

    const auto pair = stageUltraHdrRenditions(makeSdr(), makeHdr());
    check(pair.sourceRawId == 7U,
          "staging must preserve source provenance");
    check(pair.sdr.format == PackedPixelFormat::Rgba8888 &&
              pair.hdr.format == PackedPixelFormat::Rgba1010102,
          "staging must select libultrahdr packed formats by rendition intent");
    check(pair.sdr.pixels.size() == 2U && pair.hdr.pixels.size() == 2U,
          "staging must emit one packed word per pixel");
    check(pair.sdr.rowStridePixels == 2U && pair.hdr.rowStridePixels == 2U,
          "staging must express packed stride in pixels");

    check(pair.sdr.pixels[0] == 0xffff8000U,
          "SDR pack must use little-endian RGBA8888 channel bit positions");
    check(pair.sdr.pixels[1] == 0xff4000ffU,
          "SDR UNORM quantization must round deterministically");
    check(pair.hdr.pixels[0] == 0xfff80000U,
          "HDR pack must use little-endian RGBA1010102 channel bit positions");
    check(pair.hdr.pixels[1] == 0xd00003ffU,
          "HDR UNORM quantization must round deterministically");
    check(std::fabs(pair.hdrPeakTargetNits - 1000.0F) < 1.0e-6F &&
              std::fabs(pair.hdrNominalWhiteNits - 203.0F) < 1.0e-6F,
          "staging must preserve HDR display intent metadata");
}

void testStagingValidation() {
    using namespace latent::codec;

    auto sdr = makeSdr();
    auto hdr = makeHdr();

    auto wrongSource = hdr;
    wrongSource.sourceRawId = 8U;
    checkInvalidArgument(
        [&]() { (void)stageUltraHdrRenditions(sdr, wrongSource); },
        "staging must reject renditions from different scene sources");

    auto wrongExtent = hdr;
    wrongExtent.image.extent = {1U, 2U};
    checkInvalidArgument(
        [&]() { (void)stageUltraHdrRenditions(sdr, wrongExtent); },
        "staging must reject mismatched rendition extents");

    auto wrongTransfer = hdr;
    wrongTransfer.transfer = latent::imaging::TransferFunction::HLG;
    checkInvalidArgument(
        [&]() { (void)stageUltraHdrRenditions(sdr, wrongTransfer); },
        "staging must not silently reinterpret non-PQ HDR data");

    auto nonFinite = sdr;
    nonFinite.image.rgb[0] = std::numeric_limits<float>::quiet_NaN();
    checkInvalidArgument(
        [&]() { (void)stageUltraHdrRenditions(nonFinite, hdr); },
        "staging must reject non-finite encoded samples");

    auto outOfRange = sdr;
    outOfRange.image.rgb[0] = 1.01F;
    checkInvalidArgument(
        [&]() { (void)stageUltraHdrRenditions(outOfRange, hdr); },
        "staging must reject encoded samples outside [0, 1]");

    auto wrongNominalWhite = hdr;
    wrongNominalWhite.nominalWhiteNits = 100.0F;
    checkInvalidArgument(
        [&]() { (void)stageUltraHdrRenditions(sdr, wrongNominalWhite); },
        "staging must reject HDR nominal white incompatible with libultrahdr semantics");

    auto lowPeak = hdr;
    lowPeak.peakTargetNits = 200.0F;
    checkInvalidArgument(
        [&]() { (void)stageUltraHdrRenditions(sdr, lowPeak); },
        "staging must enforce libultrahdr target-display peak bounds");
}

#ifdef LATENT_HAS_ULTRAHDR
latent::imaging::SceneFrame makeCodecIntegrationScene() {
    latent::imaging::SceneFrame scene{};
    scene.sourceRawId = 91U;
    scene.image.extent = {16U, 16U};
    scene.image.rgb.reserve(16U * 16U * 3U);
    for (std::uint32_t y = 0U; y < 16U; ++y) {
        for (std::uint32_t x = 0U; x < 16U; ++x) {
            const float stop =
                -4.0F + 8.0F * static_cast<float>(x + y) / 30.0F;
            const float value = 0.18F * std::exp2(stop);
            scene.image.rgb.push_back(value);
            scene.image.rgb.push_back(value);
            scene.image.rgb.push_back(value);
        }
    }
    return scene;
}

void testLibUltraHdrJpegIntegration() {
    using namespace latent::codec;
    using namespace latent::render;

    const auto scene = makeCodecIntegrationScene();
    const auto sdr = renderReference(scene, makeSdrRenderConfig());
    const auto hdr = renderReference(
        scene, makeHdrPqRenderConfig(0.0F, 1000.0F, 203.0F));
    const auto pair = stageUltraHdrRenditions(sdr, hdr);

    UltraHdrEncodeOptions options{};
    options.container = UltraHdrContainer::Jpeg;
    options.gainMapScaleFactor = 2;
    const auto encoded = encodeUltraHdr(pair, options);

    check(encoded.container == UltraHdrContainer::Jpeg,
          "encoder must retain requested output container");
    check(encoded.bytes.size() > 100U,
          "libultrahdr integration must return a non-trivial encoded stream");
    check(encoded.bytes.size() >= 2U && encoded.bytes[0] == 0xffU &&
              encoded.bytes[1] == 0xd8U,
          "JPEG Ultra HDR output must begin with JPEG SOI");

    auto invalidOptions = options;
    invalidOptions.baseQuality = 101;
    checkInvalidArgument(
        [&]() { (void)encodeUltraHdr(pair, invalidOptions); },
        "encoder wrapper must validate quality before invoking libultrahdr");
}
#endif

}  // namespace

int main() {
    try {
        testDeterministicPacking();
        testStagingValidation();
#ifdef LATENT_HAS_ULTRAHDR
        testLibUltraHdrJpegIntegration();
#endif
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
    }

    if (failures != 0) {
        std::cerr << failures << " codec test(s) failed\n";
        return 1;
    }

    std::cout << "codec tests passed\n";
    return 0;
}
