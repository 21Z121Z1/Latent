#pragma once

#include "latent/imaging/RawBurst.h"

#include <span>
#include <vector>

namespace latent::runtime {

struct RawBufferView {
    imaging::Extent extent{};
    std::uint32_t rowStridePixels = 0;
    std::span<const std::uint16_t> pixels;
};

struct RawFrameView {
    imaging::FrameId id{};
    const imaging::RawFrameMetadata* metadata = nullptr;
    RawBufferView storage{};
};

struct HostRawBinding {
    imaging::FrameId frame{};
    RawBufferView storage{};
};

// Borrowed bindings. The caller keeps payloads alive until execution returns.
// This type is intentionally outside RawBurst and contains no semantic policy.
class HostRawBindings {
public:
    explicit HostRawBindings(std::vector<HostRawBinding> bindings);
    [[nodiscard]] imaging::RawValidation validate(const imaging::RawBurst& burst) const;
    [[nodiscard]] RawFrameView view(const imaging::RawBurst& burst, imaging::FrameId id) const;
private:
    std::vector<HostRawBinding> bindings_;
};

[[nodiscard]] RawFrameView viewRawFrame(const imaging::RawFrame& raw);
[[nodiscard]] imaging::RawValidation validateRawView(const RawFrameView& view);
[[nodiscard]] HostRawBindings bindReferenceFrames(std::span<const imaging::RawFrame> frames);

}  // namespace latent::runtime
