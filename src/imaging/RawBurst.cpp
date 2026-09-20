#include "latent/imaging/RawBurst.h"

#include <limits>
#include <set>
#include <stdexcept>

namespace latent::imaging {

BurstValidation validateRawBurst(const RawBurst& burst) {
    if (!burst.id.valid() || !burst.sequence.valid() || !burst.calibration.valid()) {
        return {BurstValidationCode::MissingIdentity, {}, "burst, sequence, and calibration IDs must be non-zero"};
    }
    if (burst.members.empty()) return {BurstValidationCode::EmptyBurst, {}, "burst must contain a frame"};
    if (burst.extent.width < 2U || burst.extent.height < 2U ||
        burst.extent.pixelCount() > std::numeric_limits<std::size_t>::max() / (sizeof(float) * 8U)) {
        return {BurstValidationCode::InvalidExtent, {}, "burst requires an addressable Bayer extent"};
    }
    const auto& first = burst.members.front().observations;
    if (first.cameraId.empty() || first.sensorMode.empty()) {
        return {BurstValidationCode::CalibrationMismatch, burst.members.front().id, "camera and sensor mode must be identified"};
    }
    std::set<FrameId> ids;
    std::int64_t previous = 0;
    for (const auto& member : burst.members) {
        if (!member.id.valid() || !ids.insert(member.id).second) {
            return {BurstValidationCode::DuplicateFrame, member.id, "frame IDs must be non-zero and unique"};
        }
        const auto& metadata = member.observations;
        if (metadata.sensorTimestampNs <= previous) {
            return {BurstValidationCode::InvalidTimestamp, member.id, "sensor timestamps must be positive and strictly increasing"};
        }
        previous = metadata.sensorTimestampNs;
        if (metadata.cameraId != first.cameraId || metadata.sensorMode != first.sensorMode || metadata.cfa != first.cfa) {
            return {BurstValidationCode::CalibrationMismatch, member.id, "first-generation burst members must share camera, sensor mode, and CFA"};
        }
        const auto check = validateRawMetadata(metadata);
        if (!check.valid) return {BurstValidationCode::InvalidMetadata, member.id, check.message};
    }
    return {};
}

RawBurst describeRawBurst(
    BurstId id, CaptureSequenceId sequence, CalibrationId calibration,
    std::span<const RawFrame> frames) {
    RawBurst burst{id, sequence, calibration, {}, {}};
    if (!frames.empty()) burst.extent = frames.front().storage.extent;
    burst.members.reserve(frames.size());
    for (const auto& frame : frames) {
        if (frame.storage.extent.width != burst.extent.width || frame.storage.extent.height != burst.extent.height) {
            throw std::invalid_argument("burst member extents differ");
        }
        burst.members.push_back({FrameId{frame.id}, static_cast<const RawFrameMetadata&>(frame)});
    }
    const auto check = validateRawBurst(burst);
    if (!check.valid()) throw std::invalid_argument(check.message);
    return burst;
}

}  // namespace latent::imaging
