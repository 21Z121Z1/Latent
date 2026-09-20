#pragma once

#include <compare>
#include <cstdint>

namespace latent::imaging {

// IDs describe semantic identity. They are not pointers, allocation IDs, or hashes.
template <typename Tag>
struct SemanticId {
    std::uint64_t value = 0;
    constexpr SemanticId() = default;
    explicit constexpr SemanticId(std::uint64_t v) : value(v) {}
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0U; }
    auto operator<=>(const SemanticId&) const = default;
};

using FrameId = SemanticId<struct FrameIdentity>;
using BurstId = SemanticId<struct BurstIdentity>;
using CaptureSequenceId = SemanticId<struct CaptureSequenceIdentity>;
using CalibrationId = SemanticId<struct CalibrationIdentity>;

}  // namespace latent::imaging
