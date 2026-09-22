#include "latent/vulkan/ComputeRunner.h"

#include "latent/vulkan/VulkanRuntime.h"

#include <algorithm>
#include <array>
#include <limits>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace latent::vulkan {
namespace {
// The existing runtime shares one queue across runners. Queue access is externally synchronized.
std::recursive_mutex queueMutex;

std::uint32_t findHostVisibleMemoryType(
    VkPhysicalDevice physicalDevice,
    std::uint32_t typeBits) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);

    std::uint32_t selected = UINT32_MAX;
    int best = -1;
    for (std::uint32_t type = 0; type < properties.memoryTypeCount; ++type) {
        const bool hostVisible =
            (properties.memoryTypes[type].propertyFlags &
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U;
        const bool hostCoherent =
            (properties.memoryTypes[type].propertyFlags &
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U;
        if (hostVisible && hostCoherent && (typeBits & (1U << type)) != 0U) {
            const auto flags = properties.memoryTypes[type].propertyFlags;
            const int score = ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? 2 : 0) +
                              ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 1 : 0);
            if (score > best) { best = score; selected = type; }
        }
    }
    if (selected != UINT32_MAX) return selected;
    throw std::runtime_error("no coherent host-visible memory type available");
}

void validateTransfer(
    const ComputeRunner::Buffer& buffer,
    const void* pointer,
    std::size_t byteCount,
    const char* operation) {
    if (static_cast<VkDeviceSize>(byteCount) > buffer.size) {
        throw std::invalid_argument(
            std::string(operation) + " exceeds the buffer size");
    }
    if (byteCount > 0U && pointer == nullptr) {
        throw std::invalid_argument(
            std::string(operation) + " requires a non-null data pointer");
    }
}

}  // namespace

std::unique_ptr<ComputeRunner> ComputeRunner::tryCreate(std::string* detail) {
    const std::lock_guard lock(queueMutex);
    const auto availability = VulkanRuntime::tryInitialize();
    if (!VulkanRuntime::available()) {
        if (detail != nullptr) {
            *detail = availability.detail.empty() ? std::string("Vulkan runtime unavailable")
                                                  : availability.detail;
        }
        return nullptr;
    }

    auto runner = std::unique_ptr<ComputeRunner>(new ComputeRunner());
    runner->physicalDevice_ = VulkanRuntime::physicalDeviceHandle();
    runner->device_ = VulkanRuntime::deviceHandle();
    runner->queueFamily_ = VulkanRuntime::computeQueueFamily();

    vkGetDeviceQueue(runner->device_, runner->queueFamily_, 0U, &runner->queue_);

    const VkCommandPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        nullptr,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        runner->queueFamily_,
    };
    if (vkCreateCommandPool(runner->device_, &poolInfo, nullptr,
                            &runner->commandPool_) != VK_SUCCESS) {
        if (detail != nullptr) {
            *detail = "VkCommandPool creation failed";
        }
        return nullptr;
    }

    const VkDescriptorPoolSize poolSize{
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        4096U,
    };
    const VkDescriptorPoolCreateInfo descriptorPoolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        nullptr,
        VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        256U,
        1U,
        &poolSize,
    };
    if (vkCreateDescriptorPool(runner->device_, &descriptorPoolInfo, nullptr,
                               &runner->descriptorPool_) != VK_SUCCESS) {
        if (detail != nullptr) {
            *detail = "VkDescriptorPool creation failed";
        }
        return nullptr;
    }

    const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
    if (vkCreateFence(runner->device_, &fenceInfo, nullptr, &runner->fence_) != VK_SUCCESS) {
        if (detail) *detail = "VkFence creation failed";
        return nullptr;
    }
    return runner;
}

ComputeRunner::~ComputeRunner() {
    const std::lock_guard lock(queueMutex);
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(device_);
    for (const auto& entry : pipelines_) {
        vkDestroyPipeline(device_, entry.pipeline, nullptr);
        vkDestroyPipelineLayout(device_, entry.pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(device_, entry.setLayout, nullptr);
    }
    pipelines_.clear();
    if (timestampPool_ != VK_NULL_HANDLE) vkDestroyQueryPool(device_, timestampPool_, nullptr);
    if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, fence_, nullptr);
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    }
    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
    }
}

ComputeRunner::Buffer ComputeRunner::createStorageBuffer(VkDeviceSize size) {
    if (size == 0U) {
        throw std::invalid_argument("storage buffer size must be non-zero");
    }

    Buffer buffer{};
    buffer.size = size;
    VkPhysicalDeviceProperties limits{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &limits);
    if (size > limits.limits.maxStorageBufferRange)
        throw std::invalid_argument("storage buffer exceeds maxStorageBufferRange");

    const VkBufferCreateInfo bufferInfo{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        nullptr,
        0U,
        size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_SHARING_MODE_EXCLUSIVE,
        0U,
        nullptr,
    };
    if (vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer.handle) !=
        VK_SUCCESS) {
        throw std::runtime_error("VkBuffer creation failed");
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer.handle, &requirements);

    VkMemoryAllocateInfo allocate{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, 0U};
    try {
        allocate.memoryTypeIndex =
            findHostVisibleMemoryType(physicalDevice_, requirements.memoryTypeBits);
    } catch (...) {
        vkDestroyBuffer(device_, buffer.handle, nullptr);
        buffer.handle = VK_NULL_HANDLE;
        throw;
    }

    if (vkAllocateMemory(device_, &allocate, nullptr, &buffer.memory) !=
        VK_SUCCESS) {
        vkDestroyBuffer(device_, buffer.handle, nullptr);
        buffer.handle = VK_NULL_HANDLE;
        throw std::runtime_error("VkDeviceMemory allocation failed");
    }

    if (vkBindBufferMemory(device_, buffer.handle, buffer.memory, 0U) !=
        VK_SUCCESS) {
        vkFreeMemory(device_, buffer.memory, nullptr);
        vkDestroyBuffer(device_, buffer.handle, nullptr);
        buffer.memory = VK_NULL_HANDLE;
        buffer.handle = VK_NULL_HANDLE;
        throw std::runtime_error("vkBindBufferMemory failed");
    }

    buffer.allocationSize = requirements.size;
    if (vkMapMemory(device_, buffer.memory, 0, size, 0, &buffer.mapped) != VK_SUCCESS) {
        destroyBuffer(buffer);
        throw std::runtime_error("persistent vkMapMemory failed");
    }
    return buffer;
}

void ComputeRunner::destroyBuffer(Buffer& buffer) {
    if (buffer.mapped) { vkUnmapMemory(device_, buffer.memory); buffer.mapped = nullptr; }
    if (buffer.handle != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer.handle, nullptr);
        buffer.handle = VK_NULL_HANDLE;
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, buffer.memory, nullptr);
        buffer.memory = VK_NULL_HANDLE;
    }
    buffer.size = 0U;
    buffer.allocationSize = 0U;
}

void ComputeRunner::upload(
    const Buffer& buffer,
    const void* data,
    std::size_t byteCount) {
    validateTransfer(buffer, data, byteCount, "buffer upload");
    if (byteCount == 0U) {
        return;
    }

    if (!buffer.mapped) throw std::invalid_argument("upload to unmapped/destroyed buffer");
    std::memcpy(buffer.mapped, data, byteCount);
}

void ComputeRunner::download(const Buffer& buffer, void* out, std::size_t byteCount) {
    validateTransfer(buffer, out, byteCount, "buffer download");
    if (byteCount == 0U) {
        return;
    }

    if (!buffer.mapped) throw std::invalid_argument("download from unmapped/destroyed buffer");
    std::memcpy(out, buffer.mapped, byteCount);
}

VkPipeline ComputeRunner::createComputePipeline(
    const std::vector<std::uint32_t>& spirv,
    std::uint32_t bindingCount,
    std::uint32_t pushConstantSize) {
    const std::lock_guard lock(queueMutex);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    if (spirv.empty() || !bindingCount || bindingCount > 16 ||
        bindingCount > props.limits.maxPerStageDescriptorStorageBuffers ||
        bindingCount > props.limits.maxDescriptorSetStorageBuffers ||
        pushConstantSize > props.limits.maxPushConstantsSize || pushConstantSize % 4U)
        throw std::invalid_argument("compute pipeline exceeds device limits");
    VkShaderModuleCreateInfo moduleInfo{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        nullptr,
        0U,
        spirv.size() * sizeof(std::uint32_t),
        spirv.data(),
    };

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &moduleInfo, nullptr, &shaderModule) !=
        VK_SUCCESS) {
        throw std::runtime_error("VkShaderModule creation failed");
    }

    std::array<VkDescriptorSetLayoutBinding,16> layoutBindings{};
    for (std::uint32_t i = 0; i < bindingCount; ++i) {
        layoutBindings[i] = {};
        layoutBindings[i].binding = i;
        layoutBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        layoutBindings[i].descriptorCount = 1U;
        layoutBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo setLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        nullptr,
        0U,
        bindingCount,
        layoutBindings.data(),
    };

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device_, &setLayoutInfo, nullptr,
                                    &setLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, shaderModule, nullptr);
        throw std::runtime_error("VkDescriptorSetLayout creation failed");
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0U;
    pushRange.size = pushConstantSize;

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        nullptr,
        0U,
        1U,
        &setLayout,
        pushConstantSize > 0U ? 1U : 0U,
        &pushRange,
    };

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr,
                               &pipelineLayout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(device_, setLayout, nullptr);
        vkDestroyShaderModule(device_, shaderModule, nullptr);
        throw std::runtime_error("VkPipelineLayout creation failed");
    }

    const VkPipelineShaderStageCreateInfo stageInfo{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        nullptr,
        0U,
        VK_SHADER_STAGE_COMPUTE_BIT,
        shaderModule,
        "main",
        nullptr,
    };

    const VkComputePipelineCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        nullptr,
        0U,
        stageInfo,
        pipelineLayout,
        VK_NULL_HANDLE,
        -1,
    };

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result =
        vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1U, &pipelineInfo,
                                 nullptr, &pipeline);

    vkDestroyShaderModule(device_, shaderModule, nullptr);

    if (result != VK_SUCCESS || pipeline == VK_NULL_HANDLE) {
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline, nullptr);
        vkDestroyPipelineLayout(device_, pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(device_, setLayout, nullptr);
        throw std::runtime_error("compute pipeline creation failed");
    }

    PipelineEntry entry{pipeline, pipelineLayout, setLayout, VK_NULL_HANDLE, VK_NULL_HANDLE,
                        bindingCount, pushConstantSize};
    const VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        nullptr, descriptorPool_, 1, &setLayout};
    const VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        nullptr, commandPool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    try {
        if (vkAllocateDescriptorSets(device_, &setInfo, &entry.descriptorSet) != VK_SUCCESS)
            throw std::runtime_error("persistent descriptor set allocation failed");
        if (vkAllocateCommandBuffers(device_, &commandInfo, &entry.command) != VK_SUCCESS)
            throw std::runtime_error("persistent command allocation failed");
        pipelines_.push_back(entry);
    } catch (...) {
        if (entry.command) vkFreeCommandBuffers(device_, commandPool_, 1, &entry.command);
        if (entry.descriptorSet) (void)vkFreeDescriptorSets(device_, descriptorPool_, 1, &entry.descriptorSet);
        vkDestroyPipeline(device_, pipeline, nullptr);
        vkDestroyPipelineLayout(device_, pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(device_, setLayout, nullptr);
        throw;
    }
    return pipeline;
}

const ComputeRunner::PipelineEntry& ComputeRunner::findPipeline(
    VkPipeline pipeline) const {
    for (const auto& entry : pipelines_) {
        if (entry.pipeline == pipeline) {
            return entry;
        }
    }
    throw std::runtime_error("dispatch references an unknown pipeline");
}

void ComputeRunner::destroyPipeline(VkPipeline pipeline) {
    const std::lock_guard lock(queueMutex);
    for (auto it = pipelines_.begin(); it != pipelines_.end(); ++it) {
        if (it->pipeline == pipeline) {
            vkFreeCommandBuffers(device_, commandPool_, 1, &it->command);
            (void)vkFreeDescriptorSets(device_, descriptorPool_, 1, &it->descriptorSet);
            vkDestroyPipeline(device_, it->pipeline, nullptr);
            vkDestroyPipelineLayout(device_, it->pipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(device_, it->setLayout, nullptr);
            pipelines_.erase(it);
            return;
        }
    }
}

bool ComputeRunner::enableTimestamps() {
    const std::lock_guard lock(queueMutex);
    if (timestampsEnabled_) return true;
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &count, families.data());
    if (queueFamily_ >= count || families[queueFamily_].timestampValidBits == 0) return false;
    timestampBits_ = families[queueFamily_].timestampValidBits;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    timestampPeriod_ = props.limits.timestampPeriod;
    const VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr,
        0, VK_QUERY_TYPE_TIMESTAMP, 2, 0};
    if (vkCreateQueryPool(device_, &queryInfo, nullptr, &timestampPool_) != VK_SUCCESS) return false;
    timestampsEnabled_ = true;
    return true;
}

void ComputeRunner::dispatch(
    VkPipeline pipeline,
    std::span<const VkDescriptorBufferInfo> bindings,
    const void* pushConstants,
    std::uint32_t pushConstantSize,
    std::uint32_t groupCountX) {
    const std::lock_guard lock(queueMutex);
    const auto& entry = findPipeline(pipeline);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    if (bindings.size() != entry.bindingCount || pushConstantSize != entry.pushConstantSize ||
        (pushConstantSize && !pushConstants) || groupCountX > props.limits.maxComputeWorkGroupCount[0])
        throw std::invalid_argument("compute dispatch layout or limit mismatch");
    // No per-dispatch descriptor/command allocation or heap vectors. All work
    // completes before reuse. Multiple runners externally synchronize the shared queue.
    const auto descriptorSet = entry.descriptorSet;
    const auto command = entry.command;
    std::array<VkWriteDescriptorSet,16> writes{};
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet,
            static_cast<std::uint32_t>(i), 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            nullptr, &bindings[i], nullptr};
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(bindings.size()), writes.data(), 0, nullptr);
    if (vkResetCommandBuffer(command, 0) != VK_SUCCESS)
        throw std::runtime_error("vkResetCommandBuffer failed");
    const VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    if (vkBeginCommandBuffer(command, &beginInfo) != VK_SUCCESS)
        throw std::runtime_error("vkBeginCommandBuffer failed");
    if (timestampsEnabled_) {
        vkCmdResetQueryPool(command, timestampPool_, 0, 2);
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestampPool_, 0);
    }
    const VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
        VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipelineLayout,
        0, 1, &descriptorSet, 0, nullptr);
    if (pushConstantSize) vkCmdPushConstants(command, entry.pipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, pushConstantSize, pushConstants);
    vkCmdDispatch(command, groupCountX, 1, 1);
    const VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &after, 0, nullptr, 0, nullptr);
    if (timestampsEnabled_)
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampPool_, 1);
    if (vkEndCommandBuffer(command) != VK_SUCCESS)
        throw std::runtime_error("vkEndCommandBuffer failed");
    if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS)
        throw std::runtime_error("vkResetFences failed");
    const VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr,
        0, nullptr, nullptr, 1, &command, 0, nullptr};
    if (vkQueueSubmit(queue_, 1, &submitInfo, fence_) != VK_SUCCESS)
        throw std::runtime_error("vkQueueSubmit failed");
    const auto wait = vkWaitForFences(device_, 1, &fence_, VK_TRUE, std::numeric_limits<std::uint64_t>::max());
    if (wait != VK_SUCCESS) {
        (void)vkQueueWaitIdle(queue_);
        throw std::runtime_error("compute fence wait failed");
    }
    if (timestampsEnabled_) {
        std::array<std::uint64_t,2> ticks{};
        if (vkGetQueryPoolResults(device_, timestampPool_, 0, 2, sizeof(ticks), ticks.data(),
            sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            throw std::runtime_error("timestamp results unavailable after fence");
        const auto mask = timestampBits_ == 64 ? UINT64_MAX : (UINT64_C(1) << timestampBits_) - 1;
        gpuMilliseconds_ += static_cast<double>((ticks[1]-ticks[0]) & mask) * timestampPeriod_ * 1e-6;
    }
}
}  // namespace latent::vulkan
