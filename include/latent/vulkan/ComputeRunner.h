#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <volk.h>

namespace latent::vulkan {

// Deliberately thin single-queue compute executor: buffer upload/download and
// one-pipeline dispatches. Graph scheduling, aliasing, and multi-pass
// management belong to a later milestone; this layer only has to be correct.
class ComputeRunner {
public:
    struct Buffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0U;
    };

    // Returns nullptr when no Vulkan device is available; the detail string
    // explains how far initialization reached.
    [[nodiscard]] static std::unique_ptr<ComputeRunner> tryCreate(std::string* detail);

    ~ComputeRunner();

    ComputeRunner(const ComputeRunner&) = delete;
    ComputeRunner& operator=(const ComputeRunner&) = delete;

    [[nodiscard]] Buffer createStorageBuffer(VkDeviceSize size);
    void destroyBuffer(Buffer& buffer);

    void upload(const Buffer& buffer, const void* data, std::size_t byteCount);
    void download(const Buffer& buffer, void* out, std::size_t byteCount);

    // All bindings are storage buffers, bound in declaration order starting
    // at set 0 / binding 0. `pushConstantSize` may be zero.
    [[nodiscard]] VkPipeline createComputePipeline(
        const std::vector<std::uint32_t>& spirv,
        std::uint32_t bindingCount,
        std::uint32_t pushConstantSize);
    void destroyPipeline(VkPipeline pipeline);

    void dispatch(
        VkPipeline pipeline,
        std::span<const VkDescriptorBufferInfo> bindings,
        const void* pushConstants,
        std::uint32_t pushConstantSize,
        std::uint32_t groupCountX);

private:
    ComputeRunner() = default;

    struct PipelineEntry {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    };

    [[nodiscard]] const PipelineEntry& findPipeline(VkPipeline pipeline) const;

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::uint32_t queueFamily_ = 0U;
    std::vector<PipelineEntry> pipelines_;
};

}  // namespace latent::vulkan
