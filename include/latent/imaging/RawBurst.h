#pragma once

#include "latent/imaging/Identity.h"
#include "latent/imaging/RawFrame.h"

#include <span>
#include <vector>

namespace latent::imaging {

struct RawBurstMember {
    FrameId id{};
    RawFrameMetadata observations{};
};

// An immutable-by-contract metadata snapshot, not a storage ownership model.
// The calibration ID asserts a common sensor coordinate system. It does not
// assert that ISO is a calibrated gain or that motion has been measured.
struct RawBurst {
    BurstId id{};
    CaptureSequenceId sequence{};
    CalibrationId calibration{};
    Extent extent{};
    std::vector<RawBurstMember> members;  // Strict capture timestamp order.
};

enum class BurstValidationCode : std::uint8_t {
    Valid,
    MissingIdentity,
    EmptyBurst,
    InvalidExtent,
    DuplicateFrame,
    InvalidTimestamp,
    CalibrationMismatch,
    InvalidMetadata,
};

struct BurstValidation {
    BurstValidationCode code = BurstValidationCode::Valid;
    FrameId frame{};
    std::string message;
    [[nodiscard]] bool valid() const noexcept { return code == BurstValidationCode::Valid; }
};

[[nodiscard]] BurstValidation validateRawBurst(const RawBurst& burst);

// Reference/fixture adapter. Only metadata is copied; pixel vectors stay with
// the caller. Production producers can construct RawBurst directly.
[[nodiscard]] RawBurst describeRawBurst(
    BurstId id, CaptureSequenceId sequence, CalibrationId calibration,
    std::span<const RawFrame> frames);

}  // namespace latent::imaging
