#pragma once
#include "latent/graph/TemporalIR.h"
#include "latent/reference/TemporalReconstruct.h"
#include <functional>
#include <stdexcept>

namespace latent::runtime {
enum class TemporalBackend : std::uint8_t { Reference, Vulkan };
enum class TemporalFallback : std::uint8_t { None, VulkanUnavailable, UncalibratedGain, EstimatedNoise };
struct TemporalExecutionPolicy {
    bool preferVulkan = true;
    std::uint64_t memoryBudgetBytes = 512ULL * 1024ULL * 1024ULL;
};
struct TemporalCapabilities {
    bool vulkanFusion = false;
};
struct TemporalRequest {
    std::optional<imaging::FrameId> reference;
    // Existing scene contract. This does not acquire temporal policy/capability fields.
    reference::ReconstructionConfig reconstruction{};
};
struct TemporalStage {
    graph::TemporalOperation operation;
    TemporalBackend backend;
};
class TemporalExecutionPlan {
public:
    [[nodiscard]] const imaging::RawBurst& burst() const noexcept { return burst_; }
    [[nodiscard]] const TemporalRequest& request() const noexcept { return request_; }
    [[nodiscard]] const reference::TemporalPolicy& policy() const noexcept { return policy_; }
    [[nodiscard]] std::span<const TemporalStage> stages() const noexcept { return stages_; }
    [[nodiscard]] std::uint64_t workingSetBound() const noexcept { return workingSetBound_; }
    [[nodiscard]] TemporalBackend fusionBackend() const noexcept { return stages_[3].backend; }
    [[nodiscard]] TemporalFallback fallback() const noexcept { return fallback_; }
    [[nodiscard]] constexpr std::uint32_t version() const noexcept { return graph::temporalSchemaVersion; }
private:
    imaging::RawBurst burst_;
    TemporalRequest request_;
    reference::TemporalPolicy policy_;
    std::vector<TemporalStage> stages_;
    std::uint64_t workingSetBound_ = 0;
    TemporalFallback fallback_ = TemporalFallback::None;
    friend TemporalExecutionPlan compileTemporalPlan(const imaging::RawBurst&, const TemporalRequest&,
        const reference::TemporalPolicy&, const TemporalExecutionPolicy&, const TemporalCapabilities&);
};
struct TemporalFrameTrace {
    imaging::FrameId frame{};
    std::array<float, 4> radiometricScale{};
    float radiometricConfidence = 0;
    bool gainEstimated = false, noiseEstimated = false;
    reference::AlignmentField alignment;
};
struct TemporalExecutionTrace {
    std::uint32_t schemaVersion = graph::temporalSchemaVersion;
    imaging::BurstId burst{};
    reference::ReferenceSelection selection;
    TemporalBackend fusionBackend = TemporalBackend::Reference;
    TemporalFallback fallback = TemporalFallback::None;
    std::uint64_t workingSetBoundBytes = 0;
    std::uint32_t concurrentSourceFrames = 1U;
    std::vector<TemporalFrameTrace> frames;
    double processingMilliseconds = 0;
};
struct TemporalProgress {
    graph::TemporalOperation stage;
    std::size_t completedFrames = 0, totalFrames = 0;
};
class TemporalCancelled final : public std::runtime_error {
public:
    TemporalCancelled() : std::runtime_error("temporal execution cancelled") {}
};
struct TemporalExecutionControl {
    // Runs synchronously on the execution thread. False cancels at a stage boundary.
    std::function<bool(const TemporalProgress&)> continueExecution;
};
struct TemporalResult {
    imaging::SceneFrame scene;
    reference::FusedRaw fused;
    TemporalExecutionTrace trace;
};
[[nodiscard]] TemporalExecutionPlan compileTemporalPlan(const imaging::RawBurst& burst,
    const TemporalRequest& request, const reference::TemporalPolicy& policy,
    const TemporalExecutionPolicy& execution, const TemporalCapabilities& capabilities);
[[nodiscard]] TemporalResult executeTemporalPlan(const TemporalExecutionPlan& plan,
                                                const HostRawBindings& bindings, const TemporalExecutionControl& control = {});
// Default multi-frame entry point. A capability probe is an execution concern.
[[nodiscard]] TemporalResult reconstructRawBurst(const imaging::RawBurst& burst,
    const HostRawBindings& bindings, const TemporalRequest& request = {},
    const reference::TemporalPolicy& policy = {}, const TemporalExecutionPolicy& execution = {},
    const TemporalExecutionControl& control = {});
}  // namespace latent::runtime
