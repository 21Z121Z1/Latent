#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace latent::vulkan {

struct ApiVersion {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
};

struct DeviceCaps {
    ApiVersion apiVersion{};
    bool computeQueue = false;
    bool storageBuffer = false;
    bool commonStorageImages = false;
    bool timestampQueries = false;

    bool androidHardwareBufferExternalMemory = false;
    bool fp16Storage = false;
    bool fp16Arithmetic = false;
    bool timelineSemaphore = false;
    bool synchronization2 = false;
    bool subgroupOperations = false;
    bool descriptorIndexing = false;
    bool shaderFloatControls = false;
    bool int16Arithmetic = false;
    bool integerDotProduct = false;
    bool cooperativeMatrix = false;
};

enum class ProductionSupport : std::uint8_t {
    Unsupported,
    Vulkan11Baseline,
    Vulkan11WithFastPaths,
};

struct CapabilityAssessment {
    ProductionSupport support = ProductionSupport::Unsupported;
    std::vector<std::string> missingRequired;
};

[[nodiscard]] bool isAtLeast(const ApiVersion& version, std::uint32_t major, std::uint32_t minor) noexcept;
[[nodiscard]] CapabilityAssessment assessProductionSupport(const DeviceCaps& caps);

}  // namespace latent::vulkan
