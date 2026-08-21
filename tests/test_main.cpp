#include "latent/backend/Backend.h"
#include "latent/graph/ImagingIR.h"
#include "latent/imaging/ColorScience.h"
#include "latent/reference/Demosaic.h"
#include "latent/reference/DngColor.h"
#include "latent/reference/RawNormalize.h"
#include "latent/vulkan/DeviceCaps.h"

#include <array>
#include <cmath>
#include <cstddef>
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
