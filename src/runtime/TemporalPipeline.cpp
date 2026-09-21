#include "latent/runtime/TemporalPipeline.h"
#ifdef LATENT_ENABLE_VULKAN_RUNTIME
#include "latent/vulkan/TemporalFusion.h"
#endif
#include <chrono>
#include <stdexcept>

namespace latent::runtime {
TemporalExecutionPlan compileTemporalPlan(const imaging::RawBurst& burst, const TemporalRequest& request,
    const reference::TemporalPolicy& policy, const TemporalExecutionPolicy& execution,
    const TemporalCapabilities& capabilities) {
    const auto valid = imaging::validateRawBurst(burst);
    if (!valid.valid()) throw std::invalid_argument(valid.message);
    reference::validateTemporalPolicy(policy);
    if (request.reconstruction.defectCorrection != reference::DefectCorrectionMode::Disabled) {
        throw std::invalid_argument("temporal path rejects defective evidence; corrected-noise propagation is not implemented");
    }
    TemporalExecutionPlan plan{};
    plan.burst_ = burst; plan.request_ = request; plan.policy_ = policy;
    // Conservative payload bound includes source/ref FP32, accumulators,
    // pyramids, result/scene, and O(N*tiles) trace/lineage. Borrowed RAW storage
    // and driver allocator overhead are excluded and must be budgeted by capture.
    const auto n = burst.extent.pixelCount();
    const auto tiles = ((static_cast<std::uint64_t>(burst.extent.width) + policy.tileSize - 1U) / policy.tileSize) *
        ((static_cast<std::uint64_t>(burst.extent.height) + policy.tileSize - 1U) / policy.tileSize);
    const bool vk = execution.preferVulkan && capabilities.vulkanFusion;
    const std::uint64_t bytesPerPixel = vk ? 160U : 112U;
    if (n > execution.memoryBudgetBytes / bytesPerPixel ||
        tiles > execution.memoryBudgetBytes / 128U / burst.members.size()) {
        throw std::invalid_argument("temporal working set exceeds execution memory budget");
    }
    plan.workingSetBound_ = n * bytesPerPixel + tiles * 128U * burst.members.size();
    if (plan.workingSetBound_ > execution.memoryBudgetBytes) {
        throw std::invalid_argument("temporal working set and trace exceed execution memory budget");
    }
    for (const auto& schema : graph::temporalSchemas) {
        plan.stages_.push_back({schema.operation,
            vk && schema.operation == graph::TemporalOperation::FuseRaw ? TemporalBackend::Vulkan : TemporalBackend::Reference});
    }
    if (execution.preferVulkan && !capabilities.vulkanFusion) plan.fallback_ = TemporalFallback::VulkanUnavailable;
    return plan;
}

TemporalResult executeTemporalPlan(const TemporalExecutionPlan& plan, const HostRawBindings& bindings) {
    const auto started = std::chrono::steady_clock::now();
    const auto& burst = plan.burst(); const auto& policy = plan.policy();
    const auto valid = bindings.validate(burst);
    if (!valid.valid) throw std::invalid_argument(valid.message);
    TemporalResult result{};
    auto& trace = result.trace;
    trace.burst = burst.id; trace.fusionBackend = plan.fusionBackend(); trace.fallback = plan.fallback();
    trace.workingSetBoundBytes = plan.workingSetBound();
    trace.selection = reference::selectBurstReference(burst, bindings, policy, plan.request().reference);
    const auto refView = bindings.view(burst, trace.selection.frame);
    const auto ref = reference::normalizeTemporalRaw(refView, *refView.metadata, policy,
                                                    plan.request().reconstruction.applyLensShading);
    std::unique_ptr<reference::TemporalFusionSession> session;
    if (plan.fusionBackend() == TemporalBackend::Reference) session = reference::makeReferenceFusionSession(ref, policy);
    else {
#ifdef LATENT_ENABLE_VULKAN_RUNTIME
        session = vulkan::makeTemporalFusionSession(ref, policy);
#else
        throw std::runtime_error("requested temporal lowering unavailable in this build");
#endif
    }
    auto lineage = std::make_shared<imaging::ImageLineage>();
    lineage->burst = burst.id; lineage->sequence = burst.sequence; lineage->calibration = burst.calibration;
    lineage->reference = trace.selection.frame;
    for (const auto& member : burst.members) lineage->inputs.push_back(member.id);
    // Fixed capture order is part of deterministic accumulation. One current
    // source is released at the end of each iteration; no N-frame FP32 cache.
    for (const auto& member : burst.members) {
        const bool isReference = member.id == trace.selection.frame;
        auto source = isReference ? reference::NormalizedRaw{} : reference::normalizeTemporalRaw(
            bindings.view(burst, member.id), *refView.metadata, policy, plan.request().reconstruction.applyLensShading);
        const auto& input = isReference ? ref : source;
        auto field = reference::alignTemporalRaw(ref, input, trace.selection.frame, member.id, policy);
        lineage->contributions.push_back(session->add(input, field, isReference));
        trace.frames.push_back({member.id, input.radiometricScale, input.radiometricConfidence,
            input.gainEstimated, input.noiseEstimated, std::move(field)});
    }
    const auto accumulation = session->finish();
    session.reset(); // Release device resources before allocating scene/render intermediates.
    result.fused = reference::finishTemporalFusion(ref, accumulation, lineage);
    auto finish = plan.request().reconstruction;
    finish.applyLensShading = false;
    result.scene = reference::reconstructSensorLinear(result.fused.sensor, finish);
    result.scene.lineage = lineage;
    result.scene.sourceRawId = burst.members.size() == 1U ? lineage->reference.value : 0U;
    // A temporal conditional variance is not silently coerced to shot*x+read.
    trace.processingMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return result;
}
TemporalResult reconstructRawBurst(const imaging::RawBurst& burst, const HostRawBindings& bindings,
    const TemporalRequest& request, const reference::TemporalPolicy& policy,
    const TemporalExecutionPolicy& execution) {
    TemporalCapabilities capabilities{};
#ifdef LATENT_ENABLE_VULKAN_RUNTIME
    if (execution.preferVulkan) capabilities.vulkanFusion = vulkan::temporalFusionAvailable(burst.extent);
#endif
    return executeTemporalPlan(compileTemporalPlan(burst, request, policy, execution, capabilities), bindings);
}
}  // namespace latent::runtime
