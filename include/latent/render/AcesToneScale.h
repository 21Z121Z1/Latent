#pragma once

namespace latent::render {

// Parameters for the ACES 2 reference tonescale scalar function. This is a
// reusable luminance/lightness primitive, NOT the complete ACES 2 Output
// Transform: the latter also performs JMh conversion, chroma compression,
// gamut compression, limiting-primaries conversion, and display encoding.
struct AcesToneScaleParams {
    float peakLuminanceNits = 100.0F;
    float normalizedWhiteNits = 100.0F;
    float contrast = 1.15F;
    float toe = 0.04F;
    float s2 = 0.0F;
    float u2 = 0.0F;
    float m2 = 0.0F;
    float forwardLimit = 0.0F;
    float inverseLimit = 0.0F;
    float logPeak = 0.0F;
};

// Transcription of the scalar tonescale parameterization from the official
// Apache-2.0 ACES 2 reference library, Lib.Academy.Tonescale.ctl. Peak values
// are constrained to the published 100-10000 nit design range.
[[nodiscard]] AcesToneScaleParams makeAcesToneScaleParams(
    float peakLuminanceNits);

// Maps a non-negative scene-referred scalar to display luminance in nits.
// Applying this independently to R/G/B is intentionally NOT supported by this
// API because that would not be the ACES 2 Output Transform.
[[nodiscard]] float acesToneScaleForward(
    float sceneValue,
    const AcesToneScaleParams& params);

}  // namespace latent::render
