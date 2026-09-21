#pragma once
#include "latent/runtime/TemporalPipeline.h"
#include <algorithm>
#include <cmath>
#include <functional>

namespace latent::test {
inline float texture(float x, float y, std::size_t channel) {
    return 0.28F + 0.06F * std::sin(0.17F * x + 0.033F * y) +
        0.04F * std::cos(0.079F * x - 0.21F * y) +
        0.03F * std::sin(0.012F * x * y) + 0.035F * static_cast<float>(channel);
}
// Deterministic integer generator. Fixture pixel arrays are reference bindings,
// never a required burst storage format or an oracle output lookup table.
inline float uniformNoise(std::uint32_t& state) {
    state ^= state << 13U; state ^= state >> 17U; state ^= state << 5U;
    return (static_cast<float>(state & 65535U) / 65535.0F * 2.0F - 1.0F) * 1.73205080757F;
}
inline imaging::RawFrame frame(std::uint64_t id, imaging::Extent extent = {97, 81},
    imaging::CfaPattern cfa = imaging::CfaPattern::RGGB, float dx = 0, float dy = 0,
    float sigma = 0, float exposureRatio = 1,
    const std::function<float(float, float, std::size_t)>& signal = texture) {
    imaging::RawFrame raw{};
    raw.id = id; raw.sensorTimestampNs = static_cast<std::int64_t>(id) * 100000000LL;
    raw.cameraId = "synthetic"; raw.sensorMode = "fixture"; raw.cfa = cfa;
    raw.exposureTimeNs = static_cast<std::int64_t>(10000000.0F * exposureRatio);
    raw.sensitivityIso = 100.0F;
    raw.staticBlack = {imaging::BlackLevel{{256,256,256,256}}, imaging::MetadataSource::StaticCharacteristic, imaging::MetadataValidity::Valid, 1.0F};
    raw.staticWhite = {4096.0F, imaging::MetadataSource::StaticCharacteristic, imaging::MetadataValidity::Valid, 1.0F};
    imaging::NoiseModel noise{}; noise.coordinate = imaging::NoiseCoordinate::NormalizedBlackSubtracted;
    noise.read.fill(sigma * sigma);
    raw.noiseProfile = {noise, imaging::MetadataSource::MeasuredCalibration, imaging::MetadataValidity::Valid, 1.0F};
    raw.exposureCalibration.source = imaging::MetadataSource::MeasuredCalibration;
    raw.exposureCalibration.gainUncertainty = 0;
    raw.storage.extent = extent; raw.storage.rowStridePixels = extent.width + 3U;
    raw.storage.pixels.resize(static_cast<std::size_t>(raw.storage.rowStridePixels) * extent.height, 65535U);
    auto rng = static_cast<std::uint32_t>(id) * 7654321U + 1U;
    for (std::uint32_t y = 0; y < extent.height; ++y) for (std::uint32_t x = 0; x < extent.width; ++x) {
        const auto c = static_cast<std::size_t>(imaging::cfaChannelAt(cfa, x, y));
        const float v = signal(static_cast<float>(x) - dx, static_cast<float>(y) - dy, c) * exposureRatio + sigma * uniformNoise(rng);
        const float code = std::clamp(std::round(256.0F + v * 3840.0F), 0.0F, 65535.0F);
        raw.storage.pixels[static_cast<std::size_t>(y) * raw.storage.rowStridePixels + x] = static_cast<std::uint16_t>(code);
    }
    return raw;
}
inline runtime::TemporalResult run(std::vector<imaging::RawFrame>& frames,
    std::optional<imaging::FrameId> ref = imaging::FrameId{1}, reference::TemporalPolicy policy = {}) {
    const auto burst = imaging::describeRawBurst(imaging::BurstId{91}, imaging::CaptureSequenceId{17}, imaging::CalibrationId{2}, frames);
    runtime::TemporalRequest request{}; request.reference = ref;
    runtime::TemporalExecutionPolicy execution{}; execution.preferVulkan = false;
    return runtime::reconstructRawBurst(burst, runtime::bindReferenceFrames(frames), request, policy, execution);
}
}  // namespace latent::test
