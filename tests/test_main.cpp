#include "latent/backend/Backend.h"
#include "latent/graph/ImagingIR.h"
#include "latent/reference/RawNormalize.h"
#include "latent/vulkan/DeviceCaps.h"

#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void checkNear(float actual, float expected, float epsilon, const std::string& message) {
    if (std::fabs(actual - expected) > epsilon) {
        ++failures;
        std::cerr << "FAIL: " << message << " expected=" << expected << " actual=" << actual << '\n';
    }
}

latent::imaging::RawFrame makeRaw4x4() {
    using namespace latent::imaging;
    RawFrame raw{};
    raw.id = 42;
    raw.storage.extent = {4, 4};
    raw.storage.rowStridePixels = 4;
    raw.storage.pixels = {
        50, 200, 50, 200,
        200, 900, 200, 900,
        50, 200, 50, 200,
        200, 900, 200, 900,
    };
    raw.cfa = CfaPattern::RGGB;
    raw.exposureTimeNs = 10'000'000;
    raw.sensitivityIso = 100.0F;
    raw.staticBlack = MetadataValue<BlackLevel>{BlackLevel{{100.0F, 100.0F, 100.0F, 100.0F}}, MetadataSource::StaticCharacteristic, MetadataValidity::Valid, 0.7F};
    raw.dynamicBlack = MetadataValue<BlackLevel>{BlackLevel{{110.0F, 110.0F, 110.0F, 110.0F}}, MetadataSource::DynamicCaptureResult, MetadataValidity::Valid, 0.9F};
    raw.opticalBlack = MetadataValue<BlackLevel>{BlackLevel{{120.0F, 120.0F, 120.0F, 120.0F}}, MetadataSource::OpticalBlackEstimate, MetadataValidity::Valid, 0.95F};
    raw.staticWhite = MetadataValue<float>{1000.0F, MetadataSource::StaticCharacteristic, MetadataValidity::Valid, 0.8F};
    raw.dynamicWhite = MetadataValue<float>{920.0F, MetadataSource::DynamicCaptureResult, MetadataValidity::Valid, 0.95F};
    return raw;
}

void testMetadataPriorityAndNegativePreservation() {
    const auto raw = makeRaw4x4();
    const auto normalized = latent::reference::normalizeRaw(raw);
    check(normalized.levels.blackSource == latent::imaging::MetadataSource::OpticalBlackEstimate,
          "optical black must outrank dynamic/static black");
    check(normalized.levels.whiteSource == latent::imaging::MetadataSource::DynamicCaptureResult,
          "dynamic white must outrank static white");
    check(normalized.samples.front() < 0.0F,
          "black-subtracted negative samples must not be clamped");
    checkNear(normalized.samples.front(), (50.0F - 120.0F) / (920.0F - 120.0F), 1.0e-6F,
              "sensor normalization must use selected black/white levels");
}

void testSceneValuesRemainUnbounded() {
    auto raw = makeRaw4x4();
    raw.storage.pixels[5] = 1200;

    latent::reference::ReconstructionConfig config{};
    config.whiteBalanceGains = {2.0F, 1.0F, 1.0F, 1.0F};
    config.cameraToAcescg.values = {
        1.0F, -0.5F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F,
    };
    config.sceneScaleEV = 1.0F;

    latent::backend::ReferenceFP32Backend backend;
    const auto scene = backend.reconstructSingleRaw(raw, config);
    check(scene.reference == latent::imaging::ReferenceDomain::Scene,
          "reference backend must produce scene-referred output");
    check(scene.transfer == latent::imaging::TransferFunction::Linear,
          "scene output must remain linear");
    check(scene.range == latent::imaging::RangeSemantics::Unbounded,
          "scene output must be unbounded");
    check(scene.allowNegative, "scene output must permit negative values");
    checkNear(scene.sceneScaleEV, 1.0F, 1.0e-6F, "scene scale must be explicit metadata");

    bool hasAboveOne = false;
    bool hasNegative = false;
    for (const auto value : scene.image.rgb) {
        hasAboveOne = hasAboveOne || value > 1.0F;
        hasNegative = hasNegative || value < 0.0F;
    }
    check(hasAboveOne, "reconstruction must not clamp scene highlights to 1.0");
    check(hasNegative, "camera-to-scene transform must preserve negative coordinates");
}

void testSceneScaleChangesStoredCoordinates() {
    const auto raw = makeRaw4x4();
    latent::reference::ReconstructionConfig base{};
    base.sceneScaleEV = 0.0F;
    latent::reference::ReconstructionConfig shifted = base;
    shifted.sceneScaleEV = 1.0F;

    const auto scene0 = latent::reference::reconstructSingleRaw(raw, base);
    const auto scene1 = latent::reference::reconstructSingleRaw(raw, shifted);
    check(scene0.image.rgb.size() == scene1.image.rgb.size(),
          "scene-scale comparison must preserve image shape");
    for (std::size_t i = 0; i < scene0.image.rgb.size(); ++i) {
        checkNear(scene1.image.rgb[i], scene0.image.rgb[i] * 2.0F, 1.0e-6F,
                  "sceneScaleEV +1 must double stored scene coordinates");
    }
}

void testImageTypeValidation() {
    latent::imaging::ImageType scene{};
    scene.extent = {16, 16};
    check(latent::imaging::validateImageType(scene).valid,
          "default SceneFrame type must be valid");

    scene.transfer = latent::imaging::TransferFunction::SRGB;
    check(!latent::imaging::validateImageType(scene).valid,
          "scene-referred sRGB transfer must be rejected");
}

void testGraphFusionStopsAtReduction() {
    using namespace latent;
    graph::ImagingGraph graph;

    imaging::ImageType sensor{};
    sensor.extent = {16, 16};
    sensor.layout = imaging::PixelLayout::Bayer;
    sensor.colorModel = imaging::ColorModel::SensorCfa;
    sensor.primaries = imaging::Primaries::SensorNative;
    sensor.whitePoint = imaging::WhitePoint::SensorNative;
    sensor.reference = imaging::ReferenceDomain::Sensor;
    sensor.range = imaging::RangeSemantics::SensorReferenceNormalized;
    sensor.allowNegative = true;

    const auto raw = graph.addInput("raw", sensor);
    const auto black = graph.addOperation({
        graph::OperationKind::BlackSubtract, "black", {raw}, sensor,
        graph::AccessPattern::Point, 0, 0, true, true, false, false});
    const auto lsc = graph.addOperation({
        graph::OperationKind::LensShading, "lsc", {black}, sensor,
        graph::AccessPattern::Point, 0, 0, true, true, false, false});
    static_cast<void>(lsc);

    imaging::ImageType stats = sensor;
    stats.extent = {1, 1};
    const auto statsValue = graph.addOperation({
        graph::OperationKind::Analyze, "stats", {lsc}, stats,
        graph::AccessPattern::Reduction, 0, 0, true, false, false, false});
    static_cast<void>(statsValue);

    const auto validation = graph.validate();
    check(validation.valid, "valid graph should pass semantic validation");

    const auto groups = graph.planConservativeFusion();
    check(groups.size() == 2U, "reduction must split a pointwise fusion chain");
    check(groups.front().operationIndices.size() == 2U,
          "black + lens shading should be a fusion candidate");
    check(groups.back().operationIndices.size() == 1U,
          "reduction should remain materialized");
}

void testVulkanCapabilityBaseline() {
    latent::vulkan::DeviceCaps caps{};
    caps.apiVersion = {1, 1, 0};
    caps.computeQueue = true;
    caps.storageBuffer = true;
    caps.commonStorageImages = true;
    caps.timestampQueries = true;

    auto assessment = latent::vulkan::assessProductionSupport(caps);
    check(assessment.support == latent::vulkan::ProductionSupport::Vulkan11Baseline,
          "Vulkan 1.1 required features should form the correctness baseline");

    caps.androidHardwareBufferExternalMemory = true;
    caps.fp16Storage = true;
    caps.fp16Arithmetic = true;
    assessment = latent::vulkan::assessProductionSupport(caps);
    check(assessment.support == latent::vulkan::ProductionSupport::Vulkan11WithFastPaths,
          "AHB/FP16 should upgrade acceleration without changing baseline correctness");

    caps.apiVersion = {1, 0, 0};
    assessment = latent::vulkan::assessProductionSupport(caps);
    check(assessment.support == latent::vulkan::ProductionSupport::Unsupported,
          "Vulkan 1.0 must not satisfy the production baseline");
}

}  // namespace

int main() {
    try {
        testMetadataPriorityAndNegativePreservation();
        testSceneValuesRemainUnbounded();
        testSceneScaleChangesStoredCoordinates();
        testImageTypeValidation();
        testGraphFusionStopsAtReduction();
        testVulkanCapabilityBaseline();
    } catch (const std::exception& error) {
        std::cerr << "UNCAUGHT EXCEPTION: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "all tests passed\n";
    return EXIT_SUCCESS;
}
