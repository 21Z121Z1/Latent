#include "latent/runtime/TemporalPipeline.h"
#include "latent/runtime/RawBindings.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <string_view>
#include <vector>

namespace {
using namespace latent;

imaging::RawFrame makeFrame(std::uint64_t id, imaging::Extent extent, float dx, float dy) {
    imaging::RawFrame raw{};
    raw.id = id;
    raw.sensorTimestampNs = static_cast<std::int64_t>(id) * 100000000LL;
    raw.cameraId = "benchmark";
    raw.sensorMode = "synthetic";
    raw.cfa = imaging::CfaPattern::RGGB;
    raw.exposureTimeNs = 10000000LL;
    raw.sensitivityIso = 100.0F;
    raw.staticBlack = {imaging::BlackLevel{{256.0F,256.0F,256.0F,256.0F}},
                       imaging::MetadataSource::StaticCharacteristic,
                       imaging::MetadataValidity::Valid, 1.0F};
    raw.staticWhite = {4096.0F, imaging::MetadataSource::StaticCharacteristic,
                       imaging::MetadataValidity::Valid, 1.0F};
    imaging::NoiseModel noise{};
    noise.coordinate = imaging::NoiseCoordinate::NormalizedBlackSubtracted;
    noise.read.fill(0.0001F);
    raw.noiseProfile = {noise, imaging::MetadataSource::MeasuredCalibration,
                        imaging::MetadataValidity::Valid, 1.0F};
    raw.exposureCalibration.source = imaging::MetadataSource::MeasuredCalibration;
    raw.exposureCalibration.gainUncertainty = 0.0F;
    raw.storage.extent = extent;
    raw.storage.rowStridePixels = extent.width;
    raw.storage.pixels.resize(extent.pixelCount());
    for (std::uint32_t y=0; y<extent.height; ++y) for (std::uint32_t x=0; x<extent.width; ++x) {
        const float px = static_cast<float>(x)-dx, py = static_cast<float>(y)-dy;
        const auto c = static_cast<float>(imaging::cfaChannelAt(raw.cfa,x,y));
        const float value = 0.28F + 0.07F*std::sin(0.031F*px+0.017F*py) +
            0.05F*std::cos(0.023F*px-0.029F*py) + 0.02F*c;
        const float code = std::clamp(std::round(256.0F + value*3840.0F),0.0F,4096.0F);
        raw.storage.pixels[static_cast<std::size_t>(y)*extent.width+x] = static_cast<std::uint16_t>(code);
    }
    return raw;
}

struct Measure {
    double medianMs = 0.0;
    runtime::TemporalExecutionTrace trace{};
    double checksum = 0.0;
};

Measure measure(const imaging::RawBurst& burst, const runtime::HostRawBindings& bindings, bool preferVulkan) {
    std::array<double,3> timings{};
    runtime::TemporalExecutionTrace trace{};
    double checksum = 0.0;
    for (std::size_t repeat=0; repeat<timings.size(); ++repeat) {
        runtime::TemporalRequest request{};
        request.reference = burst.members.front().id;
        runtime::TemporalExecutionPolicy execution{};
        execution.preferVulkan = preferVulkan;
        const auto begin = std::chrono::steady_clock::now();
        auto result = runtime::reconstructRawBurst(burst,bindings,request,{},execution);
        timings[repeat] = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        trace = result.trace;
        checksum += std::accumulate(result.fused.sensor.samples.begin(),result.fused.sensor.samples.end(),0.0);
    }
    std::sort(timings.begin(),timings.end());
    return {timings[timings.size()/2U],std::move(trace),checksum};
}

std::string_view backendName(runtime::TemporalBackend backend) {
    return backend == runtime::TemporalBackend::Vulkan ? "vulkan" : "reference";
}
}

int main() {
    try {
        const latent::imaging::Extent extent{257,193};
        std::vector<latent::imaging::RawFrame> frames;
        frames.push_back(makeFrame(1,extent,0.0F,0.0F));
        frames.push_back(makeFrame(2,extent,2.0F,-2.0F));
        frames.push_back(makeFrame(3,extent,-2.0F,2.0F));
        frames.push_back(makeFrame(4,extent,0.75F,-1.25F));
        const auto burst = latent::imaging::describeRawBurst(latent::imaging::BurstId{701},
            latent::imaging::CaptureSequenceId{702},latent::imaging::CalibrationId{703},frames);
        const auto bindings = latent::runtime::bindReferenceFrames(frames);
        const auto reference = measure(burst,bindings,false);
        const auto production = measure(burst,bindings,true);
        std::cout << "temporal_benchmark schema=1 width=" << extent.width << " height=" << extent.height
                  << " frames=" << frames.size() << " repetitions=3"
                  << " reference_median_ms=" << reference.medianMs
                  << " production_median_ms=" << production.medianMs
                  << " production_backend=" << backendName(production.trace.fusionBackend)
                  << " working_set_bound_bytes=" << production.trace.workingSetBoundBytes
                  << " concurrent_source_frames=" << production.trace.concurrentSourceFrames
                  << " fusion_additions=" << frames.size()
                  << " checksum=" << reference.checksum+production.checksum << '\n';
        return std::isfinite(reference.checksum+production.checksum) ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "benchmark failed: " << e.what() << '\n';
        return 1;
    }
}
