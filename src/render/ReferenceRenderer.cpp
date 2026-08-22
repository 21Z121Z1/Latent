#include "latent/render/ReferenceRenderer.h"

#include "latent/imaging/ColorScience.h"
#include "latent/render/AcesToneScale.h"
#include "latent/render/OutputEncoding.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace latent::render {
namespace {

constexpr imaging::ChromaticityXY kRec709Red{0.640F, 0.330F};
constexpr imaging::ChromaticityXY kRec709Green{0.300F, 0.600F};
constexpr imaging::ChromaticityXY kRec709Blue{0.150F, 0.060F};

constexpr imaging::ChromaticityXY kBt2020Red{0.708F, 0.292F};
constexpr imaging::ChromaticityXY kBt2020Green{0.170F, 0.797F};
constexpr imaging::ChromaticityXY kBt2020Blue{0.131F, 0.046F};

void validateScene(const imaging::SceneFrame& scene) {
    if (scene.reference != imaging::ReferenceDomain::Scene ||
        scene.transfer != imaging::TransferFunction::Linear ||
        scene.range != imaging::RangeSemantics::Unbounded ||
        !scene.allowNegative) {
        throw std::invalid_argument(
            "reference rendering requires the linear unbounded SceneFrame contract");
    }
    if (scene.primaries != imaging::Primaries::ACEScgAP1 ||
        scene.whitePoint != imaging::WhitePoint::D60) {
        throw std::invalid_argument(
            "reference rendering currently requires ACEScg/AP1 with D60 white");
    }
    if (!std::isfinite(scene.sceneScaleEV)) {
        throw std::invalid_argument("sceneScaleEV must be finite");
    }
    if (scene.image.extent.width == 0U || scene.image.extent.height == 0U) {
        throw std::invalid_argument("reference rendering requires a non-empty image");
    }

    const std::uint64_t pixelCount = scene.image.extent.pixelCount();
    if (pixelCount >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / 3U)) {
        throw std::invalid_argument("scene image is too large for host indexing");
    }
    const std::size_t expected = static_cast<std::size_t>(pixelCount) * 3U;
    if (scene.image.rgb.size() != expected) {
        throw std::invalid_argument(
            "SceneFrame RGB payload does not match its declared extent");
    }
}

void validateConfig(const RenderConfig& config) {
    if (config.intent != imaging::RenderIntent::SDR &&
        config.intent != imaging::RenderIntent::HDR) {
        throw std::invalid_argument("unsupported render intent");
    }
    if (!std::isfinite(config.renderExposureEV)) {
        throw std::invalid_argument("renderExposureEV must be finite");
    }
    if (!std::isfinite(config.peakTargetNits) ||
        config.peakTargetNits < 100.0F || config.peakTargetNits > 10000.0F) {
        throw std::invalid_argument(
            "render peak must be within the ACES tonescale range [100, 10000] nits");
    }
    if (!std::isfinite(config.nominalWhiteNits) ||
        !(config.nominalWhiteNits > 0.0F) ||
        config.nominalWhiteNits > config.peakTargetNits) {
        throw std::invalid_argument(
            "nominal display white must be finite, positive, and no greater than peak");
    }
    if (config.intent == imaging::RenderIntent::SDR &&
        std::fabs(config.nominalWhiteNits - config.peakTargetNits) > 1.0e-5F) {
        throw std::invalid_argument(
            "the current SDR reference branch requires nominal white equal to peak");
    }
}

imaging::Matrix3f makeAp1ToOutputMatrix(imaging::RenderIntent intent) {
    const imaging::Matrix3f ap1ToXyzD60 = imaging::rgbPrimariesToXyzMatrix(
        imaging::kAp1Red,
        imaging::kAp1Green,
        imaging::kAp1Blue,
        imaging::kAcesWhite);
    const imaging::Matrix3f d60ToD65 =
        imaging::bradfordAdaptation(imaging::kAcesWhite, imaging::kIlluminantD65);

    imaging::Matrix3f outputToXyzD65{};
    if (intent == imaging::RenderIntent::SDR) {
        outputToXyzD65 = imaging::rgbPrimariesToXyzMatrix(
            kRec709Red,
            kRec709Green,
            kRec709Blue,
            imaging::kIlluminantD65);
    } else {
        outputToXyzD65 = imaging::rgbPrimariesToXyzMatrix(
            kBt2020Red,
            kBt2020Green,
            kBt2020Blue,
            imaging::kIlluminantD65);
    }

    const auto xyzD65ToOutput = imaging::inverted(outputToXyzD65);
    if (!xyzD65ToOutput.has_value()) {
        throw std::runtime_error("output primary matrix must be invertible");
    }
    return xyzD65ToOutput->multiplied(d60ToD65).multiplied(ap1ToXyzD60);
}

std::array<float, 3> compressToTargetGamut(
    const std::array<float, 3>& rgbNits,
    float targetLuminanceNits,
    float peakTargetNits) {
    const float neutral = std::clamp(targetLuminanceNits, 0.0F, peakTargetNits);
    float chromaScale = 1.0F;

    for (const float component : rgbNits) {
        if (!std::isfinite(component)) {
            throw std::invalid_argument(
                "reference rendering produced a non-finite output-primary coordinate");
        }

        const float delta = component - neutral;
        if (delta > 0.0F) {
            chromaScale = std::min(
                chromaScale,
                (peakTargetNits - neutral) / delta);
        } else if (delta < 0.0F) {
            chromaScale = std::min(chromaScale, (0.0F - neutral) / delta);
        }
    }

    chromaScale = std::clamp(chromaScale, 0.0F, 1.0F);
    std::array<float, 3> compressed{};
    for (std::size_t channel = 0U; channel < compressed.size(); ++channel) {
        compressed[channel] = std::clamp(
            neutral + chromaScale * (rgbNits[channel] - neutral),
            0.0F,
            peakTargetNits);
    }
    return compressed;
}

std::array<float, 3> encodeOutput(
    const std::array<float, 3>& rgbNits,
    const RenderConfig& config) {
    std::array<float, 3> encoded{};
    for (std::size_t channel = 0U; channel < encoded.size(); ++channel) {
        if (config.intent == imaging::RenderIntent::SDR) {
            const float normalized = rgbNits[channel] / config.peakTargetNits;
            encoded[channel] = encodeSrgb(std::clamp(normalized, 0.0F, 1.0F));
        } else {
            encoded[channel] = encodePqFromNits(
                std::clamp(rgbNits[channel], 0.0F, config.peakTargetNits));
        }
    }
    return encoded;
}

}  // namespace

RenderConfig makeSdrRenderConfig(float renderExposureEV, float peakTargetNits) {
    RenderConfig config{};
    config.intent = imaging::RenderIntent::SDR;
    config.renderExposureEV = renderExposureEV;
    config.nominalWhiteNits = peakTargetNits;
    config.peakTargetNits = peakTargetNits;
    validateConfig(config);
    return config;
}

RenderConfig makeHdrPqRenderConfig(
    float renderExposureEV,
    float peakTargetNits,
    float nominalWhiteNits) {
    RenderConfig config{};
    config.intent = imaging::RenderIntent::HDR;
    config.renderExposureEV = renderExposureEV;
    config.nominalWhiteNits = nominalWhiteNits;
    config.peakTargetNits = peakTargetNits;
    validateConfig(config);
    return config;
}

imaging::RenderedFrame renderReference(
    const imaging::SceneFrame& scene,
    const RenderConfig& config) {
    validateScene(scene);
    validateConfig(config);

    const float coordinateToRenderScale =
        std::exp2(config.renderExposureEV - scene.sceneScaleEV);
    if (!std::isfinite(coordinateToRenderScale) ||
        !(coordinateToRenderScale > 0.0F)) {
        throw std::invalid_argument(
            "render exposure and scene scale produce an unusable coordinate transform");
    }

    const imaging::Matrix3f ap1ToXyzD60 = imaging::rgbPrimariesToXyzMatrix(
        imaging::kAp1Red,
        imaging::kAp1Green,
        imaging::kAp1Blue,
        imaging::kAcesWhite);
    const imaging::Matrix3f d60ToD65 =
        imaging::bradfordAdaptation(imaging::kAcesWhite, imaging::kIlluminantD65);
    const imaging::Matrix3f ap1ToOutput = makeAp1ToOutputMatrix(config.intent);
    const AcesToneScaleParams tone =
        makeAcesToneScaleParams(config.peakTargetNits);

    imaging::RenderedFrame rendered{};
    rendered.sourceRawId = scene.sourceRawId;
    rendered.intent = config.intent;
    rendered.primaries = config.intent == imaging::RenderIntent::SDR
                             ? imaging::Primaries::SRGBRec709
                             : imaging::Primaries::BT2020;
    rendered.whitePoint = imaging::WhitePoint::D65;
    rendered.transfer = config.intent == imaging::RenderIntent::SDR
                            ? imaging::TransferFunction::SRGB
                            : imaging::TransferFunction::PQ;
    rendered.reference = imaging::ReferenceDomain::Display;
    rendered.range = imaging::RangeSemantics::EncodedDisplay;
    rendered.allowNegative = false;
    rendered.renderExposureEV = config.renderExposureEV;
    rendered.nominalWhiteNits = config.nominalWhiteNits;
    rendered.peakTargetNits = config.peakTargetNits;
    rendered.hdrHeadroom = config.peakTargetNits / config.nominalWhiteNits;
    rendered.image.extent = scene.image.extent;
    rendered.image.rgb.reserve(scene.image.rgb.size());

    const std::size_t pixelCount =
        static_cast<std::size_t>(scene.image.extent.pixelCount());
    for (std::size_t pixel = 0U; pixel < pixelCount; ++pixel) {
        const std::size_t base = pixel * 3U;
        std::array<float, 3> exposedAp1{};
        for (std::size_t channel = 0U; channel < exposedAp1.size(); ++channel) {
            const float stored = scene.image.rgb[base + channel];
            if (!std::isfinite(stored)) {
                throw std::invalid_argument(
                    "reference rendering rejects NaN/Inf SceneFrame samples");
            }
            exposedAp1[channel] = stored * coordinateToRenderScale;
            if (!std::isfinite(exposedAp1[channel])) {
                throw std::invalid_argument(
                    "reference rendering exposure produced a non-finite scene sample");
            }
        }

        const auto xyzD60 = ap1ToXyzD60.apply(exposedAp1);
        const float sceneLuminance = xyzD60[1];
        if (!(sceneLuminance > 0.0F) || !std::isfinite(sceneLuminance)) {
            const auto encodedBlack = encodeOutput({0.0F, 0.0F, 0.0F}, config);
            rendered.image.rgb.insert(
                rendered.image.rgb.end(), encodedBlack.begin(), encodedBlack.end());
            continue;
        }

        const float targetLuminance = std::clamp(
            acesToneScaleForward(sceneLuminance, tone),
            0.0F,
            config.peakTargetNits);

        const auto xyzD65 = d60ToD65.apply(xyzD60);
        const float adaptedLuminance = xyzD65[1];
        if (!(adaptedLuminance > 0.0F) || !std::isfinite(adaptedLuminance)) {
            const auto encodedNeutral = encodeOutput(
                {targetLuminance, targetLuminance, targetLuminance}, config);
            rendered.image.rgb.insert(
                rendered.image.rgb.end(), encodedNeutral.begin(), encodedNeutral.end());
            continue;
        }

        const auto outputSceneRgb = ap1ToOutput.apply(exposedAp1);
        const float luminanceScale = targetLuminance / adaptedLuminance;
        std::array<float, 3> outputNits{};
        for (std::size_t channel = 0U; channel < outputNits.size(); ++channel) {
            outputNits[channel] = outputSceneRgb[channel] * luminanceScale;
        }

        const auto compressed = compressToTargetGamut(
            outputNits,
            targetLuminance,
            config.peakTargetNits);
        const auto encoded = encodeOutput(compressed, config);
        rendered.image.rgb.insert(
            rendered.image.rgb.end(), encoded.begin(), encoded.end());
    }

    return rendered;
}

}  // namespace latent::render
