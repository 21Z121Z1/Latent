#include "latent/render/AcesToneScale.h"
#include "latent/render/OutputEncoding.h"
#include "latent/render/ReferenceRenderer.h"
#include "latent/render/SceneAnalysis.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
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

void checkNear(
    float actual,
    float expected,
    float tolerance,
    const std::string& message) {
    if (!std::isfinite(actual) || std::fabs(actual - expected) > tolerance) {
        ++failures;
        std::cerr << "FAIL: " << message << " (actual=" << actual
                  << ", expected=" << expected
                  << ", tolerance=" << tolerance << ")\n";
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

latent::imaging::SceneFrame makeNeutralScene(
    const std::vector<float>& relativeLuminance,
    float sceneScaleEV) {
    latent::imaging::SceneFrame scene{};
    scene.image.extent = {
        static_cast<std::uint32_t>(relativeLuminance.size()), 1U};
    scene.sceneScaleEV = sceneScaleEV;

    const float coordinateScale = std::exp2(sceneScaleEV);
    scene.image.rgb.reserve(relativeLuminance.size() * 3U);
    for (const float value : relativeLuminance) {
        const float stored = value * coordinateScale;
        scene.image.rgb.push_back(stored);
        scene.image.rgb.push_back(stored);
        scene.image.rgb.push_back(stored);
    }
    return scene;
}

void testSceneScaleInvariantAnalysis() {
    using namespace latent::render;

    const std::vector<float> luminance{
        -0.01F, 0.09F, 0.18F, 0.36F, 0.72F};
    const auto base = analyzeSceneLuminance(makeNeutralScene(luminance, 0.0F));
    const auto shifted =
        analyzeSceneLuminance(makeNeutralScene(luminance, 3.0F));

    check(base.pixelCount == 5U, "scene analysis must count all pixels");
    check(base.positiveLuminanceCount == 4U,
          "scene analysis must count positive-luminance pixels");
    check(base.nonPositiveLuminanceCount == 1U,
          "scene analysis must retain a non-positive diagnostic count");

    checkNear(base.minimumEV, shifted.minimumEV, 2.0e-6F,
              "minimum EV must be invariant to sceneScaleEV");
    checkNear(base.p01EV, shifted.p01EV, 2.0e-6F,
              "p01 EV must be invariant to sceneScaleEV");
    checkNear(base.medianEV, shifted.medianEV, 2.0e-6F,
              "median EV must be invariant to sceneScaleEV");
    checkNear(base.p99EV, shifted.p99EV, 2.0e-6F,
              "p99 EV must be invariant to sceneScaleEV");
    checkNear(base.maximumEV, shifted.maximumEV, 2.0e-6F,
              "maximum EV must be invariant to sceneScaleEV");

    const float expectedMedianEV =
        0.5F * (std::log2(0.18F) + std::log2(0.36F));
    checkNear(base.medianEV, expectedMedianEV, 2.0e-5F,
              "scene median must use log-domain percentile semantics");

    const float targetMedian = std::exp2(base.medianEV);
    checkNear(suggestRenderExposureEV(base, targetMedian), 0.0F, 2.0e-6F,
              "render exposure suggestion must be separate from scene scale");

    auto malformed = makeNeutralScene({0.18F}, 0.0F);
    malformed.image.rgb.pop_back();
    checkInvalidArgument(
        [&]() { (void)analyzeSceneLuminance(malformed); },
        "scene analysis must reject payload/extent mismatches");

    checkInvalidArgument(
        [&]() {
            (void)analyzeSceneLuminance(makeNeutralScene({-0.1F, 0.0F}, 0.0F));
        },
        "scene analysis must reject scenes with no positive luminance");
}

void testSrgbEncoding() {
    using namespace latent::render;

    checkNear(encodeSrgb(0.0F), 0.0F, 1.0e-7F, "sRGB black");
    checkNear(encodeSrgb(1.0F), 1.0F, 1.0e-6F, "sRGB white");
    checkNear(encodeSrgb(0.18F), 0.46135613F, 2.0e-6F,
              "sRGB 18 percent linear golden value");

    for (const float linear : {0.0F, 0.0031308F, 0.18F, 0.5F, 1.0F}) {
        checkNear(decodeSrgb(encodeSrgb(linear)), linear, 3.0e-6F,
                  "sRGB encode/decode round trip");
    }

    checkInvalidArgument(
        []() { (void)encodeSrgb(-0.01F); },
        "sRGB encoding must not hide negative/gamut clipping");
    checkInvalidArgument(
        []() { (void)decodeSrgb(1.01F); },
        "sRGB decoding must reject out-of-range signal values");
}

void testPqEncoding() {
    using namespace latent::render;

    checkNear(encodePqFromNits(0.0F), 7.3095590e-7F, 2.0e-10F,
              "PQ zero-luminance golden value");
    checkNear(encodePqFromNits(100.0F), 0.50807842F, 2.0e-6F,
              "PQ 100-nit golden value");
    checkNear(encodePqFromNits(1000.0F), 0.75182710F, 2.0e-6F,
              "PQ 1000-nit golden value");
    checkNear(encodePqFromNits(10000.0F), 1.0F, 2.0e-6F,
              "PQ 10000-nit golden value");

    for (const float nits : {0.0F, 0.1F, 1.0F, 100.0F, 203.0F,
                             1000.0F, 4000.0F, 10000.0F}) {
        const float decoded = decodePqToNits(encodePqFromNits(nits));
        const float tolerance = std::max(2.0e-4F, nits * 2.0e-5F);
        checkNear(decoded, nits, tolerance, "PQ encode/decode round trip");
    }

    checkInvalidArgument(
        []() { (void)encodePqFromNits(10001.0F); },
        "PQ encoding must reject luminance above the ST2084 range");
    checkInvalidArgument(
        []() { (void)decodePqToNits(-0.01F); },
        "PQ decoding must reject negative signal values");
}

void testAcesToneScalePrimitive() {
    using namespace latent::render;

    const auto sdr = makeAcesToneScaleParams(100.0F);
    checkNear(sdr.s2, 0.9198583F, 3.0e-5F,
              "ACES 100-nit tonescale s2 parameter");
    checkNear(sdr.u2, 0.9917990F, 3.0e-5F,
              "ACES 100-nit tonescale u2 parameter");
    checkNear(sdr.m2, 1.0471038F, 3.0e-5F,
              "ACES 100-nit tonescale m2 parameter");
    checkNear(sdr.forwardLimit, 1024.0F, 2.0e-3F,
              "ACES 100-nit forward limit");
    checkNear(acesToneScaleForward(0.18F, sdr), 9.99993F, 2.0e-3F,
              "ACES 100-nit middle-grey golden value");
    checkNear(acesToneScaleForward(128.0F, sdr), 100.0F, 2.0e-3F,
              "ACES 100-nit roof golden value");

    const auto hdr = makeAcesToneScaleParams(1000.0F);
    checkNear(hdr.s2, 5.895927F, 2.0e-4F,
              "ACES 1000-nit tonescale s2 parameter");
    checkNear(hdr.m2, 10.172911F, 3.0e-4F,
              "ACES 1000-nit tonescale m2 parameter");
    checkNear(hdr.forwardLimit, 4096.0F, 5.0e-3F,
              "ACES 1000-nit forward limit");
    checkNear(acesToneScaleForward(0.18F, hdr), 14.51155F, 4.0e-3F,
              "ACES 1000-nit middle-grey golden value");

    checkInvalidArgument(
        []() { (void)makeAcesToneScaleParams(99.0F); },
        "ACES tonescale must reject peaks below its supported design range");
    checkInvalidArgument(
        [&]() { (void)acesToneScaleForward(-0.1F, sdr); },
        "ACES scalar tonescale must reject negative luminance input");
}

void testIndependentReferenceRenderBranches() {
    using namespace latent::render;

    auto scene = makeNeutralScene({0.18F, 1.0F, 8.0F}, 0.0F);
    scene.sourceRawId = 42U;

    const auto sdr = renderReference(scene, makeSdrRenderConfig());
    const auto hdr = renderReference(scene, makeHdrPqRenderConfig());

    check(sdr.sourceRawId == 42U && hdr.sourceRawId == 42U,
          "rendered renditions must retain scene provenance");
    check(sdr.intent == latent::imaging::RenderIntent::SDR,
          "SDR branch must identify its render intent");
    check(hdr.intent == latent::imaging::RenderIntent::HDR,
          "HDR branch must identify its render intent");
    check(sdr.primaries == latent::imaging::Primaries::SRGBRec709 &&
              sdr.transfer == latent::imaging::TransferFunction::SRGB,
          "SDR branch must encode Rec.709/sRGB output semantics");
    check(hdr.primaries == latent::imaging::Primaries::BT2020 &&
              hdr.transfer == latent::imaging::TransferFunction::PQ,
          "HDR branch must encode BT.2020/PQ output semantics");
    check(sdr.reference == latent::imaging::ReferenceDomain::Display &&
              hdr.reference == latent::imaging::ReferenceDomain::Display &&
              sdr.range == latent::imaging::RangeSemantics::EncodedDisplay &&
              hdr.range == latent::imaging::RangeSemantics::EncodedDisplay,
          "both renditions must explicitly cross into the display domain");
    check(sdr.image.extent.width == 3U && sdr.image.rgb.size() == 9U &&
              hdr.image.rgb.size() == 9U,
          "reference renderer must preserve image extent and RGB payload size");

    const float sdrMiddleGrayNits = decodeSrgb(sdr.image.rgb[0]) * 100.0F;
    const float hdrMiddleGrayNits = decodePqToNits(hdr.image.rgb[0]);
    checkNear(
        sdrMiddleGrayNits,
        acesToneScaleForward(0.18F, makeAcesToneScaleParams(100.0F)),
        3.0e-3F,
        "SDR neutral rendering must follow the 100-nit tone intent");
    checkNear(
        hdrMiddleGrayNits,
        acesToneScaleForward(0.18F, makeAcesToneScaleParams(1000.0F)),
        6.0e-3F,
        "HDR neutral rendering must follow its independent 1000-nit tone intent");
    check(hdrMiddleGrayNits > sdrMiddleGrayNits,
          "HDR rendering must not be reconstructed from the SDR tone-mapped result");
    checkNear(hdr.hdrHeadroom, 1000.0F / 203.0F, 2.0e-6F,
              "HDR headroom metadata must derive from explicit display intent");

    for (std::size_t pixel = 0U; pixel < 3U; ++pixel) {
        const std::size_t base = pixel * 3U;
        checkNear(sdr.image.rgb[base], sdr.image.rgb[base + 1U], 3.0e-5F,
                  "neutral AP1 samples must remain neutral in SDR output");
        checkNear(sdr.image.rgb[base], sdr.image.rgb[base + 2U], 3.0e-5F,
                  "neutral AP1 samples must remain neutral in SDR output");
        checkNear(hdr.image.rgb[base], hdr.image.rgb[base + 1U], 3.0e-5F,
                  "neutral AP1 samples must remain neutral in HDR output");
        checkNear(hdr.image.rgb[base], hdr.image.rgb[base + 2U], 3.0e-5F,
                  "neutral AP1 samples must remain neutral in HDR output");
    }
}

void testRenderSceneScaleAndExposureSeparation() {
    using namespace latent::render;

    const std::vector<float> values{0.01F, 0.18F, 1.0F, 16.0F};
    const auto base = renderReference(
        makeNeutralScene(values, 0.0F),
        makeHdrPqRenderConfig(0.0F, 1000.0F, 203.0F));
    const auto shifted = renderReference(
        makeNeutralScene(values, 4.0F),
        makeHdrPqRenderConfig(0.0F, 1000.0F, 203.0F));

    check(base.image.rgb.size() == shifted.image.rgb.size(),
          "scene-scale comparison requires matching payloads");
    for (std::size_t index = 0U; index < base.image.rgb.size(); ++index) {
        checkNear(base.image.rgb[index], shifted.image.rgb[index], 3.0e-6F,
                  "rendering must be invariant to SceneFrame coordinate scale");
    }

    const auto exposed = renderReference(
        makeNeutralScene({0.18F}, 3.0F),
        makeSdrRenderConfig(1.0F));
    const float exposedNits = decodeSrgb(exposed.image.rgb[0]) * 100.0F;
    checkNear(
        exposedNits,
        acesToneScaleForward(0.36F, makeAcesToneScaleParams(100.0F)),
        5.0e-3F,
        "render exposure must act on exposure-relative scene values after scene unscale");
}

void testExplicitGamutAndNegativeHandling() {
    using namespace latent::render;

    latent::imaging::SceneFrame scene{};
    scene.image.extent = {2U, 1U};
    scene.image.rgb = {
        2.0F, -0.25F, 0.10F,
        -0.10F, -0.10F, -0.10F,
    };
    const auto original = scene.image.rgb;

    const auto sdr = renderReference(scene, makeSdrRenderConfig());
    const auto hdr = renderReference(scene, makeHdrPqRenderConfig());

    for (const float encoded : sdr.image.rgb) {
        check(std::isfinite(encoded) && encoded >= 0.0F && encoded <= 1.0F,
              "SDR gamut mapping must produce finite encoded display coordinates");
    }
    for (const float encoded : hdr.image.rgb) {
        check(std::isfinite(encoded) && encoded >= 0.0F && encoded <= 1.0F,
              "HDR gamut mapping must produce finite encoded display coordinates");
    }
    check(scene.image.rgb == original,
          "rendering must never rewrite the scene-referred master");

    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        check(sdr.image.rgb[3U + channel] <= 1.0e-6F,
              "non-positive scene luminance may be clipped only at the render boundary");
        check(hdr.image.rgb[3U + channel] <= 1.0e-5F,
              "non-positive scene luminance must render to HDR black");
    }
}

void testReferenceRendererValidation() {
    using namespace latent::render;

    auto malformed = makeNeutralScene({0.18F}, 0.0F);
    malformed.transfer = latent::imaging::TransferFunction::SRGB;
    checkInvalidArgument(
        [&]() { (void)renderReference(malformed, makeSdrRenderConfig()); },
        "renderer must reject display transfer functions on SceneFrame input");

    auto nonFinite = makeNeutralScene({0.18F}, 0.0F);
    nonFinite.image.rgb[1] = std::numeric_limits<float>::quiet_NaN();
    checkInvalidArgument(
        [&]() { (void)renderReference(nonFinite, makeSdrRenderConfig()); },
        "renderer must reject non-finite scene samples");

    checkInvalidArgument(
        []() { (void)makeSdrRenderConfig(0.0F, 50.0F); },
        "SDR config must reject peaks outside the supported tone-scale range");
    checkInvalidArgument(
        []() { (void)makeHdrPqRenderConfig(0.0F, 1000.0F, 1200.0F); },
        "HDR config must reject nominal white above target peak");
    checkInvalidArgument(
        []() {
            (void)makeHdrPqRenderConfig(
                std::numeric_limits<float>::infinity(), 1000.0F, 203.0F);
        },
        "render config must reject non-finite exposure intent");
}

}  // namespace

int main() {
    try {
        testSceneScaleInvariantAnalysis();
        testSrgbEncoding();
        testPqEncoding();
        testAcesToneScalePrimitive();
        testIndependentReferenceRenderBranches();
        testRenderSceneScaleAndExposureSeparation();
        testExplicitGamutAndNegativeHandling();
        testReferenceRendererValidation();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
    }

    if (failures != 0) {
        std::cerr << failures << " render/reference test(s) failed\n";
        return 1;
    }

    std::cout << "render/reference tests passed\n";
    return 0;
}
