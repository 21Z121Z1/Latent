#include "latent/runtime/TemporalPipeline.h"
#ifdef LATENT_ENABLE_VULKAN_RUNTIME
#include "latent/vulkan/TemporalFusion.h"
#endif
#include <chrono>
#include <limits>
#include <stdexcept>

namespace latent::runtime {
std::uint64_t temporalWorkingSetBound(imaging::Extent extent, std::size_t members,
    const reference::TemporalPolicy& policy, TemporalBackend backend) {
    reference::validateTemporalPolicy(policy);
    if (extent.width < 2U || extent.height < 2U || members == 0U)
        throw std::invalid_argument("invalid temporal admission extent or membership");
    if (backend != TemporalBackend::Reference && backend != TemporalBackend::Vulkan)
        throw std::invalid_argument("invalid temporal admission backend");
    const auto n = extent.pixelCount();
    const auto tiles = ((static_cast<std::uint64_t>(extent.width) + policy.tileSize - 1U) / policy.tileSize) *
        ((static_cast<std::uint64_t>(extent.height) + policy.tileSize - 1U) / policy.tileSize);
    const std::uint64_t bytesPerPixel = backend == TemporalBackend::Vulkan ? 160U : 112U;
    // Regional lineage, motion, geometry evidence and streaming scratch reserve.
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (n > maximum / bytesPerPixel || tiles > maximum / 160U / members)
        throw std::invalid_argument("temporal working-set arithmetic overflow");
    const auto imageBytes = n * bytesPerPixel, traceBytes = tiles * 160U * members;
    if (traceBytes > maximum - imageBytes) throw std::invalid_argument("temporal working-set arithmetic overflow");
    return imageBytes + traceBytes;
}
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
    const bool vk = execution.preferVulkan && capabilities.vulkanFusion;
    plan.workingSetBound_ = temporalWorkingSetBound(burst.extent, burst.members.size(), policy,
        vk ? TemporalBackend::Vulkan : TemporalBackend::Reference);
    if (plan.workingSetBound_ > execution.memoryBudgetBytes)
        throw std::invalid_argument("temporal working set and trace exceed execution memory budget");
    for (const auto& schema : graph::temporalSchemas) {
        plan.stages_.push_back({schema.operation,
            vk && schema.operation == graph::TemporalOperation::FuseRaw ? TemporalBackend::Vulkan : TemporalBackend::Reference});
    }
    if (execution.preferVulkan && !capabilities.vulkanFusion) plan.fallback_ = TemporalFallback::VulkanUnavailable;
    return plan;
}

TemporalResult executeTemporalPlan(const TemporalExecutionPlan& plan, const HostRawBindings& bindings, const TemporalExecutionControl& control) {
    const auto started = std::chrono::steady_clock::now();
    const auto& burst = plan.burst(); const auto& policy = plan.policy();
    const auto valid = bindings.validate(burst);
    if (!valid.valid) throw std::invalid_argument(valid.message);
    std::size_t completed = 0;
    const auto checkpoint = [&](graph::TemporalOperation stage) {
        if (control.continueExecution && !control.continueExecution({stage, completed, burst.members.size()}))
            throw TemporalCancelled{};
    };
    checkpoint(graph::TemporalOperation::SelectReference);
    TemporalResult result{};
    auto& trace = result.trace;
    trace.burst = burst.id; trace.fusionBackend = plan.fusionBackend(); trace.fallback = plan.fallback();
    trace.workingSetBoundBytes = plan.workingSetBound();
    trace.selection = reference::selectBurstReference(burst, bindings, policy, plan.request().reference);
    const auto refView = bindings.view(burst, trace.selection.frame);
    checkpoint(graph::TemporalOperation::NormalizeFrame);
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
        checkpoint(graph::TemporalOperation::NormalizeFrame);
        auto source = isReference ? reference::NormalizedRaw{} : reference::normalizeTemporalRaw(
            bindings.view(burst, member.id), *refView.metadata, policy, plan.request().reconstruction.applyLensShading);
        const auto& input = isReference ? ref : source;
        checkpoint(graph::TemporalOperation::AlignFrame);
        auto field = reference::alignTemporalRaw(ref, input, trace.selection.frame, member.id, policy);
        checkpoint(graph::TemporalOperation::FuseRaw);
        lineage->contributions.push_back(session->add(input, field, isReference));
        trace.frames.push_back({member.id, input.radiometricScale, input.radiometricConfidence,
            input.gainEstimated, input.noiseEstimated, std::move(field)});
        ++completed;
    }
    checkpoint(graph::TemporalOperation::ReconstructScene);
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
    const TemporalExecutionPolicy& execution, const TemporalExecutionControl& control) {
    TemporalCapabilities capabilities{};
#ifdef LATENT_ENABLE_VULKAN_RUNTIME
    if (execution.preferVulkan) capabilities.vulkanFusion = vulkan::temporalFusionAvailable(burst.extent);
#endif
    return executeTemporalPlan(compileTemporalPlan(burst, request, policy, execution, capabilities), bindings, control);
}
}  // namespace latent::runtime
