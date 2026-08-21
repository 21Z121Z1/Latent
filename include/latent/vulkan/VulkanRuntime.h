#pragma once

#include "latent/vulkan/DeviceCaps.h"

#include <string>

#include <volk.h>

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

    // Raw handles for executor layers; valid while available() is true.
    [[nodiscard]] static VkInstance instanceHandle();
    [[nodiscard]] static VkPhysicalDevice physicalDeviceHandle();
    [[nodiscard]] static VkDevice deviceHandle();
    [[nodiscard]] static std::uint32_t computeQueueFamily();

    static void shutdown();
};

}  // namespace latent::vulkan
