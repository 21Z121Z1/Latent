#include "latent/backend/Backend.h"
#include "latent/graph/ImagingIR.h"
#include "latent/imaging/ColorScience.h"
#include "latent/imaging/Half.h"
#include "latent/imaging/RawPacking.h"
#include "latent/reference/Demosaic.h"
#include "latent/reference/DngColor.h"
#include "latent/reference/NoisePropagation.h"
#include "latent/reference/RawNormalize.h"
#include "latent/reference/SensorLinearOps.h"
#include "latent/vulkan/DeviceCaps.h"
#include "latent/vulkan/IngressPlan.h"
#ifdef LATENT_ENABLE_VULKAN_RUNTIME
#include "latent/vulkan/ComputeRunner.h"
#include "latent/vulkan/DemosaicColorKernel.h"
#include "latent/vulkan/SensorPreprocessKernel.h"
#include "latent/vulkan/VulkanRuntime.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <random>
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

void checkNear(float actual, float expected, float epsilon, const std::string& message) {
    if (std::fabs(actual - expected) > epsilon) {
        ++failures;
        std::cerr << "FAIL: " << message << " expected=" << expected << " actual=" << actual << '\n';
    }
}

void checkMatrixNear(
    const latent::imaging::Matrix3f& actual,
    const latent::imaging::Matrix3f& expected,
    float epsilon,
    const std::string& message) {
    for (std::size_t i = 0; i < actual.values.size(); ++i) {
        checkNear(actual.values[i], expected.values[i], epsilon,
                  message + " [element " + std::to_string(i) + "]");
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

void testMetadataTrustBeatsSourcePriority() {
    auto raw = makeRaw4x4();
    raw.opticalBlack.validity = latent::imaging::MetadataValidity::Suspect;
    const auto normalized = latent::reference::normalizeRaw(raw);
    check(normalized.levels.blackSource == latent::imaging::MetadataSource::DynamicCaptureResult,
          "valid dynamic black must outrank suspect optical-black metadata");
}

void testReferenceDomainChangesMustBeExplicit() {
    using namespace latent;
    graph::ImagingGraph graph;

    imaging::ImageType sensor{};
    sensor.extent = {8, 8};
    sensor.layout = imaging::PixelLayout::Bayer;
    sensor.colorModel = imaging::ColorModel::SensorCfa;
    sensor.primaries = imaging::Primaries::SensorNative;
    sensor.whitePoint = imaging::WhitePoint::SensorNative;
    sensor.reference = imaging::ReferenceDomain::Sensor;
    sensor.range = imaging::RangeSemantics::SensorReferenceNormalized;

    imaging::ImageType scene{};
    scene.extent = {8, 8};

    const auto raw = graph.addInput("raw", sensor);
    const auto badScene = graph.addOperation({
        graph::OperationKind::ColorTransform, "undeclared-domain-change", {raw}, scene,
        graph::AccessPattern::Point, 0, 0, true, true, false, false});
    static_cast<void>(badScene);
    check(!graph.validate().valid,
          "sensor-to-scene transitions must explicitly declare reference-domain changes");
}

void testInvalidReconstructionConfigIsRejected() {
    const auto raw = makeRaw4x4();
    latent::reference::ReconstructionConfig config{};
    config.whiteBalanceGains[0] = 0.0F;
    bool rejected = false;
    try {
        static_cast<void>(latent::reference::reconstructSingleRaw(raw, config));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "non-positive white-balance gains must be rejected");
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

void testMatrixOperations() {
    using latent::imaging::Matrix3f;

    const Matrix3f a{{
        2.0F, 1.0F, 0.0F,
        1.0F, 3.0F, 1.0F,
        0.0F, 1.0F, 2.0F,
    }};
    const Matrix3f b{{
        1.0F, 2.0F, 3.0F,
        4.0F, 5.0F, 6.0F,
        7.0F, 8.0F, 9.0F,
    }};

    const auto product = a.multiplied(b);
    checkNear(product.values[0], 6.0F, 1.0e-6F, "matrix multiply [0][0]");
    checkNear(product.values[4], 25.0F, 1.0e-5F, "matrix multiply [1][1]");
    checkNear(product.values[8], 24.0F, 1.0e-5F, "matrix multiply [2][2]");

    const auto inverse = latent::imaging::inverted(a);
    check(inverse.has_value(), "well-conditioned matrix must invert");
    if (inverse.has_value()) {
        const auto roundTrip = a.multiplied(*inverse);
        checkMatrixNear(roundTrip, Matrix3f::identity(), 1.0e-5F,
                        "A * inv(A) must be identity");
    }

    const Matrix3f defaultMatrix;
    for (std::size_t i = 0; i < defaultMatrix.values.size(); ++i) {
        const bool diagonal = i == 0 || i == 4 || i == 8;
        checkNear(defaultMatrix.values[i], diagonal ? 1.0F : 0.0F, 0.0F,
                  "default matrix must be identity");
    }
    Matrix3f zero{};
    zero.values = {1.0F, 2.0F, 3.0F, 2.0F, 4.0F, 6.0F, 3.0F, 6.0F, 9.0F};
    check(!latent::imaging::inverted(zero).has_value(),
          "rank-deficient matrix must not invert");

    const auto transposed = b.transposed();
    checkNear(transposed.values[1], b.values[3], 0.0F, "transpose swaps off-diagonals");
    checkNear(transposed.values[3], b.values[1], 0.0F, "transpose swaps off-diagonals");
}

void testRp177PrimariesDerivationMatchesOfficialAcesMatrices() {
    using namespace latent::imaging;

    const auto npmAp0 =
        rgbPrimariesToXyzMatrix(kAp0Red, kAp0Green, kAp0Blue, kAcesWhite);
    const auto npmAp1 =
        rgbPrimariesToXyzMatrix(kAp1Red, kAp1Green, kAp1Blue, kAcesWhite);

    const auto ap0ToAp1 = inverted(npmAp1);
    check(ap0ToAp1.has_value(), "AP1 NPM must invert");
    if (ap0ToAp1.has_value()) {
        checkMatrixNear(ap0ToAp1->multiplied(npmAp0), kAp0ToAp1, 2.0e-5F,
                        "RP177-derived AP0->AP1 must match official ACES matrix");
    }

    const auto ap1ToAp0 = inverted(npmAp0);
    check(ap1ToAp0.has_value(), "AP0 NPM must invert");
    if (ap1ToAp0.has_value()) {
        checkMatrixNear(ap1ToAp0->multiplied(npmAp1), kAp1ToAp0, 2.0e-5F,
                        "RP177-derived AP1->AP0 must match official ACES matrix");
    }

    const auto whiteAsAp1 = npmAp1.apply({1.0F, 1.0F, 1.0F});
    const auto whiteChroma = xyzToChromaticity(whiteAsAp1);
    checkNear(whiteChroma.x, kAcesWhite.x, 1.0e-5F, "AP1 white x chromaticity");
    checkNear(whiteChroma.y, kAcesWhite.y, 1.0e-5F, "AP1 white y chromaticity");
}

void testBradfordAdaptationMatchesPublishedMatrix() {
    using namespace latent::imaging;

    const Matrix3f lindbloomD65ToD50{{
         1.0478112F,  0.0228866F, -0.0501270F,
         0.0295424F,  0.9904844F, -0.0170491F,
        -0.0092345F,  0.0150436F,  0.7521316F,
    }};

    const auto adaptation = bradfordAdaptation(kIlluminantD65, kIlluminantD50);
    checkMatrixNear(adaptation, lindbloomD65ToD50, 2.0e-4F,
                    "Bradford D65->D50 must match published values");

    const auto roundTrip = bradfordAdaptation(kIlluminantD50, kIlluminantD65)
                               .multiplied(adaptation);
    checkMatrixNear(roundTrip, Matrix3f::identity(), 1.0e-5F,
                    "Bradford adaptation must compose to identity");
}

void testCorrelatedTemperatureEstimators() {
    using namespace latent::imaging;

    const auto d65 = xyToCorrelatedTemperature(kIlluminantD65);
    check(std::fabs(d65.cctKelvin - 6503.0F) < 5.0F,
          "Robertson D65 CCT should be ~6503K (DNG SDK table)");
    const auto d50 = xyToCorrelatedTemperature(kIlluminantD50);
    check(std::fabs(d50.cctKelvin - 5001.8F) < 5.0F,
          "Robertson D50 CCT should be ~5002K");
    const auto illuminantA = xyToCorrelatedTemperature({0.44758F, 0.40745F});
    check(std::fabs(illuminantA.cctKelvin - 2855.6F) < 5.0F,
          "Robertson illuminant A CCT should be ~2856K");

    checkNear(xyToCctMcCamy(kIlluminantD65), 6504.3893830F, 1.0F,
              "McCamy D65 cross-check against colour-science doctest");

    bool rejected = false;
    try {
        static_cast<void>(xyToCorrelatedTemperature({0.33F, -0.5F}));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "non-physical chromaticity must be rejected");
}

latent::reference::DngCameraProfile makeCanonProfile() {
    latent::reference::DngCameraProfile profile{};
    profile.colorMatrix1 = {{
        0.5309F, -0.0229F, -0.0336F,
        -0.6241F, 1.3265F, 0.3337F,
        -0.0817F, 0.1215F, 0.6664F,
    }};
    profile.colorMatrix2 = {{
        0.4716F, 0.0603F, -0.0830F,
        -0.7798F, 1.5474F, 0.2480F,
        -0.1496F, 0.1937F, 0.6651F,
    }};
    profile.forwardMatrix1 = {{
        0.8924F, -0.1041F, 0.1760F,
        0.4351F, 0.6621F, -0.0972F,
        0.0505F, -0.1562F, 0.9308F,
    }};
    profile.forwardMatrix2 = profile.forwardMatrix1;
    profile.forwardMatricesPresent = true;
    profile.calibrationIlluminant1Cct = 2850.0F;
    profile.calibrationIlluminant2Cct = 6500.0F;
    return profile;
}

void testDngInterpolationGoldenAndClamping() {
    using namespace latent::imaging;
    using namespace latent::reference;

    const auto profile = makeCanonProfile();
    check(validateDngProfile(profile).valid, "Canon reference profile must validate");

    const Matrix3f goldenInterpolated{{
        0.4854908F, 0.0408106F, -0.0714282F,
        -0.7433278F, 1.4956549F, 0.2680749F,
        -0.1336946F, 0.1767874F, 0.6654045F,
    }};
    const auto interpolated = interpolateByCct(
        5000.0F,
        profile.calibrationIlluminant1Cct,
        profile.calibrationIlluminant2Cct,
        profile.colorMatrix1,
        profile.colorMatrix2);
    checkMatrixNear(interpolated, goldenInterpolated, 2.0e-5F,
                    "mired interpolation must match colour-hdri golden value");

    checkMatrixNear(
        interpolateByCct(2000.0F, 2850.0F, 6500.0F, profile.colorMatrix1, profile.colorMatrix2),
        profile.colorMatrix1, 0.0F, "interpolation below range must clamp to matrix1");
    checkMatrixNear(
        interpolateByCct(9000.0F, 2850.0F, 6500.0F, profile.colorMatrix1, profile.colorMatrix2),
        profile.colorMatrix2, 0.0F, "interpolation above range must clamp to matrix2");
    checkMatrixNear(
        interpolateByCct(2850.0F, 2850.0F, 6500.0F, profile.colorMatrix1, profile.colorMatrix2),
        profile.colorMatrix1, 0.0F, "interpolation at cct1 must return matrix1");
    checkMatrixNear(
        interpolateByCct(6500.0F, 2850.0F, 6500.0F, profile.colorMatrix1, profile.colorMatrix2),
        profile.colorMatrix2, 0.0F, "interpolation at cct2 must return matrix2");
}

void testCameraNeutralGoldenAndRoundTrip() {
    using namespace latent::imaging;
    using namespace latent::reference;

    const auto profile = makeCanonProfile();
    const ChromaticityXY wbXy{0.32816244F, 0.34698169F};

    const auto neutral = cameraNeutralFromXy(wbXy, profile);
    checkNear(neutral[0], 0.4130699F, 2.0e-5F, "camera neutral R golden value");
    checkNear(neutral[1], 1.0F, 1.0e-6F, "camera neutral G must normalize to 1");
    checkNear(neutral[2], 0.646465F, 2.0e-5F, "camera neutral B golden value");

    const auto recoveredXy = xyFromCameraNeutral(neutral, profile);
    checkNear(recoveredXy.x, wbXy.x, 1.0e-4F, "neutral->xy round trip x");
    checkNear(recoveredXy.y, wbXy.y, 1.0e-4F, "neutral->xy round trip y");
}

void testCameraToXyzD50ForwardMatrixGolden() {
    using namespace latent::imaging;
    using namespace latent::reference;

    const auto profile = makeCanonProfile();
    const ChromaticityXY wbXy{0.32816244F, 0.34698169F};

    const auto result = cameraToXyzD50Matrix(wbXy, profile);
    check(result.method == CameraToXyzMethod::ForwardMatrix,
          "profile with forward matrices must use the ForwardMatrix path");

    const Matrix3f golden{{
        2.1604087F, -0.1041F, 0.2722498F,
        1.0533324F, 0.6621F, -0.1503561F,
        0.1222553F, -0.1562F, 1.4398304F,
    }};
    checkMatrixNear(result.matrix, golden, 2.0e-5F,
                    "FM-path camera->XYZ D50 must match colour-hdri golden value");

    const auto neutral = cameraNeutralFromXy(wbXy, profile);
    const auto mappedXyz = result.matrix.apply(neutral);
    const auto mappedChroma = xyzToChromaticity(mappedXyz);
    checkNear(mappedChroma.x, kIlluminantD50.x, 1.0e-4F,
              "FM path must map camera neutral onto D50 chromaticity x");
    checkNear(mappedChroma.y, kIlluminantD50.y, 1.0e-4F,
              "FM path must map camera neutral onto D50 chromaticity y");
    checkNear(mappedXyz[1], 1.0F, 1.0e-4F, "FM path must preserve luminance of the neutral");
}

void testInverseColorMatrixFallbackAlsoMapsNeutralToD50() {
    using namespace latent::imaging;
    using namespace latent::reference;

    auto profile = makeCanonProfile();
    profile.forwardMatricesPresent = false;
    check(validateDngProfile(profile).valid,
          "profile without forward matrices must validate");

    const ChromaticityXY wbXy{0.400F, 0.350F};
    const auto result = cameraToXyzD50Matrix(wbXy, profile);
    check(result.method == CameraToXyzMethod::InverseColorMatrix,
          "profile without forward matrices must use the inverse-CM fallback");

    const auto neutral = cameraNeutralFromXy(wbXy, profile);
    const auto mappedXyz = result.matrix.apply(neutral);
    const auto mappedChroma = xyzToChromaticity(mappedXyz);
    checkNear(mappedChroma.x, kIlluminantD50.x, 1.0e-4F,
              "fallback path must map camera neutral onto D50 x");
    checkNear(mappedChroma.y, kIlluminantD50.y, 1.0e-4F,
              "fallback path must map camera neutral onto D50 y");
}

void testXyzD50ToAcescgWhiteMapping() {
    using namespace latent::imaging;
    using namespace latent::reference;

    const auto matrix = xyzD50ToAcescgMatrix();
    const auto white = matrix.apply(xyToXyz(kIlluminantD50));
    checkNear(white[0], 1.0F, 1.0e-5F, "D50 white must map to AP1 R=1");
    checkNear(white[1], 1.0F, 1.0e-5F, "D50 white must map to AP1 G=1");
    checkNear(white[2], 1.0F, 1.0e-5F, "D50 white must map to AP1 B=1");
}

void testDngProfileValidationRejectsBadProfiles() {
    using namespace latent::reference;

    auto badForward = makeCanonProfile();
    badForward.forwardMatrix1 = latent::imaging::Matrix3f::identity();
    badForward.forwardMatrix2 = latent::imaging::Matrix3f::identity();
    check(!validateDngProfile(badForward).valid,
          "forward matrices that do not map unit vector to D50 must be rejected");

    auto badOrder = makeCanonProfile();
    badOrder.calibrationIlluminant1Cct = 7000.0F;
    check(!validateDngProfile(badOrder).valid,
          "unordered calibration illuminants must be rejected");

    auto singular = makeCanonProfile();
    singular.colorMatrix1.values = {};
    check(!validateDngProfile(singular).valid,
          "singular color matrix must be rejected");

    auto negativeBalance = makeCanonProfile();
    negativeBalance.analogBalance = {1.0F, -1.0F, 1.0F};
    check(!validateDngProfile(negativeBalance).valid,
          "negative analog balance must be rejected");
}

constexpr std::array<float, 36> kGoldenMhcCfa{{
    5.2999997e-01F, 7.2000003e-01F, 5.0999999e-01F, 6.9000000e-01F,
    5.5000001e-01F, 6.9999999e-01F, 6.1000001e-01F, 3.4999999e-01F,
    6.6000003e-01F, 3.1000000e-01F, 6.0000002e-01F, 3.3000001e-01F,
    5.4000002e-01F, 7.0999998e-01F, 5.1999998e-01F, 6.8000001e-01F,
    5.6000000e-01F, 7.2000003e-01F, 6.2000000e-01F, 3.4000000e-01F,
    6.4999998e-01F, 3.0000001e-01F, 6.3000000e-01F, 3.4999999e-01F,
    5.0000000e-01F, 7.4000001e-01F, 4.9000001e-01F, 7.3000002e-01F,
    5.0999999e-01F, 7.0999998e-01F, 6.0000002e-01F, 3.6000001e-01F,
    6.7000002e-01F, 3.1999999e-01F, 5.8999997e-01F, 3.4000000e-01F,
}};

constexpr std::array<float, 108> kGoldenMhcRgb{{
    5.2999997e-01F, 5.9875000e-01F, 5.5437499e-01F, 6.1812502e-01F,
    7.2000003e-01F, 5.9375000e-01F, 5.0999999e-01F, 6.3625002e-01F,
    5.0437498e-01F, 5.7937503e-01F, 6.9000000e-01F, 5.5874997e-01F,
    5.5000001e-01F, 6.2000000e-01F, 4.8500001e-01F, 7.0499998e-01F,
    6.9999999e-01F, 5.8937502e-01F, 5.3937501e-01F, 6.1000001e-01F,
    4.6187499e-01F, 4.1624999e-01F, 6.0250002e-01F, 3.4999999e-01F,
    5.0812501e-01F, 6.6000003e-01F, 3.1375000e-01F, 4.5437500e-01F,
    6.0374999e-01F, 3.1000000e-01F, 4.9562499e-01F, 6.0000002e-01F,
    2.9624999e-01F, 5.6312501e-01F, 5.4124999e-01F, 3.3000001e-01F,
    5.4000002e-01F, 6.2875003e-01F, 4.9312499e-01F, 5.9500003e-01F,
    7.0999998e-01F, 3.6500001e-01F, 5.1999998e-01F, 6.7250001e-01F,
    3.2124999e-01F, 5.5750000e-01F, 6.8000001e-01F, 3.2437500e-01F,
    5.6000000e-01F, 6.4999998e-01F, 3.1125000e-01F, 7.6437497e-01F,
    7.2000003e-01F, 4.6250001e-01F, 5.2437502e-01F, 6.2000000e-01F,
    4.7312501e-01F, 4.6187499e-01F, 6.4625001e-01F, 3.4000000e-01F,
    4.6562499e-01F, 6.4999998e-01F, 2.9562500e-01F, 4.9750000e-01F,
    6.5750003e-01F, 3.0000001e-01F, 4.8750001e-01F, 6.3000000e-01F,
    3.1312501e-01F, 6.3999999e-01F, 6.1250001e-01F, 3.4999999e-01F,
    5.0000000e-01F, 5.9875000e-01F, 4.5562500e-01F, 5.5312502e-01F,
    7.4000001e-01F, 4.3812501e-01F, 4.9000001e-01F, 6.6750002e-01F,
    2.8500000e-01F, 5.1999998e-01F, 7.3000002e-01F, 4.1437501e-01F,
    5.0999999e-01F, 6.2625003e-01F, 2.6937500e-01F, 7.0125002e-01F,
    7.0999998e-01F, 5.0749999e-01F, 5.7687497e-01F, 6.0000002e-01F,
    4.9750000e-01F, 5.3125000e-01F, 5.6999999e-01F, 3.6000001e-01F,
    6.3937497e-01F, 6.7000002e-01F, 4.2375001e-01F, 5.5750000e-01F,
    5.7249999e-01F, 3.1999999e-01F, 5.6687498e-01F, 5.8999997e-01F,
    3.8624999e-01F, 5.3937501e-01F, 4.9625000e-01F, 3.4000000e-01F,
}};

latent::reference::SensorLinearFrameF32 makeGoldenSensorFrame() {
    latent::reference::SensorLinearFrameF32 frame{};
    frame.extent = {6, 6};
    frame.cfa = latent::imaging::CfaPattern::RGGB;
    frame.samples.assign(std::begin(kGoldenMhcCfa), std::end(kGoldenMhcCfa));
    return frame;
}

void testMalvarHeCutlerGoldenVectors() {
    using namespace latent::reference;

    const auto frame = makeGoldenSensorFrame();
    const auto rgb = demosaicSensorLinear(
        frame, {1.0F, 1.0F, 1.0F, 1.0F}, DemosaicMethod::MalvarHeCutler2004);

    check(rgb.rgb.size() == kGoldenMhcRgb.size(), "golden demosaic output size mismatch");
    for (std::size_t i = 0; i < kGoldenMhcRgb.size(); ++i) {
        checkNear(rgb.rgb[i], kGoldenMhcRgb[i], 1.0e-6F,
                  "MHC golden vector [index " + std::to_string(i) + "]");
    }
}

void testMalvarHeCutlerProperties() {
    using namespace latent::imaging;
    using namespace latent::reference;

    SensorLinearFrameF32 constantFrame{};
    constantFrame.extent = {8, 8};
    constantFrame.cfa = CfaPattern::RGGB;
    constantFrame.samples.assign(64U, 0.42F);

    const auto constantRgb = demosaicSensorLinear(
        constantFrame, {1.0F, 1.0F, 1.0F, 1.0F}, DemosaicMethod::MalvarHeCutler2004);
    for (const auto value : constantRgb.rgb) {
        checkNear(value, 0.42F, 1.0e-5F, "MHC must reproduce a constant field exactly");
    }

    SensorLinearFrameF32 rampFrame{};
    rampFrame.extent = {10, 10};
    rampFrame.cfa = CfaPattern::GRBG;
    rampFrame.samples.resize(100U);
    for (std::uint32_t y = 0; y < 10U; ++y) {
        for (std::uint32_t x = 0; x < 10U; ++x) {
            rampFrame.samples[static_cast<std::size_t>(y) * 10U + x] =
                0.1F + 0.8F * (static_cast<float>(x) / 9.0F);
        }
    }

    const auto rampRgb = demosaicSensorLinear(
        rampFrame, {1.0F, 1.0F, 1.0F, 1.0F}, DemosaicMethod::MalvarHeCutler2004);
    for (std::uint32_t y = 2; y < 8U; ++y) {
        for (std::uint32_t x = 2; x < 8U; ++x) {
            const auto expected = 0.1F + 0.8F * (static_cast<float>(x) / 9.0F);
            for (std::size_t c = 0; c < 3; ++c) {
                checkNear(
                    rampRgb.rgb[(static_cast<std::size_t>(y) * 10U + x) * 3U + c],
                    expected,
                    1.0e-4F,
                    "MHC must reproduce a linear ramp in the interior");
            }
        }
    }

    bool rejected = false;
    try {
        static_cast<void>(demosaicSensorLinear(
            constantFrame, {1.0F, 0.0F, 1.0F, 1.0F}, DemosaicMethod::MalvarHeCutler2004));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "non-positive white-balance gains must be rejected by demosaic");
}

void testEndToEndDngPipelineEquivalence() {
    using namespace latent::imaging;
    using namespace latent::reference;

    const auto raw = makeRaw4x4();
    const auto profile = makeCanonProfile();

    ReconstructionConfig dngConfig{};
    dngConfig.colorPath = ColorPath::DngProfile;
    dngConfig.dngProfile = profile;
    dngConfig.whiteBalanceGains = {1.15F, 1.0F, 1.0F, 0.82F};
    dngConfig.whiteBalanceXy = {0.32816244F, 0.34698169F};
    dngConfig.sceneScaleEV = 0.3F;
    dngConfig.whiteBalanceConfidence = 0.8F;

    const auto viaDng = reconstructSingleRaw(raw, dngConfig);

    ReconstructionConfig explicitConfig = dngConfig;
    explicitConfig.colorPath = ColorPath::ExplicitMatrix;
    explicitConfig.cameraToAcescg =
        xyzD50ToAcescgMatrix()
            .multiplied(cameraToXyzD50Matrix(dngConfig.whiteBalanceXy, profile).matrix);

    const auto viaExplicit = reconstructSingleRaw(raw, explicitConfig);

    check(viaDng.image.rgb.size() == viaExplicit.image.rgb.size(),
          "color paths must produce identically shaped output");
    for (std::size_t i = 0; i < viaDng.image.rgb.size(); ++i) {
        checkNear(viaDng.image.rgb[i], viaExplicit.image.rgb[i], 1.0e-5F,
                  "DNG color path must equal its composed explicit matrix [index " +
                      std::to_string(i) + "]");
    }

    bool hasNegative = false;
    bool hasAboveOne = false;
    for (const auto value : viaDng.image.rgb) {
        hasNegative = hasNegative || value < 0.0F;
        hasAboveOne = hasAboveOne || value > 1.0F;
    }
    check(hasNegative, "DNG pipeline must preserve negative scene coordinates");
    check(hasAboveOne, "DNG pipeline must preserve unbounded highlights");
    check(viaDng.sceneScaleEV == 0.3F, "scene scale metadata must survive the DNG path");
}

void testLensShadingMapValidation() {
    using namespace latent::imaging;

    LensShadingMap valid{};
    valid.gridColumns = 3;
    valid.gridRows = 3;
    valid.gains.assign(3U * 3U * 4U, 1.0F);
    check(validateLensShadingMap(valid).valid, "uniform unit-gain map must validate");

    auto emptyGrid = valid;
    emptyGrid.gridColumns = 0;
    check(!validateLensShadingMap(emptyGrid).valid, "zero-column grid must be rejected");

    auto wrongCount = valid;
    wrongCount.gains.pop_back();
    check(!validateLensShadingMap(wrongCount).valid,
          "gain count must equal gridColumns * gridRows * 4");

    auto subUnitGain = valid;
    subUnitGain.gains[5] = 0.9F;
    check(!validateLensShadingMap(subUnitGain).valid, "gains below 1.0 must be rejected");

    auto nanGain = valid;
    nanGain.gains[7] = std::nanf("");
    check(!validateLensShadingMap(nanGain).valid, "non-finite gains must be rejected");
}

latent::imaging::LensShadingMap makeGradientShadingMap(std::uint32_t columns, std::uint32_t rows) {
    latent::imaging::LensShadingMap map{};
    map.gridColumns = columns;
    map.gridRows = rows;
    map.gains.resize(static_cast<std::size_t>(columns) * rows * 4U);
    for (std::uint32_t gy = 0; gy < rows; ++gy) {
        for (std::uint32_t gx = 0; gx < columns; ++gx) {
            const float gain =
                1.0F + 0.2F * static_cast<float>(gx) / static_cast<float>(columns - 1U);
            const auto base =
                (static_cast<std::size_t>(gy) * columns + gx) * 4U;
            for (std::size_t c = 0; c < 4; ++c) {
                map.gains[base + c] = gain;
            }
        }
    }
    return map;
}

void testLensShadingApplication() {
    using namespace latent::imaging;
    using namespace latent::reference;

    SensorLinearFrameF32 frame{};
    frame.extent = {8, 8};
    frame.cfa = CfaPattern::RGGB;
    frame.samples.resize(64U);
    for (std::size_t i = 0; i < frame.samples.size(); ++i) {
        frame.samples[i] = static_cast<float>(i) * 0.01F;
    }

    LensShadingMap uniform{};
    uniform.gridColumns = 1;
    uniform.gridRows = 1;
    uniform.gains.assign(4U, 1.5F);
    const auto uniformResult = applyLensShading(frame, uniform);
    for (std::size_t i = 0; i < frame.samples.size(); ++i) {
        checkNear(uniformResult.samples[i], frame.samples[i] * 1.5F, 1.0e-6F,
                  "uniform lens shading map must scale every sample");
    }

    const auto gradientMap = makeGradientShadingMap(3, 3);
    const auto gradientResult = applyLensShading(frame, gradientMap);
    for (std::uint32_t y = 0; y < 8U; ++y) {
        for (std::uint32_t x = 0; x < 8U; ++x) {
            const auto index = static_cast<std::size_t>(y) * 8U + x;
            const auto expected =
                frame.samples[index] *
                lensShadingGainAt(gradientMap, frame.extent, CfaPattern::RGGB, x, y);
            checkNear(gradientResult.samples[index], expected, 1.0e-6F,
                      "applyLensShading must match per-pixel gain evaluation");
        }
    }

    checkNear(lensShadingGainAt(gradientMap, frame.extent, CfaPattern::RGGB, 0U, 0U),
              1.0F, 1.0e-6F, "grid corner must reproduce the exact grid gain");
    checkNear(lensShadingGainAt(gradientMap, frame.extent, CfaPattern::RGGB, 7U, 0U),
              1.2F, 1.0e-5F, "opposite grid corner must reproduce the exact grid gain");
    const auto centerGain =
        lensShadingGainAt(gradientMap, frame.extent, CfaPattern::RGGB, 4U, 4U);
    checkNear(centerGain, 1.1F + 0.1F * (4.0F * 2.0F / 7.0F - 1.0F), 1.0e-6F,
              "center gain must bilinearly interpolate between grid columns");
}

void testDefectCorrection() {
    using namespace latent::imaging;
    using namespace latent::reference;

    SensorLinearFrameF32 frame{};
    frame.extent = {8, 8};
    frame.cfa = CfaPattern::RGGB;
    frame.samples.assign(64U, 0.5F);

    const std::size_t deadIndex = 3U * 8U + 3U;
    frame.samples[deadIndex] = -0.75F;

    const auto corrected = correctDefects(frame, {{3U, 3U}});

    bool neighborsUntouched = true;
    for (std::size_t i = 0; i < frame.samples.size(); ++i) {
        if (i == deadIndex) {
            continue;
        }
        neighborsUntouched =
            neighborsUntouched && corrected.samples[i] == frame.samples[i];
    }
    check(neighborsUntouched, "defect correction must not modify non-defect samples");

    const auto channel = cfaChannelAt(CfaPattern::RGGB, 3U, 3U);
    check(channel == CfaChannel::B, "expected B site at (3,3) in RGGB");
    checkNear(corrected.samples[deadIndex], 0.5F, 1.0e-6F,
              "dead pixel must be replaced by the same-channel neighbor median");

    SensorLinearFrameF32 negativeFrame{};
    negativeFrame.extent = {8, 8};
    negativeFrame.cfa = CfaPattern::BGGR;
    negativeFrame.samples.assign(64U, -0.25F);
    negativeFrame.samples[4U * 8U + 4U] = 2.0F;
    const auto negativeCorrected = correctDefects(negativeFrame, {{4U, 4U}});
    checkNear(negativeCorrected.samples[4U * 8U + 4U], -0.25F, 1.0e-6F,
              "correction must preserve negative scene values");

    bool rejected = false;
    try {
        static_cast<void>(correctDefects(frame, {{9U, 3U}}));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "out-of-bounds defect coordinates must be rejected");

    SensorLinearFrameF32 tiny{};
    tiny.extent = {2, 2};
    tiny.cfa = CfaPattern::RGGB;
    tiny.samples.assign(4U, 0.5F);
    rejected = false;
    try {
        static_cast<void>(correctDefects(tiny, {{0U, 0U}}));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "defect without same-channel neighbors must be rejected");
}

void testDefectDetection() {
    using namespace latent::imaging;
    using namespace latent::reference;

    SensorLinearFrameF32 frame{};
    frame.extent = {12, 12};
    frame.cfa = CfaPattern::GRBG;
    frame.samples.resize(12U * 12U);
    for (std::uint32_t y = 0; y < 12U; ++y) {
        for (std::uint32_t x = 0; x < 12U; ++x) {
            frame.samples[static_cast<std::size_t>(y) * 12U + x] =
                0.3F + 0.02F * static_cast<float>(x);
        }
    }

    frame.samples[2U * 12U + 5U] = 2.5F;
    frame.samples[8U * 12U + 9U] = -1.0F;

    DefectDetectionConfig config{};
    config.enabled = true;
    config.sigmaMultiplier = 8.0F;
    config.minAbsoluteDelta = 0.05F;

    const auto candidates = detectDefectCandidates(frame, config, nullptr);
    check(candidates.size() == 2U, "exactly the two injected defects must be detected");
    if (candidates.size() == 2U) {
        check(candidates[0].x == 5U && candidates[0].y == 2U,
              "hot pixel must be detected in row-major order");
        check(candidates[1].x == 9U && candidates[1].y == 8U,
              "dead pixel must be detected in row-major order");
    }

    const auto roundTrip = correctDefects(frame, candidates);
    checkNear(roundTrip.samples[2U * 12U + 5U], 0.40F, 1.0e-5F,
              "corrected hot pixel must equal the same-channel neighbor median");
    check(detectDefectCandidates(roundTrip, config, nullptr).empty(),
          "corrected frame must contain no remaining defect candidates");

    DefectDetectionConfig disabled = config;
    disabled.enabled = false;
    check(detectDefectCandidates(frame, disabled, nullptr).empty(),
          "disabled detection must return no candidates");

    NoiseModel noise{};
    for (std::size_t c = 0; c < 4; ++c) {
        noise.read[c] = 1.0e6F;
    }
    check(detectDefectCandidates(frame, config, &noise).empty(),
          "huge read noise must suppress detection via the noise-aware threshold");
}

void testNoiseModelValidationAndTransform() {
    using namespace latent::imaging;
    using namespace latent::reference;

    NoiseModel valid{};
    valid.shot = {0.5F, 0.5F, 0.5F, 0.5F};
    valid.read = {4.0F, 4.0F, 4.0F, 4.0F};
    check(validateNoiseModel(valid).valid, "positive noise model must validate");

    auto negative = valid;
    negative.shot[2] = -0.1F;
    check(!validateNoiseModel(negative).valid, "negative shot coefficient must be rejected");

    auto nonFinite = valid;
    nonFinite.read[1] = std::nanf("");
    check(!validateNoiseModel(nonFinite).valid, "non-finite coefficients must be rejected");

    SelectedRawLevels levels{};
    levels.black = BlackLevel{{100.0F, 100.0F, 100.0F, 100.0F}};
    levels.white = 1100.0F;

    const auto normalized = normalizeNoiseModel(valid, levels);
    for (std::size_t c = 0; c < 4; ++c) {
        checkNear(normalized.shot[c], 0.0005F, 1.0e-9F,
                  "normalized shot gain must equal S / (W - B)");
        checkNear(normalized.read[c], 5.4e-5F, 1.0e-10F,
                  "normalized read noise must equal (S*B + O) / (W - B)^2");
    }
}

void testTapWeightsMirrorDemosaicOutput() {
    using namespace latent::imaging;
    using namespace latent::reference;

    SensorLinearFrameF32 frame{};
    frame.extent = {12, 16};
    frame.cfa = CfaPattern::GBRG;
    std::mt19937 rng(1234U);
    std::uniform_real_distribution<float> signal(0.05F, 0.95F);
    for (auto& sample : frame.samples) {
        sample = signal(rng);
    }
    frame.samples.resize(static_cast<std::size_t>(frame.extent.pixelCount()));
    while (frame.samples.size() <
           static_cast<std::size_t>(frame.extent.pixelCount())) {
        frame.samples.push_back(signal(rng));
    }

    const std::array<float, 4> gains{1.2F, 1.0F, 1.05F, 0.95F};

    for (const auto method : {DemosaicMethod::BaselineBoxAverage,
                              DemosaicMethod::MalvarHeCutler2004}) {
        const auto rgb = demosaicSensorLinear(frame, gains, method);
        for (std::uint32_t y = 0; y < frame.extent.height; ++y) {
            for (std::uint32_t x = 0; x < frame.extent.width; ++x) {
                for (std::size_t c = 0; c < 3; ++c) {
                    const auto taps = demosaicTapWeights(
                        frame.extent, frame.cfa, x, y, c, gains, method);
                    float reconstructed = 0.0F;
                    for (const auto& tap : taps) {
                        reconstructed +=
                            tap.weight *
                            frame.samples[static_cast<std::size_t>(tap.y) *
                                              frame.extent.width +
                                          tap.x];
                    }
                    const auto actual =
                        rgb.rgb[(static_cast<std::size_t>(y) * frame.extent.width + x) * 3U + c];
                    checkNear(reconstructed, actual, 2.0e-5F * std::max(1.0F, std::fabs(actual)),
                              "tap weights must mirror demosaic output");
                }
            }
        }
    }
}

void testPropagatedSigmaAnalyticCases() {
    using namespace latent::imaging;
    using namespace latent::reference;

    // RGGB pixel (3,3) is a B site; its known channel is B (rgb index 2).
    PropagatedNoise noise{};
    noise.valid = true;
    noise.extent = {8, 8};
    noise.cfa = CfaPattern::RGGB;
    noise.demosaicMethod = DemosaicMethod::BaselineBoxAverage;
    for (std::size_t c = 0; c < 4; ++c) {
        noise.shot[c] = 0.0F;
        noise.read[c] = 1.0F;
    }

    SigmaQuery query{};
    query.x = 3U;
    query.y = 3U;
    query.rgbChannel = 1U;
    query.sceneValueRgb = {0.5F, 0.5F, 0.5F};

    checkNear(propagatedSigma(noise, query), 0.5F, 1.0e-5F,
              "read-noise-only baseline G interpolation must give sqrt(1/4)");

    query.rgbChannel = 2U;
    checkNear(propagatedSigma(noise, query), 1.0F, 1.0e-5F,
              "known-channel identity tap must carry full variance");

    noise.demosaicMethod = DemosaicMethod::MalvarHeCutler2004;
    checkNear(propagatedSigma(noise, query), 1.0F, 1.0e-5F,
              "MHC known-channel identity tap must carry full variance");

    // MHC green interpolation at a B site taps the B center (4/8), four G
    // axial neighbors (2/8) and four B far-axial samples (-1/8). With WB
    // gains {1,2,2,1} the per-tap variance weights differ by channel:
    //   Var = (4/8)^2*1^2 + 4*(2/8)^2*2^2 + 4*(1/8)^2*1^2 = 21/16
    noise.whiteBalanceGains = {1.0F, 2.0F, 2.0F, 1.0F};
    query.rgbChannel = 1U;
    checkNear(propagatedSigma(noise, query), std::sqrt(21.0F / 16.0F), 1.0e-5F,
              "MHC green sigma must weight each tap by its own channel gain");

    // Baseline G interpolation with the same gains: four G taps of weight
    // 1/4, each amplified by its channel gain squared.
    noise.demosaicMethod = DemosaicMethod::BaselineBoxAverage;
    checkNear(propagatedSigma(noise, query), 1.0F, 1.0e-5F,
              "baseline G sigma must scale linearly with the WB gain");

    noise.whiteBalanceGains = {1.0F, 1.0F, 1.0F, 1.0F};
    const auto withoutLsc = propagatedSigma(noise, query);
    noise.lensShadingApplied = true;
    noise.lensShading = makeGradientShadingMap(3, 3);
    const auto withLsc = propagatedSigma(noise, query);
    check(withLsc > withoutLsc,
          "lens shading gains must increase propagated sigma");
}

latent::imaging::RawFrame makeNoisyMonteCarloRaw() {
    using namespace latent::imaging;

    RawFrame raw{};
    raw.id = 7U;
    raw.storage.extent = {32, 32};
    raw.storage.rowStridePixels = 32U;
    raw.storage.pixels.resize(32U * 32U);
    raw.cfa = CfaPattern::RGGB;
    raw.exposureTimeNs = 10'000'000;
    raw.sensitivityIso = 100.0F;
    raw.staticBlack = MetadataValue<BlackLevel>{
        BlackLevel{{64.0F, 64.0F, 64.0F, 64.0F}},
        MetadataSource::StaticCharacteristic, MetadataValidity::Valid, 0.9F};
    raw.staticWhite = MetadataValue<float>{
        1024.0F, MetadataSource::StaticCharacteristic, MetadataValidity::Valid, 0.9F};

    NoiseModel profile{};
    for (std::size_t c = 0; c < 4; ++c) {
        profile.shot[c] = 0.9F;
        profile.read[c] = 16.0F;
    }
    raw.noiseProfile = MetadataValue<NoiseModel>{
        profile, MetadataSource::MeasuredCalibration, MetadataValidity::Valid, 1.0F};

    raw.lensShading = MetadataValue<LensShadingMap>{
        makeGradientShadingMap(3, 3),
        MetadataSource::DynamicCaptureResult, MetadataValidity::Valid, 0.9F};

    return raw;
}

void fillMonteCarloSignal(latent::imaging::RawFrame& raw) {
    for (std::uint32_t y = 0; y < 32U; ++y) {
        for (std::uint32_t x = 0; x < 32U; ++x) {
            const float signalCode =
                200.0F + 500.0F * (static_cast<float>(x + y) / 62.0F);
            raw.storage.pixels[static_cast<std::size_t>(y) * 32U + x] =
                static_cast<std::uint16_t>(signalCode);
        }
    }
}

void testMonteCarloNoisePropagation() {
    using namespace latent::imaging;
    using namespace latent::reference;

    constexpr int kRealizations = 256;
    constexpr std::uint32_t kSize = 32U;

    const auto noiselessRaw = makeNoisyMonteCarloRaw();
    fillMonteCarloSignal(const_cast<latent::imaging::RawFrame&>(noiselessRaw));

    ReconstructionConfig config{};
    config.defectCorrection = DefectCorrectionMode::Disabled;
    config.applyLensShading = true;
    config.whiteBalanceGains = {1.1F, 1.0F, 1.0F, 0.9F};
    config.cameraToAcescg.values = {
        1.0F, 0.02F, -0.02F,
        -0.01F, 1.0F, 0.01F,
        0.01F, -0.01F, 1.0F,
    };
    config.sceneScaleEV = 0.5F;

    const auto noiseless = reconstructSingleRaw(noiselessRaw, config);
    check(noiseless.propagatedNoise.valid,
          "noise metadata must produce a valid propagated-noise record");

    auto noisyRaw = makeNoisyMonteCarloRaw();
    const auto pristinePixels = noiselessRaw.storage.pixels;
    const auto& profile = *noisyRaw.noiseProfile.value;

    std::mt19937 rng(20260821U);
    std::vector<std::vector<float>> samples(
        static_cast<std::size_t>(noiseless.image.rgb.size()));
    std::normal_distribution<float> gaussian(0.0F, 1.0F);

    for (int realization = 0; realization < kRealizations; ++realization) {
        noisyRaw.storage.pixels = pristinePixels;
        for (std::uint32_t y = 0; y < kSize; ++y) {
            for (std::uint32_t x = 0; x < kSize; ++x) {
                const auto index = static_cast<std::size_t>(y) * kSize + x;
                const float code = static_cast<float>(pristinePixels[index]);
                const auto channel = static_cast<std::size_t>(
                    cfaChannelAt(noisyRaw.cfa, x, y));
                const float sigmaCode =
                    std::sqrt(profile.shot[channel] * code + profile.read[channel]);
                float noisy = code + gaussian(rng) * sigmaCode;
                noisy = std::clamp(noisy, 0.0F, 65535.0F);
                noisyRaw.storage.pixels[index] =
                    static_cast<std::uint16_t>(noisy + 0.5F);
            }
        }

        const auto scene = reconstructSingleRaw(noisyRaw, config);
        if (realization == 0) {
            samples.assign(scene.image.rgb.size(), {});
        }
        for (std::size_t i = 0; i < scene.image.rgb.size(); ++i) {
            samples[i].push_back(scene.image.rgb[i]);
        }
    }

    std::vector<double> relativeErrors;
    std::vector<double> biasErrors;

    for (std::uint32_t y = 2U; y < kSize - 2U; ++y) {
        for (std::uint32_t x = 2U; x < kSize - 2U; ++x) {
            for (std::size_t c = 0; c < 3; ++c) {
                const auto index =
                    (static_cast<std::size_t>(y) * kSize + x) * 3U + c;

                double mean = 0.0;
                for (const auto value : samples[index]) {
                    mean += static_cast<double>(value);
                }
                mean /= static_cast<double>(kRealizations);

                double variance = 0.0;
                for (const auto value : samples[index]) {
                    const double d = static_cast<double>(value) - mean;
                    variance += d * d;
                }
                variance /= static_cast<double>(kRealizations - 1);
                const double empiricalSigma = std::sqrt(variance);

                SigmaQuery query{};
                query.x = x;
                query.y = y;
                query.rgbChannel = c;
                query.sceneValueRgb = {noiseless.image.rgb[index - c],
                                       noiseless.image.rgb[index - c + 1U],
                                       noiseless.image.rgb[index - c + 2U]};
                const double predictedSigma =
                    propagatedSigma(noiseless.propagatedNoise, query);

                if (!(predictedSigma > 1.0e-9)) {
                    continue;
                }
                relativeErrors.push_back(
                    std::fabs(empiricalSigma - predictedSigma) / predictedSigma);

                biasErrors.push_back(
                    std::fabs(mean - static_cast<double>(noiseless.image.rgb[index])) /
                    predictedSigma);
            }
        }
    }

    check(relativeErrors.size() > 1000U,
          "monte carlo must evaluate a meaningful pixel count");

    std::sort(relativeErrors.begin(), relativeErrors.end());
    std::sort(biasErrors.begin(), biasErrors.end());
    const double medianRelative = relativeErrors[relativeErrors.size() / 2U];
    const double p95Relative = relativeErrors[(relativeErrors.size() * 95U) / 100U];
    const double medianBias = biasErrors[biasErrors.size() / 2U];

    check(medianRelative <= 0.12,
          "median sigma prediction error must stay within 12% (got " +
              std::to_string(medianRelative) + ")");
    check(p95Relative <= 0.35,
          "p95 sigma prediction error must stay within 35% (got " +
              std::to_string(p95Relative) + ")");
    check(medianBias <= 0.35,
          "reconstruction must remain unbiased within sampling noise (median |mean-true|/sigma = " +
              std::to_string(medianBias) + ")");
}

void testPropagatedNoiseDefaultsAndGating() {
    using namespace latent::reference;

    const auto raw = makeRaw4x4();
    ReconstructionConfig config{};
    const auto withoutMetadata = reconstructSingleRaw(raw, config);
    check(!withoutMetadata.propagatedNoise.valid,
          "missing noise metadata must leave propagated noise invalid");

    auto withNoise = makeRaw4x4();
    latent::imaging::NoiseModel profile{};
    for (std::size_t c = 0; c < 4; ++c) {
        profile.read[c] = 4.0F;
    }
    withNoise.noiseProfile = latent::imaging::MetadataValue<latent::imaging::NoiseModel>{
        profile, latent::imaging::MetadataSource::MeasuredCalibration,
        latent::imaging::MetadataValidity::Valid, 1.0F};

    const auto propagated = reconstructSingleRaw(withNoise, config);
    check(propagated.propagatedNoise.valid,
          "usable noise metadata must produce a valid propagated record");
    check(propagated.propagatedNoise.extent.width == 4U,
          "propagated extent must match the reconstruction extent");

    ReconstructionConfig gated = config;
    gated.propagateNoise = false;
    check(!reconstructSingleRaw(withNoise, gated).propagatedNoise.valid,
          "propagation must be disableable by configuration");
}

void packRaw10Row(const std::vector<std::uint16_t>& pixels, std::vector<std::uint8_t>& out) {
    std::size_t i = 0U;
    while (i + 4U <= pixels.size()) {
        const auto p0 = pixels[i];
        const auto p1 = pixels[i + 1U];
        const auto p2 = pixels[i + 2U];
        const auto p3 = pixels[i + 3U];
        out.push_back(static_cast<std::uint8_t>(p0 >> 2U));
        out.push_back(static_cast<std::uint8_t>(p1 >> 2U));
        out.push_back(static_cast<std::uint8_t>(p2 >> 2U));
        out.push_back(static_cast<std::uint8_t>(p3 >> 2U));
        out.push_back(static_cast<std::uint8_t>((p0 & 0x3U) |
                                                ((p1 & 0x3U) << 2U) |
                                                ((p2 & 0x3U) << 4U) |
                                                ((p3 & 0x3U) << 6U)));
        i += 4U;
    }
    const std::size_t remaining = pixels.size() - i;
    for (std::size_t k = 0; k < remaining; ++k) {
        out.push_back(static_cast<std::uint8_t>(pixels[i + k] >> 2U));
    }
    if (remaining > 0U) {
        std::uint32_t lsb = 0U;
        for (std::size_t k = 0; k < remaining; ++k) {
            lsb |= (pixels[i + k] & 0x3U) << (2U * k);
        }
        out.push_back(static_cast<std::uint8_t>(lsb));
    }
}

void packRaw12Row(const std::vector<std::uint16_t>& pixels, std::vector<std::uint8_t>& out) {
    std::size_t i = 0U;
    while (i + 2U <= pixels.size()) {
        const auto p0 = pixels[i];
        const auto p1 = pixels[i + 1U];
        out.push_back(static_cast<std::uint8_t>(p0 >> 4U));
        out.push_back(static_cast<std::uint8_t>(p1 >> 4U));
        out.push_back(static_cast<std::uint8_t>((p0 & 0xFU) | ((p1 & 0xFU) << 4U)));
        i += 2U;
    }
    if (pixels.size() - i == 1U) {
        out.push_back(static_cast<std::uint8_t>(pixels[i] >> 4U));
        out.push_back(static_cast<std::uint8_t>(pixels[i] & 0xFU));
    }
}

void testRawPackingGoldenVectors() {
    using namespace latent::imaging;

    // RAW10 group formula pinned byte-for-byte: pixels {0x123, 0x256, 0x38A,
    // 0x3FF} -> bytes {0x48, 0x95, 0xE2, 0xFF, 0xEB}.
    {
        std::vector<std::uint8_t> packed;
        packRaw10Row({0x123U, 0x256U, 0x38AU, 0x3FFU}, packed);
        const std::array<std::uint8_t, 5> expected{0x48U, 0x95U, 0xE2U, 0xFFU, 0xEBU};
        check(packed == std::vector<std::uint8_t>(expected.begin(), expected.end()),
              "RAW10 packing must match the MIPI/Android group formula");
    }

    // RAW12 pair formula: pixels {0xABC, 0x147} -> bytes {0xAB, 0x14, 0x7C}.
    {
        std::vector<std::uint8_t> packed;
        packRaw12Row({0xABCU, 0x147U}, packed);
        const std::array<std::uint8_t, 3> expected{0xABU, 0x14U, 0x7CU};
        check(packed == std::vector<std::uint8_t>(expected.begin(), expected.end()),
              "RAW12 packing must match the MIPI/Android pair formula");
    }

    PackedRawLayout layout{};
    layout.extent = {4, 1};
    layout.packing = RawPacking::Raw10;
    layout.rowStrideBytes = 5U;
    const std::array<std::uint8_t, 5> bytes{0x48U, 0x95U, 0xE2U, 0xFFU, 0xEBU};
    const auto samples = unpackPackedRaw(
        layout, bytes.data(), static_cast<std::uint64_t>(bytes.size()));
    check(samples.size() == 4U, "unpacked sample count must equal the extent");
    checkNear(static_cast<float>(samples[0]), static_cast<float>(0x123U), 0.0F,
              "RAW10 pixel 0 golden value");
    checkNear(static_cast<float>(samples[1]), static_cast<float>(0x256U), 0.0F,
              "RAW10 pixel 1 golden value");
    checkNear(static_cast<float>(samples[2]), static_cast<float>(0x38AU), 0.0F,
              "RAW10 pixel 2 golden value");
    checkNear(static_cast<float>(samples[3]), static_cast<float>(0x3FFU), 0.0F,
              "RAW10 pixel 3 golden value");

    PackedRawLayout raw16{};
    raw16.extent = {3, 1};
    raw16.packing = RawPacking::Raw16;
    raw16.rowStrideBytes = 6U;
    const std::array<std::uint8_t, 6> leBytes{0x34U, 0x12U, 0x78U, 0x56U, 0xCDU, 0xABU};
    const auto raw16Samples = unpackPackedRaw(
        raw16, leBytes.data(), static_cast<std::uint64_t>(leBytes.size()));
    check(raw16Samples[0] == 0x1234U && raw16Samples[1] == 0x5678U &&
              raw16Samples[2] == 0xABCDU,
          "RAW16 must decode little-endian samples");
}

void testRawPackingRoundTrip() {
    using namespace latent::imaging;

    std::mt19937 rng(777U);

    for (const auto packing : {RawPacking::Raw10, RawPacking::Raw12}) {
        const std::uint16_t maxValue =
            packing == RawPacking::Raw10 ? 1023U : 4095U;
        std::uniform_int_distribution<std::uint32_t> sampleRange(0U, maxValue);

        for (std::uint32_t width = 1U; width <= 9U; ++width) {
            std::vector<std::uint16_t> pixels(width);
            for (auto& pixel : pixels) {
                pixel = static_cast<std::uint16_t>(sampleRange(rng));
            }

            std::vector<std::uint8_t> packed;
            if (packing == RawPacking::Raw10) {
                packRaw10Row(pixels, packed);
            } else {
                packRaw12Row(pixels, packed);
            }
            const auto minStride = minRowStrideBytes({width, 1U}, packing);
            check(packed.size() == minStride,
                  "packed row size must equal ceil(width * bpp / 8)");

            PackedRawLayout layout{};
            layout.extent = {width, 2U};
            layout.packing = packing;
            layout.rowStrideBytes = minStride + 3U;

            std::vector<std::uint8_t> buffer(
                static_cast<std::size_t>(layout.rowStrideBytes * 2U), 0xA5U);
            std::copy(packed.begin(), packed.end(), buffer.begin());
            std::copy(packed.begin(), packed.end(),
                      buffer.begin() + layout.rowStrideBytes);

            const auto unpacked =
                unpackPackedRaw(layout, buffer.data(),
                                static_cast<std::uint64_t>(buffer.size()));

            bool rowsMatch = true;
            for (std::uint32_t row = 0; row < 2U; ++row) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    rowsMatch = rowsMatch &&
                                unpacked[static_cast<std::size_t>(row) * width + x] ==
                                    pixels[x];
                }
            }
            check(rowsMatch,
                  "pack/unpack round trip must be identity across partial groups");
        }
    }
}

void testRawPackingValidation() {
    using namespace latent::imaging;

    check(minRowStrideBytes({4U, 1U}, RawPacking::Raw10) == 5U,
          "RAW10 stride minimum for four pixels is five bytes");
    check(minRowStrideBytes({5U, 1U}, RawPacking::Raw10) == 7U,
          "RAW10 trailing partial group still occupies ceil(width*10/8) bytes");
    check(minRowStrideBytes({3U, 1U}, RawPacking::Raw12) == 5U,
          "RAW12 odd widths round up to a full nibble byte");

    PackedRawLayout ok{};
    ok.extent = {4U, 2U};
    ok.packing = RawPacking::Raw10;
    ok.rowStrideBytes = 8U;
    std::vector<std::uint8_t> buffer(16U, 0U);
    check(validatePackedRawLayout(ok, buffer.data(),
                                  static_cast<std::uint64_t>(buffer.size()))
              .valid,
          "valid layout with row padding must validate");

    PackedRawLayout smallStride = ok;
    smallStride.rowStrideBytes = 4U;
    check(!validatePackedRawLayout(smallStride, buffer.data(),
                                   static_cast<std::uint64_t>(buffer.size()))
               .valid,
          "row stride below the packed row width must be rejected");

    // Required minimum is rowStride * (height - 1) + packedRowWidth = 13.
    check(validatePackedRawLayout(ok, buffer.data(), 13U).valid,
          "buffer exactly matching the layout requirement must validate");
    check(!validatePackedRawLayout(ok, buffer.data(), 12U).valid,
          "buffer smaller than the layout requires must be rejected");

    PackedRawLayout emptyExtent = ok;
    emptyExtent.extent = {0U, 2U};
    check(!validatePackedRawLayout(emptyExtent, nullptr, 0U).valid,
          "zero extents must be rejected");

    bool rejected = false;
    try {
        PackedRawLayout bad = ok;
        bad.rowStrideBytes = 2U;
        static_cast<void>(
            unpackPackedRaw(bad, buffer.data(),
                            static_cast<std::uint64_t>(buffer.size())));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "unpack must reject layouts that fail validation");
}

void testIngressDecisionTable() {
    using namespace latent::imaging;
    using namespace latent::vulkan;

    DeviceCaps caps{};
    caps.apiVersion = {1U, 1U, 0U};
    caps.computeQueue = true;
    caps.storageBuffer = true;
    caps.commonStorageImages = true;
    caps.timestampQueries = true;

    IngressRequest request{};
    request.buffer.format = kAhbFormatRaw10;
    request.packing = RawPacking::Raw10;

    auto decision = planIngress(caps, request);
    check(decision.path == IngressPath::PortableCopy,
          "without the AHB extension ingress must use the portable copy");
    check(decision.sharing == SharingGuarantee::None,
          "portable copy cannot claim any sharing guarantee");

    caps.androidHardwareBufferExternalMemory = true;
    decision = planIngress(caps, request);
    check(decision.path == IngressPath::PortableCopy,
          "camera RAW must default to the portable unpack baseline");
    check(decision.requiresRuntimeFormatProbe,
          "camera RAW decisions should recommend an advisory runtime probe");
    check(!decision.externalFormatExpected,
          "Bayer RAW must not be routed through the external-format sampler path");

    request.buffer.format = kAhbFormatRawPrivate;
    decision = planIngress(caps, request);
    check(decision.path == IngressPath::Unsupported,
          "RAW_PRIVATE must stay unsupported without a device profile");

    request.buffer.format = kAhbFormatYuv420_888;
    decision = planIngress(caps, request);
    check(decision.path == IngressPath::DirectImportCandidate &&
              decision.externalFormatExpected && decision.requiresRuntimeFormatProbe,
          "YUV must be a direct-import candidate through the YCbCr sampler path");

    request.buffer.format = kAhbFormatBlob;
    request.buffer.usage = kAhbUsageGpuDataBuffer;
    decision = planIngress(caps, request);
    check(decision.path == IngressPath::DirectImportCandidate &&
              !decision.requiresRuntimeFormatProbe,
          "BLOB with GPU_DATA_BUFFER imports as VkBuffer memory");

    request.buffer.usage = 0U;
    decision = planIngress(caps, request);
    check(decision.path == IngressPath::Unsupported,
          "BLOB without GPU_DATA_BUFFER usage must not import");

    DeviceCaps legacy{};
    legacy.apiVersion = {1U, 0U, 0U};
    decision = planIngress(legacy, request);
    check(decision.path == IngressPath::Unsupported,
          "devices below the production baseline must reject ingress entirely");
}

#ifdef LATENT_ENABLE_VULKAN_RUNTIME
void testVulkanDemosaicColorDifferential() {
    using namespace latent::imaging;
    using namespace latent::reference;
    using namespace latent::vulkan;

    std::string detail;
    auto runner = ComputeRunner::tryCreate(&detail);
    if (runner == nullptr) {
        std::cout << "compute runner unavailable (" << detail
                  << "); demosaic differential test skipped\n";
        return;
    }

    SensorPreprocessKernel preprocess(*runner);
    DemosaicColorKernel demosaic(*runner);

    std::mt19937 rng(20260823U);
    std::uniform_int_distribution<std::uint32_t> codeDist(0U, 1023U);

    struct Case {
        CfaPattern cfa;
        Extent extent;
        DemosaicMethod method;
        bool useLsc;
    };
    const std::array<Case, 6> cases{{
        {CfaPattern::RGGB, {64U, 48U}, DemosaicMethod::MalvarHeCutler2004, true},
        {CfaPattern::GRBG, {61U, 41U}, DemosaicMethod::BaselineBoxAverage, true},
        {CfaPattern::GBRG, {33U, 17U}, DemosaicMethod::MalvarHeCutler2004, false},
        {CfaPattern::BGGR, {40U, 40U}, DemosaicMethod::BaselineBoxAverage, false},
        {CfaPattern::RGGB, {256U, 2U}, DemosaicMethod::MalvarHeCutler2004, true},
        {CfaPattern::GRBG, {17U, 64U}, DemosaicMethod::MalvarHeCutler2004, true},
    }};

    const std::array<float, 4> blackLevels{80.0F, 84.0F, 84.0F, 88.0F};
    const std::array<float, 4> wbGains{1.6F, 1.0F, 1.05F, 1.35F};
    const std::array<float, 9> cameraToScene{
        1.0F, -0.05F, 0.02F,
        -0.01F, 1.0F, 0.03F,
        0.01F, -0.02F, 1.0F,
    };
    const float sceneScale = 1.5F;

#ifdef LATENT_ENABLE_VULKAN_RUNTIME
    if (std::getenv("LATENT_DEMOSAIC_DEBUG") != nullptr) {
        const auto& caps = latent::vulkan::VulkanRuntime::deviceCaps();
        std::cout << "demosaic debug device api=" << caps.apiVersion.major
                  << '.' << caps.apiVersion.minor << '.'
                  << caps.apiVersion.patch << '\n';
    }
#endif

    for (const auto& testCase : cases) {
        PreprocessParams preParams{};
        preParams.extent = testCase.extent;
        preParams.cfa = testCase.cfa;
        preParams.whiteLevel = 1000.0F;
        preParams.black = blackLevels;
        preParams.wbGains = wbGains;
        if (testCase.useLsc) {
            preParams.lensShading = makeGradientShadingMap(3, 3);
        }

        std::vector<std::uint16_t> canonical(
            static_cast<std::size_t>(preParams.extent.pixelCount()));
        for (auto& code : canonical) {
            code = static_cast<std::uint16_t>(codeDist(rng));
        }

        const auto bayerBits = preprocess.run(preParams, canonical);

        DemosaicColorParams params{};
        params.extent = testCase.extent;
        params.cfa = testCase.cfa;
        params.demosaicMethod = testCase.method;
        params.cameraToScene = cameraToScene;
        params.sceneScale = sceneScale;

        const auto rgbaBits = demosaic.run(params, bayerBits);
        check(rgbaBits.size() ==
                  static_cast<std::size_t>(params.extent.pixelCount()) * 4U,
              "demosaic kernel output must be RGBA16F shaped");

        // CPU reference: consume the SAME FP16 Bayer the kernel consumed.
        SensorLinearFrameF32 gained{};
        gained.extent = testCase.extent;
        gained.cfa = testCase.cfa;
        gained.samples.resize(bayerBits.size());
        for (std::size_t i = 0; i < bayerBits.size(); ++i) {
            gained.samples[i] = halfBitsToFloat(bayerBits[i]);
        }
        const auto cpuRgb = demosaicSensorLinear(
            gained, {1.0F, 1.0F, 1.0F, 1.0F}, testCase.method);

        // Per the article's error-budget doctrine, relative error is only
        // meaningful away from cancellation; near-zero coordinates (matrix
        // rows cancelling across similar camera channels) are gated with an
        // absolute tolerance scaled by the pixel's dynamic range instead.
        std::vector<double> conditionedRelativeErrors;
        std::size_t nanCount = 0U;
        std::size_t outOfTolerance = 0U;
        std::size_t signMismatch = 0U;
        double worstAbsolute = 0.0;

        const auto cameraAt = [&cpuRgb](std::uint32_t p, std::uint32_t c) {
            return cpuRgb.rgb[static_cast<std::size_t>(p) * 3U + c];
        };

        for (std::uint32_t p = 0; p < params.extent.pixelCount(); ++p) {
            float sceneExpectedRgb[3];
            float pixelMagnitude = 0.0F;
            for (std::uint32_t c = 0; c < 3U; ++c) {
                float value = 0.0F;
                for (std::uint32_t j = 0; j < 3U; ++j) {
                    value += cameraToScene[static_cast<std::size_t>(c) * 3U +
                                           j] *
                             cameraAt(p, j);
                }
                sceneExpectedRgb[c] = value * sceneScale;
                pixelMagnitude =
                    std::max(pixelMagnitude, std::fabs(value));
            }

            for (std::uint32_t c = 0; c < 3U; ++c) {
                const float sceneExpected = sceneExpectedRgb[c];

                const auto actualBits =
                    rgbaBits[static_cast<std::size_t>(p) * 4U + c];
                if (((actualBits & 0x7C00U) == 0x7C00U) &&
                    ((actualBits & 0x03FFU) != 0U)) {
                    ++nanCount;
                    continue;
                }

                const float actualValue = halfBitsToFloat(actualBits);
                const float absoluteError =
                    std::fabs(actualValue - sceneExpected);

                // Two chained FP16 storages plus FP32 accumulation:
                // a few ulps of the pixel scale bound every legitimate
                // difference.
                const float absoluteTolerance =
                    std::max(pixelMagnitude * 4.0e-3F, 1.0e-4F);
                if (absoluteError > absoluteTolerance) {
                    ++outOfTolerance;
                    worstAbsolute = std::max(
                        worstAbsolute,
                        static_cast<double>(absoluteError));
                    if (std::getenv("LATENT_DEMOSAIC_DEBUG") != nullptr &&
                        outOfTolerance <= 5U) {
                        const auto x = p % params.extent.width;
                        const auto y = p / params.extent.width;
                        std::cout << "demosaic mismatch case="
                                  << static_cast<int>(testCase.cfa)
                                  << " method=" << params.demosaicMethod
                                  << " x=" << x << " y=" << y << " c=" << c
                                  << " expected=" << sceneExpected
                                  << " actual=" << actualValue
                                  << " bits=0x" << std::hex << actualBits
                                  << std::dec << "\n";
                    }
                }

                if (std::fabs(sceneExpected) >
                    0.05F * pixelMagnitude + 1.0e-3F) {
                    conditionedRelativeErrors.push_back(
                        static_cast<double>(absoluteError) /
                        static_cast<double>(
                            std::fabs(sceneExpected)));
                } else if ((sceneExpected < 0.0F) !=
                           (actualValue < 0.0F) &&
                           std::fabs(sceneExpected) > absoluteTolerance) {
                    ++signMismatch;
                }
            }
        }

        check(conditionedRelativeErrors.size() > 1000U,
              "demosaic differential must evaluate a meaningful pixel count");
        check(nanCount == 0U, "demosaic kernel must never emit NaN");
        check(outOfTolerance == 0U,
              "every demosaic sample must stay within its scaled absolute "
              "tolerance (worst " +
                  std::to_string(worstAbsolute) + ")");
        check(signMismatch == 0U,
              "negative scene coordinates must preserve their sign");

        std::sort(conditionedRelativeErrors.begin(),
                  conditionedRelativeErrors.end());
        const double median =
            conditionedRelativeErrors[conditionedRelativeErrors.size() / 2U];
        const double p99 = conditionedRelativeErrors
            [static_cast<std::size_t>(
                 static_cast<double>(conditionedRelativeErrors.size()) *
                 0.99)];
        check(median <= 2.0e-3,
              "median relative error must stay under 0.2% (got " +
                  std::to_string(median * 100.0) + "%)");
        check(p99 <= 1.0e-2,
              "p99 relative error must stay under 1% (got " +
                  std::to_string(p99 * 100.0) + "%)");
    }
}

void testVulkanPreprocessDifferential() {
    using namespace latent::imaging;
    using namespace latent::reference;
    using namespace latent::vulkan;

    std::string detail;
    auto runner = ComputeRunner::tryCreate(&detail);
    if (runner == nullptr) {
        std::cout << "compute runner unavailable (" << detail
                  << "); preprocess differential test skipped\n";
        return;
    }

    SensorPreprocessKernel kernel(*runner);

    std::mt19937 rng(20260822U);
    std::uniform_int_distribution<std::uint32_t> codeDist(0U, 1023U);

    struct Case {
        CfaPattern cfa;
        Extent extent;
        bool useLsc;
        std::array<float, 4> wb;
    };
    const std::array<Case, 5> cases{{
        {CfaPattern::RGGB, {64U, 48U}, true, {1.6F, 1.0F, 1.0F, 1.35F}},
        {CfaPattern::GRBG, {64U, 48U}, true, {1.0F, 1.0F, 1.0F, 1.0F}},
        {CfaPattern::GBRG, {61U, 41U}, false, {1.2F, 1.0F, 1.05F, 1.1F}},
        {CfaPattern::BGGR, {33U, 17U}, true, {1.8F, 1.0F, 1.0F, 0.9F}},
        {CfaPattern::RGGB, {256U, 2U}, true, {1.4F, 1.0F, 1.02F, 1.3F}},
    }};

    const std::array<float, 4> blackLevels{80.0F, 84.0F, 84.0F, 88.0F};

    for (const auto& testCase : cases) {
        PreprocessParams params{};
        params.extent = testCase.extent;
        params.cfa = testCase.cfa;
        params.whiteLevel = 1000.0F;
        params.black = blackLevels;
        params.wbGains = testCase.wb;
        if (testCase.useLsc) {
            params.lensShading = makeGradientShadingMap(3, 3);
        }

        std::vector<std::uint16_t> canonical(
            static_cast<std::size_t>(params.extent.pixelCount()));
        std::vector<float> expected(static_cast<std::size_t>(
            params.extent.pixelCount()));

        std::size_t subBlackCount = 0U;
        for (std::uint32_t y = 0; y < params.extent.height; ++y) {
            for (std::uint32_t x = 0; x < params.extent.width; ++x) {
                const auto index =
                    static_cast<std::size_t>(y) * params.extent.width + x;
                const auto channel = static_cast<std::size_t>(
                    cfaChannelAt(testCase.cfa, x, y));
                const auto code = static_cast<std::uint16_t>(codeDist(rng));
                if (static_cast<float>(code) < blackLevels[channel]) {
                    ++subBlackCount;
                }

                canonical[index] = code;
                const float range =
                    params.whiteLevel - blackLevels[channel];
                const float normalized =
                    (static_cast<float>(code) - blackLevels[channel]) / range;
                float value = normalized;
                if (testCase.useLsc) {
                    value *= lensShadingGainAt(
                        params.lensShading, params.extent, testCase.cfa, x, y);
                }
                expected[index] = value * params.wbGains[channel];
            }
        }

        check(subBlackCount > 0U,
              "random codes must include sub-black samples for the "
              "negative-preservation check");

        const auto gpuHalves = kernel.run(params, canonical);
        check(gpuHalves.size() == expected.size(),
              "kernel output must match input sample count");

        std::size_t exact = 0U;
        std::size_t withinTolerance = 0U;
        std::size_t nanCount = 0U;
        std::size_t negativeCount = 0U;
        std::size_t expectedNegativeCount = 0U;

        for (std::size_t i = 0; i < expected.size(); ++i) {
            const std::uint16_t expectedBits = floatToHalfBits(expected[i]);
            const std::uint16_t actualBits = gpuHalves[i];

            if (((actualBits & 0x7C00U) == 0x7C00U) &&
                ((actualBits & 0x03FFU) != 0U)) {
                ++nanCount;
                continue;
            }

            if ((actualBits & 0x8000U) != 0U) {
                ++negativeCount;
            }
            if ((expectedBits & 0x8000U) != 0U) {
                ++expectedNegativeCount;
            }

            if (actualBits == expectedBits) {
                ++exact;
                ++withinTolerance;
                continue;
            }

            const float actualValue = halfBitsToFloat(actualBits);
            const float expectedValue = halfBitsToFloat(expectedBits);
            const float magnitude = std::max(std::fabs(expectedValue), 1.0e-6F);
            const float relativeError =
                std::fabs(actualValue - expectedValue) / magnitude;
            // One FP16 ulp is 2^-11 of the value; allow two plus a small
            // epsilon for near-tie rounding differences.
            if (relativeError <= 1.0e-2F) {
                ++withinTolerance;
            }
        }

        const std::string prefix = "preprocess differential [" +
                                   std::to_string(static_cast<int>(testCase.cfa)) + "]";
        check(nanCount == 0U, prefix + ": kernel must never emit NaN");
        check(withinTolerance == expected.size(),
              prefix + ": every sample must match within tolerance (" +
                  std::to_string(expected.size() - withinTolerance) +
                  " outliers)");
        const double exactRatio =
            static_cast<double>(exact) / static_cast<double>(expected.size());
        // Bit-exactness is the expected outcome for matching operation orders
        // but is not the contract: drivers may evaluate knife-edge fp16
        // midpoints differently (observed on MoltenVK for ~0.2% of samples,
        // always within one ulp). The hard gates are the tolerance budget,
        // zero NaNs, and sign preservation below black.
        check(exactRatio >= 0.995,
              prefix + ": bit-exact ratio must be >= 99.5% (got " +
                  std::to_string(exactRatio * 100.0) + "%)");
        check(negativeCount == expectedNegativeCount,
              prefix + ": sub-black samples must stay negative");
    }
}

void testVulkanRuntimeSmoke() {
    const auto availability = latent::vulkan::VulkanRuntime::tryInitialize();
    if (!availability.loaderAvailable) {
        std::cout << "vulkan loader unavailable; runtime smoke test skipped\n";
        return;
    }

    const auto& caps = latent::vulkan::VulkanRuntime::deviceCaps();
    if (availability.instanceCreated && availability.deviceCreated) {
        const auto assessment = latent::vulkan::assessProductionSupport(caps);
        check(assessment.support != latent::vulkan::ProductionSupport::Unsupported,
              "an initialized device must satisfy its own baseline assessment");
        check(caps.computeQueue, "initialized runtime must report its compute queue");
        check(latent::vulkan::isAtLeast(caps.apiVersion, 1U, 1U),
              "device creation requires requesting Vulkan 1.1");
    } else {
        std::cout << "vulkan device unavailable (" << availability.detail
                  << "); capability-only smoke completed\n";
    }

    latent::vulkan::VulkanRuntime::shutdown();
    check(!latent::vulkan::VulkanRuntime::available(),
          "shutdown must release the runtime");
}
#endif

struct HalfGolden {
    std::uint32_t valueBits;
    std::uint16_t bits;
};
constexpr std::array<HalfGolden, 111> kHalfGoldens{{
    {0x00000000U, 0x0000U},
    {0x80000000U, 0x8000U},
    {0x3F800000U, 0x3C00U},
    {0xBF800000U, 0xBC00U},
    {0x3F000000U, 0x3800U},
    {0x40000000U, 0x4000U},
    {0x477FE000U, 0x7BFFU},
    {0x477FEF00U, 0x7BFFU},
    {0x477FF000U, 0x7C00U},
    {0x477FFF00U, 0x7C00U},
    {0x38800000U, 0x0400U},
    {0x33800000U, 0x0001U},
    {0x33000000U, 0x0000U},
    {0x33C00000U, 0x0002U},
    {0x7F800000U, 0x7C00U},
    {0xFF800000U, 0xFC00U},
    {0x7FC00000U, 0x7E00U},
    {0x45000000U, 0x6800U},
    {0x45001000U, 0x6800U},
    {0x45800100U, 0x6C00U},
    {0x45C00400U, 0x6E00U},
    {0x4715D1D9U, 0x78AFU},
    {0xC605B413U, 0xF02EU},
    {0x47441BB5U, 0x7A21U},
    {0x46D7DF0CU, 0x76BFU},
    {0xC75DEF2CU, 0xFAEFU},
    {0x47820D91U, 0x7C00U},
    {0x470ECF8FU, 0x7876U},
    {0x471C7101U, 0x78E4U},
    {0xC74B6017U, 0xFA5BU},
    {0xC5D90FC0U, 0xEEC8U},
    {0xC68D508EU, 0xF46BU},
    {0x47696319U, 0x7B4BU},
    {0x469D5A3CU, 0x74EBU},
    {0x473082A0U, 0x7984U},
    {0xC5F79019U, 0xEFBDU},
    {0xC7152A94U, 0xF8A9U},
    {0x45EECEF6U, 0x6F76U},
    {0xC76E8996U, 0xFB74U},
    {0x47332C5DU, 0x7999U},
    {0x46900208U, 0x7480U},
    {0x470D2449U, 0x7869U},
    {0xC69F1CBBU, 0xF4F9U},
    {0x4780B4DDU, 0x7C00U},
    {0x4756FCF5U, 0x7AB8U},
    {0x47183DB1U, 0x78C2U},
    {0xC726FE95U, 0xF938U},
    {0xC591987AU, 0xEC8DU},
    {0xC7797B79U, 0xFBCCU},
    {0xC73D0F79U, 0xF9E8U},
    {0x46C835B5U, 0x7642U},
    {0x4705DAB4U, 0x782FU},
    {0x477FAB5DU, 0x7BFDU},
    {0xC6BE80E6U, 0xF5F4U},
    {0xC68DAF48U, 0xF46DU},
    {0xC585317EU, 0xEC2AU},
    {0xC729D202U, 0xF94FU},
    {0xC74A62FDU, 0xFA53U},
    {0xC55494F7U, 0xEAA5U},
    {0xC71558B1U, 0xF8ABU},
    {0x46B9BBEBU, 0x75CEU},
    {0xC6097AEDU, 0xF04CU},
    {0x4735EEF3U, 0x79AFU},
    {0x46DB0A3BU, 0x76D8U},
    {0xC6CD3957U, 0xF66AU},
    {0x4735B45FU, 0x79AEU},
    {0x4726AB03U, 0x7935U},
    {0xC676241CU, 0xF3B1U},
    {0xC6E78422U, 0xF73CU},
    {0x46C79ABEU, 0x763DU},
    {0xC74502A7U, 0xFA28U},
    {0xC7241CDAU, 0xF921U},
    {0xC786B4A4U, 0xFC00U},
    {0x471CE96AU, 0x78E7U},
    {0x46B44E3DU, 0x75A2U},
    {0x46E0664EU, 0x7703U},
    {0x47198610U, 0x78CCU},
    {0xC5B3BE55U, 0xED9EU},
    {0x46165F12U, 0x70B3U},
    {0xC744FC6CU, 0xFA28U},
    {0xC752CDCAU, 0xFA96U},
    {0x34B4D241U, 0x0006U},
    {0xB378481DU, 0x8001U},
    {0x340C17EEU, 0x0002U},
    {0x350E452AU, 0x0009U},
    {0x3490A717U, 0x0005U},
    {0x33E61F2DU, 0x0002U},
    {0x33FE4AF6U, 0x0002U},
    {0xB4D281C9U, 0x8007U},
    {0xB57BE3E8U, 0x8010U},
    {0xB407E5FCU, 0x8002U},
    {0xB5193B2FU, 0x800AU},
    {0xB4446EE9U, 0x8003U},
    {0x353DBB59U, 0x000CU},
    {0xB50ED714U, 0x8009U},
    {0xB56D2269U, 0x800FU},
    {0xB4EABCBDU, 0x8007U},
    {0xB4DDA084U, 0x8007U},
    {0x34ADDB46U, 0x0005U},
    {0x33F4F384U, 0x0002U},
    {0x35186AACU, 0x000AU},
    {0x34B06E29U, 0x0006U},
    {0xB449085EU, 0x8003U},
    {0x352896A2U, 0x000BU},
    {0xB532CAE5U, 0x800BU},
    {0xB5801EFAU, 0x8010U},
    {0xB55C1765U, 0x800EU},
    {0x34EEC1ACU, 0x0007U},
    {0xB3A3BC6EU, 0x8001U},
    {0xB535DA74U, 0x800BU},
    {0x310F97C2U, 0x0000U},
}};

void testHalfConversionGoldenVectors() {
    using namespace latent::imaging;

    for (const auto& golden : kHalfGoldens) {
        float value = 0.0F;
        std::memcpy(&value, &golden.valueBits, sizeof(value));
        const auto converted = floatToHalfBits(value);

        const bool inputIsNan =
            (golden.valueBits & 0x7FFFFFFFU) > 0x7F800000U;
        if (inputIsNan) {
            check(((converted & 0x7C00U) == 0x7C00U) &&
                      ((converted & 0x03FFU) != 0U),
                  "NaN must convert to a half NaN");
            continue;
        }
        if (converted != golden.bits) {
            ++failures;
            std::cerr << "FAIL: half golden mismatch value=0x" << std::hex
                      << golden.valueBits << " expected=0x" << golden.bits
                      << " actual=0x" << converted << std::dec << '\n';
        }

        // Round trip through the decoder must recover the original f32
        // exactly whenever the source is exactly representable.
        const auto decoded = halfBitsToFloat(golden.bits);
        if ((golden.bits & 0x7C00U) != 0x7C00U) {
            const auto reEncoded = floatToHalfBits(decoded);
            check(reEncoded == golden.bits,
                  "half decode/encode round trip must be stable");
        }
    }
}

}  // namespace

int main() {
    try {
        testMetadataPriorityAndNegativePreservation();
        testMetadataTrustBeatsSourcePriority();
        testSceneValuesRemainUnbounded();
        testSceneScaleChangesStoredCoordinates();
        testImageTypeValidation();
        testReferenceDomainChangesMustBeExplicit();
        testInvalidReconstructionConfigIsRejected();
        testGraphFusionStopsAtReduction();
        testVulkanCapabilityBaseline();
        testMatrixOperations();
        testRp177PrimariesDerivationMatchesOfficialAcesMatrices();
        testBradfordAdaptationMatchesPublishedMatrix();
        testCorrelatedTemperatureEstimators();
        testDngInterpolationGoldenAndClamping();
        testCameraNeutralGoldenAndRoundTrip();
        testCameraToXyzD50ForwardMatrixGolden();
        testInverseColorMatrixFallbackAlsoMapsNeutralToD50();
        testXyzD50ToAcescgWhiteMapping();
        testDngProfileValidationRejectsBadProfiles();
        testMalvarHeCutlerGoldenVectors();
        testMalvarHeCutlerProperties();
        testEndToEndDngPipelineEquivalence();
        testLensShadingMapValidation();
        testLensShadingApplication();
        testDefectCorrection();
        testDefectDetection();
        testNoiseModelValidationAndTransform();
        testTapWeightsMirrorDemosaicOutput();
        testPropagatedSigmaAnalyticCases();
        testMonteCarloNoisePropagation();
        testPropagatedNoiseDefaultsAndGating();
        testRawPackingGoldenVectors();
        testRawPackingRoundTrip();
        testRawPackingValidation();
        testHalfConversionGoldenVectors();
        testIngressDecisionTable();
#ifdef LATENT_ENABLE_VULKAN_RUNTIME
        testVulkanPreprocessDifferential();
        testVulkanDemosaicColorDifferential();
        testVulkanRuntimeSmoke();
#endif
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
