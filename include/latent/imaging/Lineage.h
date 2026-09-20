#pragma once

#include "latent/imaging/Identity.h"

#include <array>
#include <cstdint>
#include <cstddef>
#include <utility>
#include <memory>
#include <vector>

namespace latent::imaging {

enum class TemporalRejection : std::uint8_t {
    Border,
    Clipped,
    Alignment,
    Motion,
    Metadata,
    Count,
};

// A region reports counts, not a claim that every pixel had the same decision.
// This is a bounded regional audit, not N full-resolution confidence textures.
struct RegionSupport {
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    std::uint32_t accepted = 0;
    std::array<std::uint32_t, static_cast<std::size_t>(TemporalRejection::Count)> rejected{};
    bool operator==(const RegionSupport&) const = default;
};

struct FrameContribution {
    FrameId frame{};
    std::uint64_t acceptedSamples = 0;
    std::vector<RegionSupport> regions;
    bool operator==(const FrameContribution&) const = default;
};

struct ImageLineage {
    BurstId burst{};                 // Empty only for a legacy single-frame source.
    CaptureSequenceId sequence{};
    CalibrationId calibration{};
    std::vector<FrameId> inputs;     // Capture order, including rejected members.
    FrameId reference{};             // Output geometry and radiometric coordinate.
    std::vector<FrameContribution> contributions;
    bool operator==(const ImageLineage&) const = default;
};

using Lineage = std::shared_ptr<const ImageLineage>;

[[nodiscard]] inline bool sameLineage(const Lineage& a, const Lineage& b) {
    return a == b || (a && b && *a == *b);
}

[[nodiscard]] inline Lineage singleFrameLineage(FrameId frame) {
    if (!frame.valid()) return {};  // Preserve the legacy unknown-ID state.
    ImageLineage lineage{};
    lineage.inputs.push_back(frame);
    lineage.reference = frame;
    return std::make_shared<const ImageLineage>(std::move(lineage));
}

}  // namespace latent::imaging
