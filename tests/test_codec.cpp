#include "latent/codec/UltraHdrEncoder.h"
#include "latent/codec/UltraHdrStaging.h"
#include "latent/render/ReferenceRenderer.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const std::string& message) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void checkInvalid(const std::function<void()>& fn, const std::string& message) {
    bool threw = false;
    try { fn(); } catch (const std::invalid_argument&) { threw = true; }
    check(threw, message);
}

latent::imaging::RenderedFrame makeRendition(bool hdr) {
    using namespace latent::imaging;
    RenderedFrame frame{};
    frame.sourceRawId = 7U;
    frame.intent = hdr ? RenderIntent::HDR : RenderIntent::SDR;
    frame.primaries = hdr ? Primaries::BT2020 : Primaries::SRGBRec709;
    frame.whitePoint = WhitePoint::D65;
    frame.transfer = hdr ? TransferFunction::PQ : TransferFunction::SRGB;
    frame.reference = ReferenceDomain::Display;
    frame.range = RangeSemantics::EncodedDisplay;
    frame.allowNegative = false;
    frame.nominalWhiteNits = hdr ? 203.0F : 100.0F;
    frame.peakTargetNits = hdr ? 1000.0F : 100.0F;
    frame.hdrHeadroom = hdr ? 1000.0F / 203.0F : 1.0F;
    frame.image.extent = {2U, 1U};
    frame.image.rgb = {0.0F, 0.5F, 1.0F, 1.0F, 0.0F, 0.25F};
    return frame;
}

void testPackingAndValidation() {
    using namespace latent::codec;
    auto sdr = makeRendition(false);
    auto hdr = makeRendition(true);
    const auto pair = stageUltraHdrRenditions(sdr, hdr);
    check(pair.sourceRawId == 7U, "staging preserves provenance");
    check(pair.sdr.format == PackedPixelFormat::Rgba8888 &&
          pair.hdr.format == PackedPixelFormat::Rgba1010102,
          "staging selects documented packed formats");
    check(pair.sdr.pixels.size() == 2U && pair.hdr.pixels.size() == 2U,
          "one packed word per pixel");
    check(pair.sdr.pixels[0] == 0xffff8000U && pair.sdr.pixels[1] == 0xff4000ffU,
          "RGBA8888 packing and rounding");
    check(pair.hdr.pixels[0] == 0xfff80000U && pair.hdr.pixels[1] == 0xd00003ffU,
          "RGBA1010102 packing and rounding");
    check(std::fabs(pair.hdrNominalWhiteNits - 203.0F) < 1.0e-6F,
          "203-nit nominal white preserved");

    auto bad = hdr; bad.sourceRawId = 8U;
    checkInvalid([&] { (void)stageUltraHdrRenditions(sdr, bad); }, "source mismatch rejected");
    bad = hdr; bad.transfer = latent::imaging::TransferFunction::HLG;
    checkInvalid([&] { (void)stageUltraHdrRenditions(sdr, bad); }, "non-PQ HDR rejected");
    bad = hdr; bad.nominalWhiteNits = 100.0F;
    checkInvalid([&] { (void)stageUltraHdrRenditions(sdr, bad); }, "non-203 nominal white rejected");
    bad = hdr; bad.peakTargetNits = 200.0F;
    checkInvalid([&] { (void)stageUltraHdrRenditions(sdr, bad); }, "low HDR peak rejected");
    auto badSdr = sdr; badSdr.image.rgb[0] = std::numeric_limits<float>::quiet_NaN();
    checkInvalid([&] { (void)stageUltraHdrRenditions(badSdr, hdr); }, "NaN rejected");
    badSdr = sdr; badSdr.image.rgb[0] = 1.01F;
    checkInvalid([&] { (void)stageUltraHdrRenditions(badSdr, hdr); }, "out-of-range sample rejected");
}

#ifdef LATENT_HAS_ULTRAHDR
latent::imaging::SceneFrame makeScene() {
    latent::imaging::SceneFrame scene{};
    scene.sourceRawId = 91U;
    scene.image.extent = {16U, 16U};
    scene.image.rgb.reserve(16U * 16U * 3U);
    for (std::uint32_t y = 0; y < 16U; ++y) {
        for (std::uint32_t x = 0; x < 16U; ++x) {
            const float value = 0.18F * std::exp2(-4.0F + 8.0F * static_cast<float>(x + y) / 30.0F);
            scene.image.rgb.insert(scene.image.rgb.end(), {value, value, value});
        }
    }
    return scene;
}

void testRealCodecRoundTripProbe() {
    using namespace latent::codec;
    using namespace latent::render;
    const auto scene = makeScene();
    const auto sdr = renderReference(scene, makeSdrRenderConfig());
    const auto hdr = renderReference(scene, makeHdrPqRenderConfig(0.0F, 1000.0F, 203.0F));
    const auto pair = stageUltraHdrRenditions(sdr, hdr);
    UltraHdrEncodeOptions options{};
    options.gainMapScaleFactor = 2;
    const auto encoded = encodeUltraHdr(pair, options);
    check(encoded.bytes.size() > 100U && encoded.bytes[0] == 0xffU && encoded.bytes[1] == 0xd8U,
          "real encoder returns JPEG stream");

    const auto probe = probeUltraHdr(encoded);
    check(probe.imageWidth == 16 && probe.imageHeight == 16, "probe recovers base extent");
    check(probe.gainMapWidth > 0 && probe.gainMapHeight > 0,
          "probe finds gain-map image");
    check(probe.gainMapWidth <= probe.imageWidth && probe.gainMapHeight <= probe.imageHeight,
          "gain map does not exceed base extent");
    check(probe.hasGainMapMetadata, "probe finds gain-map metadata");

    auto invalid = options; invalid.baseQuality = 101;
    checkInvalid([&] { (void)encodeUltraHdr(pair, invalid); }, "invalid quality rejected");
    EncodedUltraHdr empty{};
    checkInvalid([&] { (void)probeUltraHdr(empty); }, "empty probe input rejected");
}
#endif
}  // namespace

int main() {
    try {
        testPackingAndValidation();
#ifdef LATENT_HAS_ULTRAHDR
        testRealCodecRoundTripProbe();
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
