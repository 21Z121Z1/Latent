#pragma once
#include "latent/reference/TemporalReconstruct.h"
#include <string>

namespace latent::vulkan {
// Physical lowering only. The caller retains normalized reference ownership
// until the session is destroyed. No backend handle enters a semantic frame.
[[nodiscard]] bool temporalFusionAvailable(imaging::Extent extent, std::string* detail = nullptr);
[[nodiscard]] std::unique_ptr<reference::TemporalFusionSession> makeTemporalFusionSession(
    const reference::NormalizedRaw& reference, const reference::TemporalPolicy& policy);
}  // namespace latent::vulkan
