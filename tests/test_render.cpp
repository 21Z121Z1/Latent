#include "latent/render/AcesToneScale.h"
#include "latent/render/OutputEncoding.h"
#include "latent/render/SceneAnalysis.h"

#include <cmath>
#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
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

}  // namespace

int main() {
    try {
        testSceneScaleInvariantAnalysis();
        testSrgbEncoding();
        testPqEncoding();
        testAcesToneScalePrimitive();
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
