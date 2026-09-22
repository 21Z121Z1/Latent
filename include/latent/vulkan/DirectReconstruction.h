#pragma once
#include "latent/reference/DirectReconstruct.h"
namespace latent::vulkan {
// Null means initialization/capability failure. Numerical failures throw; they
// are not hidden by substituting a different reconstruction halfway through.
[[nodiscard]] std::unique_ptr<reference::DirectTileExecutor> makeVulkanDirectExecutor(
    std::size_t maxSourcePixels, std::size_t maxOutputPixels,
    const reference::DirectKernelPolicy&, std::string* detail = nullptr,
    bool collectTimestamps = false);
}
