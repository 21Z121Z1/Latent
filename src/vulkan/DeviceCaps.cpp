#include "latent/vulkan/DeviceCaps.h"

namespace latent::vulkan {

bool isAtLeast(const ApiVersion& version, std::uint32_t major, std::uint32_t minor) noexcept {
    if (version.major != major) {
        return version.major > major;
    }
    return version.minor >= minor;
}

CapabilityAssessment assessProductionSupport(const DeviceCaps& caps) {
    CapabilityAssessment result{};

    if (!isAtLeast(caps.apiVersion, 1U, 1U)) {
        result.missingRequired.emplace_back("Vulkan 1.1");
    }
    if (!caps.computeQueue) {
        result.missingRequired.emplace_back("compute queue");
    }
    if (!caps.storageBuffer) {
        result.missingRequired.emplace_back("storage buffer support");
    }
    if (!caps.commonStorageImages) {
        result.missingRequired.emplace_back("common storage image support");
    }
    if (!caps.timestampQueries) {
        result.missingRequired.emplace_back("timestamp queries");
    }

    if (!result.missingRequired.empty()) {
        result.support = ProductionSupport::Unsupported;
        return result;
    }

    const bool hasFastPath = caps.androidHardwareBufferExternalMemory ||
                             (caps.fp16Storage && caps.fp16Arithmetic) ||
                             caps.timelineSemaphore || caps.synchronization2 ||
                             caps.subgroupOperations;
    result.support = hasFastPath ? ProductionSupport::Vulkan11WithFastPaths
                                 : ProductionSupport::Vulkan11Baseline;
    return result;
}

}  // namespace latent::vulkan
