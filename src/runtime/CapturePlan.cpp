#include "latent/runtime/CapturePlan.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace latent::runtime {
CapturePlan compileCapturePlan(const CaptureObservations& o, const CaptureIntent& i,
    const CaptureCapabilities& c) {
    if (!c.raw) throw std::invalid_argument("Night RAW pipeline unsupported on this camera/device");
    if (!o.aeConverged) throw std::invalid_argument("AE must converge before the constant-exposure burst");
    if (!c.manualSensor && !c.aeLock) throw std::invalid_argument("constant exposure requires manual sensor or AE lock");
    if (o.exposureTimeNs <= 0 || o.frameDurationNs <= 0 || o.sensitivityIso == 0 ||
        c.minimumExposureNs <= 0 || c.maximumExposureNs < c.minimumExposureNs ||
        c.minimumRawFrameDurationNs <= 0 || c.maximumFrameDurationNs < c.minimumRawFrameDurationNs ||
        c.minimumIso == 0 || c.maximumIso < c.minimumIso || c.bytesPerRawFrame == 0 ||
        c.maximumRetainedFrames == 0 || i.maximumFrames == 0 || i.maximumFrames > 64 ||
        i.latencyBudgetNs <= 0 || i.totalIntegrationBudgetNs <= 0 ||
        !std::isfinite(i.targetStandardDeviation) || i.targetStandardDeviation <= 0 ||
        !std::isfinite(i.maximumAngularTravelRadians) || i.maximumAngularTravelRadians <= 0 ||
        !std::isfinite(i.highlightExposureEv) || i.highlightExposureEv < -8 || i.highlightExposureEv > 0) {
        throw std::invalid_argument("invalid capture observations, intent or capabilities");
    }
    if (o.normalizedMidtoneVariance && (!std::isfinite(*o.normalizedMidtoneVariance) || *o.normalizedMidtoneVariance < 0))
        throw std::invalid_argument("invalid observed noise variance");
    if (o.angularSpeedRadiansPerSecond && (!std::isfinite(*o.angularSpeedRadiansPerSecond) || *o.angularSpeedRadiansPerSecond < 0))
        throw std::invalid_argument("invalid angular speed observation");
    CapturePlan plan{};
    plan.control = c.manualSensor ? ExposureControl::Manual : ExposureControl::LockedAutoExposure;
    auto exposure = o.exposureTimeNs;
    if (c.manualSensor) {
        const double proposed = static_cast<double>(exposure) * std::exp2(static_cast<double>(i.highlightExposureEv));
        exposure = proposed >= static_cast<double>(c.maximumExposureNs) ? c.maximumExposureNs :
            static_cast<std::int64_t>(proposed);
        if (o.gyroTimestampComparable && o.angularSpeedRadiansPerSecond && *o.angularSpeedRadiansPerSecond > 0) {
            const double motionLimit = 1.0e9 * static_cast<double>(i.maximumAngularTravelRadians) /
                static_cast<double>(*o.angularSpeedRadiansPerSecond);
            if (motionLimit < static_cast<double>(exposure)) {
                exposure = static_cast<std::int64_t>(motionLimit);
                plan.motionConstraintUsed = true;
            }
        }
        exposure = std::clamp(exposure, c.minimumExposureNs, c.maximumExposureNs);
    } else if (exposure < c.minimumExposureNs || exposure > c.maximumExposureNs) {
        throw std::invalid_argument("locked AE exposure is outside reported sensor limits");
    }
    // Do not alter nominal gain to conceal reduced integration/highlight protection.
    const auto iso = c.manualSensor ? std::clamp(o.sensitivityIso, c.minimumIso, c.maximumIso) : o.sensitivityIso;
    if (iso < c.minimumIso || iso > c.maximumIso) throw std::invalid_argument("locked AE ISO outside reported limits");
    const auto duration = std::max({exposure, o.frameDurationNs, c.minimumRawFrameDurationNs});
    if (duration > c.maximumFrameDurationNs) throw std::invalid_argument("RAW cadence cannot satisfy sensor frame duration limits");
    const auto maxFrames = std::min<std::uint64_t>({i.maximumFrames, c.maximumRetainedFrames,
        c.retainedRawBudgetBytes / c.bytesPerRawFrame,
        static_cast<std::uint64_t>(i.latencyBudgetNs / duration),
        static_cast<std::uint64_t>(i.totalIntegrationBudgetNs / exposure)});
    if (maxFrames == 0) throw std::invalid_argument("capture budgets cannot hold even one RAW frame");
    plan.noiseEstimated = !o.normalizedMidtoneVariance.has_value();
    // Conservative policy prior, not a claimed sensor measurement. Scaling the
    // prior/observation back to AE coordinates assumes read-noise dominance.
    const double variance = o.normalizedMidtoneVariance.value_or(1.0e-4F);
    const double ratio = static_cast<double>(o.exposureTimeNs) / static_cast<double>(exposure);
    const double target = static_cast<double>(i.targetStandardDeviation);
    const double desired = std::max(1.0, std::ceil(variance * ratio * ratio / (target * target)));
    const auto count = static_cast<std::uint32_t>(std::min(desired, static_cast<double>(maxFrames)));
    plan.qualityLimited = desired > static_cast<double>(maxFrames);
    plan.frames.assign(count, FrameExposurePlan{exposure, duration, iso});
    plan.reason = c.manualSensor ? "constant manual exposure; highlight headroom; noise-target frame count" :
                                  "locked AE exposure; sensor controls remain observations";
    if (plan.noiseEstimated) plan.reason += "; noise prior, not measured";
    if (plan.motionConstraintUsed) plan.reason += "; timestamp-compatible gyro limits exposure";
    if (plan.qualityLimited) plan.reason += "; quality target limited by capture/resource budget";
    return plan;
}
}  // namespace latent::runtime
