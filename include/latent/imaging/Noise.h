#pragma once

#include "latent/imaging/RawFrame.h"
#include "latent/imaging/Types.h"

#include <array>

namespace latent::imaging {

// Lazy, fully composable description of per-pixel noise variance for a
// SceneFrame. Data only: evaluation lives in the reference backend
// (reference::propagatedSigma) so the imaging layer stays free of
// backend dependencies.
//
// Variance model per output RGB channel (before demosaic mixing):
//   Var[s_c(p)] = (wbGain_c * lscGain_c(p))^2 * (shot_c * x_c + read_c)
// where x_c is the pre-gain sensor-linear signal estimate and s_c is the
// gained sample feeding the demosaic kernel.
struct PropagatedNoise {
    bool valid = false;

    Extent extent{};
    CfaPattern cfa = CfaPattern::RGGB;
    DemosaicMethod demosaicMethod = DemosaicMethod::MalvarHeCutler2004;

    // Per-CFA-channel noise after black/white normalization, before WB/LSC.
    std::array<float, 4> shot{0.0F, 0.0F, 0.0F, 0.0F};
    std::array<float, 4> read{0.0F, 0.0F, 0.0F, 0.0F};

    std::array<float, 4> whiteBalanceGains{1.0F, 1.0F, 1.0F, 1.0F};

    bool lensShadingApplied = false;
    LensShadingMap lensShading{};

    // Precomputed inverse of the camera-to-scene matrix used by the run.
    Matrix3f sceneToCamera{};
    float sceneScale = 1.0F;
};

}  // namespace latent::imaging
