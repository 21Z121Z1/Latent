#pragma once

#include "latent/reference/JointScene.h"
#include "latent/runtime/TiledReconstruction.h"

namespace latent::runtime {

struct JointSceneResult {
    imaging::SceneFrame scene;
    // Interleaved scene RGB bounds; empty only when color.propagateNoise=false.
    // See finishDirectSceneTile. Never masquerades as SceneFrame's affine noise.
    std::vector<float> conditionalVarianceUpperBound;
    DirectReconstructionTrace trace;
};

// Materializing scene adapter over the canonical STREAMING reconstruction.
// Only final scene RGB and optional variance bounds are image-sized; there is no
// full-frame camera RGB, feature tensor, FusedRaw, or second reconstruction.
// Output residency is admitted against caps before allocation or source reads.
// For larger-than-RAM output use reconstructRawTiles with a streaming sink.
//
// The explicit reference fixes grid/radiometry AND the G0/G1 balance observation.
// A mismatched green ratio in color is rejected, not silently reinterpreted.
// Sensor corrections belong to policy, so color's sensor correction flags must
// be disabled. Every output color requires positive coverage; uncovered pixels
// fail the scene operation rather than being silently displayed as black.
//
// continueExecution, when supplied, returns true to proceed and false to cancel.
// Cancellation discards the partial scene and propagates as an exception.
//
// Lineage retains all capture-order inputs, even when delegated frame reduction
// or rejection excludes some. Per-frame accepted tap counts are not inferred
// from aggregate moments: contributions remain unavailable (empty), not zero.
[[nodiscard]] JointSceneResult reconstructRawScene(const imaging::RawBurst& burst,
    RawTileSource& source, const ReconstructionGrid& grid, imaging::FrameId reference,
    const reference::ReconstructionConfig& color, const TiledReconstructionPolicy& policy = {},
    const ReconstructionCapabilities& caps = {},
    std::span<const ReconstructionMotion> motion = {}, std::function<bool()> continueExecution = {});

}  // namespace latent::runtime
