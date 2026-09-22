#pragma once
#include "latent/imaging/RawBurst.h"
#include "latent/reference/DirectReconstruct.h"
#include "latent/reference/TemporalReconstruct.h"
#include "latent/runtime/RawBindings.h"
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace latent::runtime {
inline constexpr const char* kDirectReconstructionVersion = "latent.direct-cfa.1";
class RawTileSource {
public:
    virtual ~RawTileSource() = default;
    // Synchronous, packed rows, exact bounds. Source must throw on a short read.
    virtual void read(imaging::FrameId, imaging::SensorRect, std::span<std::uint16_t>) = 0;
    [[nodiscard]] virtual std::uint64_t residentBytes() const = 0;
};
[[nodiscard]] std::unique_ptr<RawTileSource> makeHostTileSource(const imaging::RawBurst&,const HostRawBindings&);
struct RawFileBinding {
    imaging::FrameId frame{};
    std::string path;
    std::uint64_t offsetBytes = 0;
    std::uint32_t rowStridePixels = 0;
};
// Packed little-endian RAW16 (not a DNG parser). Files are never removed or
// rewritten by the source. No mmap/full-file page fault residency assumption.
[[nodiscard]] std::unique_ptr<RawTileSource> makeFileTileSource(const imaging::RawBurst&,std::span<const RawFileBinding>);
class ReconstructionTileSink {
public:
    virtual ~ReconstructionTileSink() = default;
    virtual void write(imaging::SensorRect,std::span<const reference::ReconstructedPixel>) = 0;
    [[nodiscard]] virtual std::uint64_t residentBytes() const = 0;
};
struct ReconstructionGrid {
    imaging::Extent extent{};
    // Reference-buffer pixel center coordinates of output (0,0), and steps.
    float originX = 0, originY = 0, stepX = 1, stepY = 1;
};
struct ReconstructionMotion {
    imaging::FrameId frame{};
    // Affine + optional row displacement + piecewise local residual in reference
    // RAW pixel coordinates. No hidden change of sensor/zoom calibration space.
    std::array<float,6> affine{1,0,0,0,1,0};
    float rowDx = 0, rowDy = 0, confidence = 1;
    std::uint32_t tileSize = 0, columns = 0, rows = 0;
    std::vector<reference::MotionTile> residual;
};
struct TiledReconstructionPolicy {
    reference::DirectKernelPolicy kernel{};
    std::uint32_t tileSize = 128, maximumFrames = 8, maximumDisplacement = 64;
    std::uint64_t guideBudgetBytes = 16U*1024U*1024U;
    bool allowFrameReduction = true;
    bool allowNominalIsoGainEstimate = false;
    bool applyLensShading = true;
    float missingNoiseVariance = 1e-4F;
};
struct ReconstructionCapabilities {
    std::uint64_t hostBudgetBytes = 256U*1024U*1024U;
    std::uint64_t deviceBudgetBytes = 64U*1024U*1024U;
    std::uint64_t maxStorageBufferRange = 128U*1024U*1024U;
    bool preferVulkan = false;
    bool collectGpuTimings = false;
    // 0: nominal, 1: moderate, 2: severe. Observation, not an image intent.
    std::uint32_t thermalSeverity = 0;
};
struct TiledReconstructionPlan {
    std::uint32_t tileSize = 0, guideStep = 0, frames = 0, radiusX = 0, radiusY = 0;
    std::uint32_t sourceTileSide = 0;
    std::uint64_t hostWorkingBytes = 0, sourceResidentBytes = 0, sinkResidentBytes = 0, deviceWorkingBytes = 0;
    bool vulkan = false;
    std::vector<std::string> decisions;
};
struct DirectReconstructionTrace {
    imaging::FrameId reference{};
    TiledReconstructionPlan plan{};
    std::vector<ReconstructionMotion> motion;
    std::vector<std::string> rejectedFrames;
    std::uint64_t rawBytesRead = 0, transferBytes = 0, mappedAccessBytes = 0, outputPixels = 0, tiles = 0;
    // Owned arena allocations; allocator/runtime/driver internal allocations are
    // not claimed to be included. resident measurement belongs to benchmarks.
    std::uint64_t arenaAllocations = 0, invalidChannels = 0, referenceFallbackChannels = 0;
    double guideMilliseconds = 0, alignmentMilliseconds = 0, reconstructionMilliseconds = 0;
    double totalMilliseconds = 0, gpuMilliseconds = -1;
    double meanEffectiveFrames = 0, meanConfidence = 0, meanVariance = 0;
};
[[nodiscard]] TiledReconstructionPlan planTiledReconstruction(const imaging::RawBurst&,const ReconstructionGrid&,
    const TiledReconstructionPolicy&,const ReconstructionCapabilities&,std::uint64_t sourceResidentBytes,
    std::uint64_t sinkResidentBytes = 0);
// Explicit motion is an observation used by deterministic fixtures / calibrated
// external registration. Empty motion means estimate from low-resolution guides.
// Cancellation throws; sinks must stage output and publish only after success.
[[nodiscard]] DirectReconstructionTrace reconstructRawTiles(const imaging::RawBurst&,RawTileSource&,
    const ReconstructionGrid&,const TiledReconstructionPolicy&,const ReconstructionCapabilities&,
    ReconstructionTileSink&,std::span<const ReconstructionMotion> motion = {},
    std::optional<imaging::FrameId> reference = {},std::function<bool()> continueExecution = {});
[[nodiscard]] std::string directReconstructionJson(const DirectReconstructionTrace&);
// Converts evidence only within a requested tile; useful for debug and oracles.
void normalizeMosaicTile(const imaging::RawFrameMetadata&,const imaging::RawFrameMetadata& reference,
    imaging::Extent,imaging::SensorRect,std::span<const std::uint16_t>,const TiledReconstructionPolicy&,
    std::span<reference::MosaicEvidence>);
}  // namespace latent::runtime
