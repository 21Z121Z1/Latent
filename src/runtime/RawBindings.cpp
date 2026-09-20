#include "latent/runtime/RawBindings.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace latent::runtime {

RawFrameView viewRawFrame(const imaging::RawFrame& raw) {
    return {imaging::FrameId{raw.id}, &raw,
            {raw.storage.extent, raw.storage.rowStridePixels, raw.storage.pixels}};
}

imaging::RawValidation validateRawView(const RawFrameView& view) {
    if (view.metadata == nullptr) return {false, "RAW view requires metadata"};
    const auto check = imaging::validateRawMetadata(*view.metadata);
    if (!check.valid) return check;
    const auto& storage = view.storage;
    if (storage.extent.width == 0U || storage.extent.height == 0U ||
        storage.rowStridePixels < storage.extent.width) {
        return {false, "RAW view has an invalid extent or stride"};
    }
    const auto required = static_cast<std::uint64_t>(storage.rowStridePixels) *
                          (storage.extent.height - 1U) + storage.extent.width;
    if (storage.pixels.size() < required) return {false, "RAW view storage is too short"};
    return {};
}

HostRawBindings::HostRawBindings(std::vector<HostRawBinding> bindings) : bindings_(std::move(bindings)) {}

imaging::RawValidation HostRawBindings::validate(const imaging::RawBurst& burst) const {
    const auto semantic = imaging::validateRawBurst(burst);
    if (!semantic.valid()) return {false, semantic.message};
    if (bindings_.size() != burst.members.size()) return {false, "binding count differs from burst membership"};
    std::set<imaging::FrameId> seen;
    for (const auto& binding : bindings_) {
        if (!seen.insert(binding.frame).second) return {false, "duplicate physical frame binding"};
        const auto member = std::find_if(burst.members.begin(), burst.members.end(),
            [&](const auto& value) { return value.id == binding.frame; });
        if (member == burst.members.end()) return {false, "physical binding is not a burst member"};
        if (binding.storage.extent.width != burst.extent.width || binding.storage.extent.height != burst.extent.height) {
            return {false, "physical binding extent differs from the semantic burst"};
        }
        const auto check = validateRawView({member->id, &member->observations, binding.storage});
        if (!check.valid) return check;
    }
    return {};
}

RawFrameView HostRawBindings::view(const imaging::RawBurst& burst, imaging::FrameId id) const {
    const auto member = std::find_if(burst.members.begin(), burst.members.end(),
        [&](const auto& value) { return value.id == id; });
    const auto binding = std::find_if(bindings_.begin(), bindings_.end(),
        [&](const auto& value) { return value.frame == id; });
    if (member == burst.members.end() || binding == bindings_.end()) {
        throw std::invalid_argument("frame has no matching semantic member and physical binding");
    }
    if (binding->storage.extent.width != burst.extent.width ||
        binding->storage.extent.height != burst.extent.height) {
        throw std::invalid_argument("physical binding extent differs from the semantic burst");
    }
    RawFrameView result{id, &member->observations, binding->storage};
    const auto check = validateRawView(result);
    if (!check.valid) throw std::invalid_argument(check.message);
    return result;
}

HostRawBindings bindReferenceFrames(std::span<const imaging::RawFrame> frames) {
    std::vector<HostRawBinding> bindings;
    bindings.reserve(frames.size());
    for (const auto& frame : frames) {
        bindings.push_back({imaging::FrameId{frame.id}, viewRawFrame(frame).storage});
    }
    return HostRawBindings(std::move(bindings));
}

}  // namespace latent::runtime
