#pragma once

#include "latent/imaging/SceneFrame.h"

#include <cstddef>

namespace latent::render {

struct SceneLuminanceStats {
    std::size_t pixelCount = 0U;
    std::size_t positiveLuminanceCount = 0U;
    std::size_t nonPositiveLuminanceCount = 0U;

    // Percentiles are expressed in EV of exposure-relative scene luminance.
    // Stored SceneFrame coordinates are first made invariant to sceneScaleEV:
    // log2(relative luminance) = log2(stored luminance) - sceneScaleEV.
    float minimumEV = 0.0F;
    float p01EV = 0.0F;
    float medianEV = 0.0F;
    float p99EV = 0.0F;
    float maximumEV = 0.0F;
};

// Reference scene analysis for the current ACEScg/AP1-D60 SceneFrame contract.
// It performs no rendering and never rewrites SceneFrame pixels.
[[nodiscard]] SceneLuminanceStats analyzeSceneLuminance(
    const imaging::SceneFrame& scene);

// A policy helper only: choose render exposure so the analyzed median scene
// luminance lands on targetMedianLinear. This value belongs to rendering
// intent; it must never be folded back into RAW normalization or sceneScaleEV.
[[nodiscard]] float suggestRenderExposureEV(
    const SceneLuminanceStats& stats,
    float targetMedianLinear = 0.18F);

}  // namespace latent::render
