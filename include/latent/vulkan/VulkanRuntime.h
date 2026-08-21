#pragma once

#include "latent/vulkan/DeviceCaps.h"

#include <string>

namespace latent::vulkan {

struct RuntimeAvailability {
    bool loaderAvailable = false;
    bool instanceCreated = false;
    bool deviceCreated = false;
    std::string detail;
};

// Thin RAII adapter over the Vulkan loader that populates the backend-neutral
// DeviceCaps record from real instance/device queries. Every step degrades
// gracefully: on any failure the runtime stays unavailable and `detail`
// explains why, so callers (and tests) can proceed without a GPU.
class VulkanRuntime {
public:
    [[nodiscard]] static RuntimeAvailability tryInitialize();

    [[nodiscard]] static bool available();
    [[nodiscard]] static const DeviceCaps& deviceCaps();

    static void shutdown();
};

}  // namespace latent::vulkan
