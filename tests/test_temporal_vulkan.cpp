#include "TemporalFixture.h"
#include "latent/vulkan/TemporalFusion.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
using namespace latent;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::vector<float> errors;
void compare(std::span<const float> cpu, std::span<const float> gpu, float absolute, float relative) {
    check(cpu.size() == gpu.size(), "differential extent mismatch");
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        check(std::isfinite(gpu[i]) && std::isfinite(cpu[i]), "non-finite differential result");
        const float error = std::abs(cpu[i] - gpu[i]);
        check(error <= absolute + relative * std::abs(cpu[i]), "temporal Vulkan error budget exceeded");
        errors.push_back(error);
    }
}
void compareContribution(const imaging::FrameContribution& cpu, const imaging::FrameContribution& gpu) {
    imaging::ImageLineage a{}, b{}; a.contributions.push_back(cpu); b.contributions.push_back(gpu);
    check(imaging::sameLineage(std::make_shared<imaging::ImageLineage>(a),
        std::make_shared<imaging::ImageLineage>(b)), "Vulkan regional rejection/support mismatch");
}
void fixture(imaging::CfaPattern cfa, imaging::Extent extent, unsigned scenario) {
    auto rf = test::frame(1, extent, cfa, 0, 0, 0.02F);
    auto sf = test::frame(2, extent, cfa, 0, 0, 0.02F, 0.5F);
    auto ref = reference::normalizeTemporalRaw(runtime::viewRawFrame(rf), rf, {}, false);
    auto source = reference::normalizeTemporalRaw(runtime::viewRawFrame(sf), rf, {}, false);
    auto identity = reference::alignTemporalRaw(ref, ref, imaging::FrameId{1}, imaging::FrameId{1}, {});
    auto field = identity; field.source = imaging::FrameId{2};
    for (auto& tile : field.tiles) { tile.dx = 0.75F; tile.dy = -0.25F; tile.confidence = 0.9F; }
    if (scenario == 1U) {
        for (auto& sample : ref.samples) sample.value -= 0.5F;
        for (auto& sample : source.samples) sample.value -= 0.5F;
    } else if (scenario == 2U) {
        for (auto& sample : source.samples) { sample.value = 1.2F; sample.usable = 0; }
    } else if (scenario == 3U) {
        for (auto& sample : source.samples) sample.value += 0.6F;
    } else if (scenario == 4U) {
        for (auto& tile : field.tiles) tile.confidence = 0;
    } else if (scenario == 5U) {
        for (auto& sample : ref.samples) { sample.usable = 0; sample.value = 1.25F; }
        for (auto& sample : source.samples) { sample.usable = 0; sample.value = 1.5F; }
    } else if (scenario == 6U) {
        source.samples[0].value = std::numeric_limits<float>::quiet_NaN();
        source.samples[4].usable = 0; source.samples[4].invalidReason = 4;
        field.tiles[0].dx = field.tiles[0].dy = 0;
    }
    auto cpu = reference::makeReferenceFusionSession(ref, {});
    auto gpu = vulkan::makeTemporalFusionSession(ref, {});
    compareContribution(cpu->add(ref, identity, true), gpu->add(ref, identity, true));
    compareContribution(cpu->add(source, field, false), gpu->add(source, field, false));
    const auto ca = cpu->finish(), ga = gpu->finish();
    const auto a = reference::finishTemporalFusion(ref, ca, {}), b = reference::finishTemporalFusion(ref, ga, {});
    // FP32 storage/accumulation, same interpolation. Allow division/order/driver
    // rounding, not sign changes or display clipping. No FP16 path is used.
    compare(a.sensor.samples, b.sensor.samples, 2e-6F, 2e-5F);
    compare(a.uncertainty.marginalVariance, b.uncertainty.marginalVariance, 1e-8F, 1e-4F);
    compare(a.uncertainty.effectiveSampleCount, b.uncertainty.effectiveSampleCount, 2e-5F, 2e-5F);
    compare(a.uncertainty.alignmentConfidence, b.uncertainty.alignmentConfidence, 1e-6F, 1e-6F);
    compare(a.uncertainty.robustnessConfidence, b.uncertainty.robustnessConfidence, 1e-6F, 2e-5F);
    for (std::size_t i = 0; i < a.sensor.samples.size(); ++i) {
        if (std::abs(a.sensor.samples[i]) > 1e-6F) check(std::signbit(a.sensor.samples[i]) == std::signbit(b.sensor.samples[i]), "Vulkan sign regression");
        check(b.uncertainty.marginalVariance[i] >= 0, "negative variance");
    }
}
void lifetime() {
    auto raw = test::frame(1, {17,15});
    auto ref = reference::normalizeTemporalRaw(runtime::viewRawFrame(raw), raw, {}, false);
    const auto field = reference::alignTemporalRaw(ref, ref, imaging::FrameId{1}, imaging::FrameId{1}, {});
    auto cpu = reference::makeReferenceFusionSession(ref, {});
    auto gpu = vulkan::makeTemporalFusionSession(ref, {});
    for (unsigned repeat = 0; repeat < 320U; ++repeat) {
        compareContribution(cpu->add(ref, field, true), gpu->add(ref, field, true));
    }
    const auto ca = cpu->finish(), ga = gpu->finish();
    const auto a = reference::finishTemporalFusion(ref, ca, {}), b = reference::finishTemporalFusion(ref, ga, {});
    compare(a.sensor.samples, b.sensor.samples, 2e-6F, 2e-5F);
    compare(a.uncertainty.effectiveSampleCount, b.uncertainty.effectiveSampleCount, 1e-4F, 2e-5F);
    check(b.uncertainty.effectiveSampleCount[0] > 319.9F, "progressive support lost across dispatches");
}
void wholePipeline() {
    std::vector<imaging::RawFrame> frames{test::frame(1), test::frame(2, {97,81}, imaging::CfaPattern::RGGB,2,-2,0.002F)};
    const auto burst = imaging::describeRawBurst(imaging::BurstId{3}, imaging::CaptureSequenceId{4}, imaging::CalibrationId{5}, frames);
    const auto bindings = runtime::bindReferenceFrames(frames);
    runtime::TemporalRequest request{}; request.reference = imaging::FrameId{1};
    runtime::TemporalExecutionPolicy execution{}; execution.preferVulkan = false;
    const auto cpu = runtime::reconstructRawBurst(burst, bindings, request, {}, execution);
    execution.preferVulkan = true;
    const auto gpu = runtime::reconstructRawBurst(burst, bindings, request, {}, execution);
    check(gpu.trace.fusionBackend == runtime::TemporalBackend::Vulkan, "default multi-frame path silently skipped Vulkan");
    compare(cpu.scene.image.rgb, gpu.scene.image.rgb, 5e-6F, 5e-5F);
    check(imaging::sameLineage(cpu.scene.lineage, gpu.scene.lineage), "backend changed semantic lineage");
}
}
int main() {
    try {
        std::string detail;
        if (!latent::vulkan::temporalFusionAvailable({13,11}, &detail)) {
            if (std::getenv("LATENT_REQUIRE_VULKAN")) throw std::runtime_error(detail);
            std::cout << "SKIP Vulkan temporal execution: " << detail << '\n'; return 0;
        }
        for (auto cfa : {latent::imaging::CfaPattern::RGGB, latent::imaging::CfaPattern::GRBG,
                         latent::imaging::CfaPattern::GBRG, latent::imaging::CfaPattern::BGGR}) {
            for (auto extent : {latent::imaging::Extent{13,11}, latent::imaging::Extent{66,47}})
                for (unsigned scenario = 0; scenario < 7; ++scenario) fixture(cfa, extent, scenario);
        }
        lifetime(); wholePipeline();
        std::sort(errors.begin(), errors.end());
        double mean = 0; for (float error : errors) mean += error;
        std::cout << "temporal Vulkan differential samples=" << errors.size() << " max=" << errors.back()
                  << " p95=" << errors[errors.size()*95U/100U] << " mean=" << mean/static_cast<double>(errors.size())
                  << " lifetime_dispatches=320; all properties passed\n";
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
