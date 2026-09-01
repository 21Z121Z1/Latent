#pragma once

#include "latent/imaging/RenderedFrame.h"
#include "latent/imaging/SceneFrame.h"

namespace latent::render {

struct RenderConfig {
    imaging::RenderIntent intent = imaging::RenderIntent::SDR;
    float renderExposureEV = 0.0F;
    float nominalWhiteNits = 100.0F;
    float peakTargetNits = 100.0F;
};

[[nodiscard]] RenderConfig makeSdrRenderConfig(
    float renderExposureEV = 0.0F,
    float peakTargetNits = 100.0F);

[[nodiscard]] RenderConfig makeHdrPqRenderConfig(
    float renderExposureEV = 0.0F,
    float peakTargetNits = 1000.0F,
    float nominalWhiteNits = 203.0F);

// Deterministic FP32 reference rendering from Latent's scene-referred AP1/D60
// master into an encoded display rendition. This deliberately composes only
// primitives whose semantics are explicit in this repository:
//
//   scene coordinate unscale -> render exposure -> scalar ACES 2 tonescale
//   -> AP1/D60 to output primaries/D65 -> neutral-axis gamut compression
//   -> sRGB or PQ encoding.
//
// It is NOT the complete ACES 2 Output Transform. In particular it does not
// claim the ACES JMh/chroma-compression/gamut-compression appearance model.
// Its purpose is to establish the stable SceneFrame -> independent SDR/HDR
// rendition contract that production Vulkan and libultrahdr integration can
// differential-test against.
[[nodiscard]] imaging::RenderedFrame renderReference(
    const imaging::SceneFrame& scene,
    const RenderConfig& config);

}  // namespace latent::render
