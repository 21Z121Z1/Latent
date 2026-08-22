#include "latent/vulkan/ComputeRunner.h"

#include "latent/vulkan/VulkanRuntime.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace latent::vulkan {
namespace {

std::uint32_t findHostVisibleMemoryType(
    VkPhysicalDevice physicalDevice,
    std::uint32_t typeBits) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);

    for (std::uint32_t type = 0; type < properties.memoryTypeCount; ++type) {
        const bool hostVisible =
            (properties.memoryTypes[type].propertyFlags &
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U;
        const bool hostCoherent =
            (properties.memoryTypes[type].propertyFlags &
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U;
        if (hostVisible && hostCoherent && (typeBits & (1U << type)) != 0U) {
            return type;
        }
    }
    throw std::runtime_error("no host-visible device memory type available");
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

    return runner;
}

ComputeRunner::~ComputeRunner() {
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
    allocate.memoryTypeIndex =
        findHostVisibleMemoryType(physicalDevice_, requirements.memoryTypeBits);

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

    return buffer;
}

void ComputeRunner::destroyBuffer(Buffer& buffer) {
    if (buffer.handle != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer.handle, nullptr);
        buffer.handle = VK_NULL_HANDLE;
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, buffer.memory, nullptr);
        buffer.memory = VK_NULL_HANDLE;
    }
    buffer.size = 0U;
}

void ComputeRunner::upload(
    const Buffer& buffer,
    const void* data,
    std::size_t byteCount) {
    validateTransfer(buffer, data, byteCount, "buffer upload");
    if (byteCount == 0U) {
        return;
    }

    void* mapped = nullptr;
    if (vkMapMemory(device_, buffer.memory, 0U,
                    static_cast<VkDeviceSize>(byteCount), 0U, &mapped) !=
        VK_SUCCESS) {
        throw std::runtime_error("vkMapMemory failed");
    }
    std::memcpy(mapped, data, byteCount);
    vkUnmapMemory(device_, buffer.memory);
}

void ComputeRunner::download(const Buffer& buffer, void* out, std::size_t byteCount) {
    validateTransfer(buffer, out, byteCount, "buffer download");
    if (byteCount == 0U) {
        return;
    }

    void* mapped = nullptr;
    if (vkMapMemory(device_, buffer.memory, 0U,
                    static_cast<VkDeviceSize>(byteCount), 0U, &mapped) !=
        VK_SUCCESS) {
        throw std::runtime_error("vkMapMemory failed");
    }
    std::memcpy(out, mapped, byteCount);
    vkUnmapMemory(device_, buffer.memory);
}

VkPipeline ComputeRunner::createComputePipeline(
    const std::vector<std::uint32_t>& spirv,
    std::uint32_t bindingCount,
    std::uint32_t pushConstantSize) {
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

    std::vector<VkDescriptorSetLayoutBinding> layoutBindings(bindingCount);
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
        vkDestroyPipelineLayout(device_, pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(device_, setLayout, nullptr);
        throw std::runtime_error("compute pipeline creation failed");
    }

    pipelines_.push_back(PipelineEntry{pipeline, pipelineLayout, setLayout});
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
    for (auto it = pipelines_.begin(); it != pipelines_.end(); ++it) {
        if (it->pipeline == pipeline) {
            vkDestroyPipeline(device_, it->pipeline, nullptr);
            vkDestroyPipelineLayout(device_, it->pipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(device_, it->setLayout, nullptr);
            pipelines_.erase(it);
            return;
        }
    }
}

void ComputeRunner::dispatch(
    VkPipeline pipeline,
    std::span<const VkDescriptorBufferInfo> bindings,
    const void* pushConstants,
    std::uint32_t pushConstantSize,
    std::uint32_t groupCountX) {
    const auto& entry = findPipeline(pipeline);

    const VkDescriptorSetAllocateInfo allocateInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        nullptr,
        descriptorPool_,
        1U,
        &entry.setLayout,
    };

    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &allocateInfo, &descriptorSet) !=
        VK_SUCCESS) {
        throw std::runtime_error("descriptor set allocation failed");
    }

    VkCommandBuffer command = VK_NULL_HANDLE;
    const auto cleanupSubmission = [&]() noexcept {
        if (command != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(device_, commandPool_, 1U, &command);
            command = VK_NULL_HANDLE;
        }
        if (descriptorSet != VK_NULL_HANDLE) {
            (void)vkFreeDescriptorSets(
                device_, descriptorPool_, 1U, &descriptorSet);
            descriptorSet = VK_NULL_HANDLE;
        }
    };

    try {
        std::vector<VkWriteDescriptorSet> writes(bindings.size());
        for (std::size_t i = 0; i < bindings.size(); ++i) {
            writes[i] = {};
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = descriptorSet;
            writes[i].dstBinding = static_cast<std::uint32_t>(i);
            writes[i].descriptorCount = 1U;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &bindings[i];
        }
        vkUpdateDescriptorSets(
            device_, static_cast<std::uint32_t>(writes.size()), writes.data(),
            0U, nullptr);

        const VkCommandBufferAllocateInfo commandInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            nullptr,
            commandPool_,
            VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            1U,
        };
        if (vkAllocateCommandBuffers(device_, &commandInfo, &command) !=
            VK_SUCCESS) {
            throw std::runtime_error("command buffer allocation failed");
        }

        const VkCommandBufferBeginInfo beginInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            nullptr,
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            nullptr,
        };
        if (vkBeginCommandBuffer(command, &beginInfo) != VK_SUCCESS) {
            throw std::runtime_error("vkBeginCommandBuffer failed");
        }

        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                entry.pipelineLayout, 0U, 1U,
                                &descriptorSet, 0U, nullptr);
        if (pushConstantSize > 0U && pushConstants != nullptr) {
            vkCmdPushConstants(command, entry.pipelineLayout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                               pushConstantSize, pushConstants);
        }
        vkCmdDispatch(command, groupCountX, 1U, 1U);
        if (vkEndCommandBuffer(command) != VK_SUCCESS) {
            throw std::runtime_error("vkEndCommandBuffer failed");
        }

        const VkSubmitInfo submitInfo{
            VK_STRUCTURE_TYPE_SUBMIT_INFO,
            nullptr,
            0U,
            nullptr,
            nullptr,
            1U,
            &command,
            0U,
            nullptr,
        };
        if (vkQueueSubmit(queue_, 1U, &submitInfo, VK_NULL_HANDLE) !=
            VK_SUCCESS) {
            throw std::runtime_error("vkQueueSubmit failed");
        }
        if (vkQueueWaitIdle(queue_) != VK_SUCCESS) {
            throw std::runtime_error("vkQueueWaitIdle failed");
        }

        cleanupSubmission();
    } catch (...) {
        // vkQueueWaitIdle is required before freeing a submitted command
        // buffer if an error happened after submission. A device-loss error
        // may be returned again; cleanup still releases host-side handles.
        (void)vkQueueWaitIdle(queue_);
        cleanupSubmission();
        throw;
    }
}

}  // namespace latent::vulkan
