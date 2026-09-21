#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace latent::runtime {

// Measurements are not policy. ISO is a request/observation, never calibrated gain.
struct CaptureObservations {
    std::int64_t exposureTimeNs = 0;
    std::int64_t frameDurationNs = 0;
    std::uint32_t sensitivityIso = 0;
    bool aeConverged = false;
    std::optional<float> normalizedMidtoneVariance;
    std::optional<float> angularSpeedRadiansPerSecond;
    bool gyroTimestampComparable = false;
};
struct CaptureIntent {
    float targetStandardDeviation = 0.006F;
    std::int64_t latencyBudgetNs = 1500000000LL;
    std::int64_t totalIntegrationBudgetNs = 1000000000LL;
    float maximumAngularTravelRadians = 0.0015F;
    float highlightExposureEv = -0.5F;
    std::uint32_t maximumFrames = 8;
};
struct CaptureCapabilities {
    bool raw = false;
    bool manualSensor = false;
    bool aeLock = false;
    std::int64_t minimumExposureNs = 0, maximumExposureNs = 0;
    std::int64_t minimumRawFrameDurationNs = 0, maximumFrameDurationNs = 0;
    std::uint32_t minimumIso = 0, maximumIso = 0;
    // This budget is AFTER reserving reconstruction, UI and driver headroom.
    std::uint64_t retainedRawBudgetBytes = 0, bytesPerRawFrame = 0;
    std::uint32_t maximumRetainedFrames = 0;
};
enum class ExposureControl : std::uint8_t { Manual, LockedAutoExposure };
struct FrameExposurePlan {
    std::int64_t exposureTimeNs = 0, frameDurationNs = 0;
    std::uint32_t sensitivityIso = 0;
};
struct CapturePlan {
    ExposureControl control = ExposureControl::Manual;
    std::vector<FrameExposurePlan> frames;
    bool noiseEstimated = false;
    bool motionConstraintUsed = false;
    bool qualityLimited = false;
    std::string reason;
};

// Independent constant-exposure policy. No adaptive bracket or vendor schedule.
[[nodiscard]] CapturePlan compileCapturePlan(const CaptureObservations& observations,
    const CaptureIntent& intent, const CaptureCapabilities& capabilities);

}  // namespace latent::runtime
