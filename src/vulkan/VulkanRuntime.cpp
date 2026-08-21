#include "latent/vulkan/VulkanRuntime.h"

#include "latent/vulkan/IngressPlan.h"

#include <algorithm>
#include <cstring>
#include <volk.h>

namespace latent::vulkan {
namespace {

struct RuntimeState {
    bool initialized = false;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    DeviceCaps caps{};
    std::string detail;
};

RuntimeState& state() {
    static RuntimeState value;
    return value;
}

bool hasExtension(const char* name, const std::vector<VkExtensionProperties>& extensions) {
    return std::any_of(extensions.begin(), extensions.end(), [&](const auto& entry) {
        return std::strcmp(entry.extensionName, name) == 0;
    });
}

ApiVersion decodeApiVersion(std::uint32_t version) noexcept {
    return {VK_API_VERSION_MAJOR(version),
            VK_API_VERSION_MINOR(version),
            VK_API_VERSION_PATCH(version)};
}

std::uint32_t findComputeQueueFamily(VkPhysicalDevice physicalDevice) {
    std::uint32_t familyCount = 0U;
    vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, &familyCount, nullptr);
    if (familyCount == 0U) {
        return VK_QUEUE_FAMILY_IGNORED;
    }

    std::vector<VkQueueFamilyProperties2> families(familyCount);
    for (auto& family : families) {
        family.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
    }
    vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, &familyCount,
                                              families.data());

    for (std::uint32_t index = 0; index < families.size(); ++index) {
        if ((families[index].queueFamilyProperties.queueFlags &
             VK_QUEUE_COMPUTE_BIT) != 0U) {
            return index;
        }
    }
    return VK_QUEUE_FAMILY_IGNORED;
}

}  // namespace

RuntimeAvailability VulkanRuntime::tryInitialize() {
    RuntimeAvailability availability{};
    auto& runtime = state();
    if (runtime.initialized) {
        availability.loaderAvailable = true;
        availability.instanceCreated = runtime.instance != VK_NULL_HANDLE;
        availability.deviceCreated = runtime.device != VK_NULL_HANDLE;
        if (runtime.instance == VK_NULL_HANDLE || runtime.device == VK_NULL_HANDLE) {
            availability.detail = runtime.detail;
        }
        return availability;
    }

    if (volkInitialize() != VK_SUCCESS) {
        runtime.detail = "Vulkan loader (libvulkan) is unavailable";
        availability.detail = runtime.detail;
        return availability;
    }
    availability.loaderAvailable = true;

    // Portability drivers (MoltenVK on macOS) are only enumerated when the
    // application opts in; the extension simply does not exist elsewhere.
    std::uint32_t instanceExtensionCount = 0U;
    vkEnumerateInstanceExtensionProperties(
        nullptr, &instanceExtensionCount, nullptr);
    std::vector<VkExtensionProperties> instanceExtensions(
        instanceExtensionCount);
    if (instanceExtensionCount > 0U) {
        vkEnumerateInstanceExtensionProperties(
            nullptr, &instanceExtensionCount, instanceExtensions.data());
    }

    const bool portabilityEnumeration =
        std::any_of(instanceExtensions.begin(), instanceExtensions.end(),
                    [](const auto& entry) {
                        return std::strcmp(entry.extensionName,
                                           VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0;
                    });

    std::vector<const char*> enabledInstanceExtensions;
    VkInstanceCreateFlags instanceFlags = 0U;
    if (portabilityEnumeration) {
        enabledInstanceExtensions.push_back(
            VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        instanceFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VkApplicationInfo application;
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pNext = nullptr;
    application.pApplicationName = "latent";
    application.applicationVersion = 1U;
    application.pEngineName = "latent";
    application.engineVersion = 1U;
    application.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instanceInfo;
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pNext = nullptr;
    instanceInfo.flags = instanceFlags;
    instanceInfo.pApplicationInfo = &application;
    instanceInfo.enabledLayerCount = 0U;
    instanceInfo.ppEnabledLayerNames = nullptr;
    instanceInfo.enabledExtensionCount =
        static_cast<std::uint32_t>(enabledInstanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames =
        enabledInstanceExtensions.empty() ? nullptr
                                          : enabledInstanceExtensions.data();

    if (vkCreateInstance(&instanceInfo, nullptr, &runtime.instance) != VK_SUCCESS) {
        volkFinalize();
        runtime.detail = "VkInstance creation failed";
        availability.detail = runtime.detail;
        return availability;
    }
    availability.instanceCreated = true;
    volkLoadInstance(runtime.instance);

    std::uint32_t deviceCount = 0U;
    vkEnumeratePhysicalDevices(runtime.instance, &deviceCount, nullptr);
    if (deviceCount == 0U) {
        runtime.detail = "no Vulkan physical devices found";
        availability.detail = runtime.detail;
        return availability;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(runtime.instance, &deviceCount, devices.data());

    for (const auto candidate : devices) {
        if (findComputeQueueFamily(candidate) != VK_QUEUE_FAMILY_IGNORED) {
            runtime.physicalDevice = candidate;
            break;
        }
    }
    if (runtime.physicalDevice == VK_NULL_HANDLE) {
        runtime.detail = "no physical device exposes a compute queue";
        availability.detail = runtime.detail;
        return availability;
    }

    VkPhysicalDeviceVulkan11Properties properties11;
    properties11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
    properties11.pNext = nullptr;
    VkPhysicalDeviceProperties2 properties;
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &properties11;
    vkGetPhysicalDeviceProperties2(runtime.physicalDevice, &properties);

    VkPhysicalDeviceVulkan11Features features11;
    features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features11.pNext = nullptr;
    VkPhysicalDeviceVulkan12Features features12;
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = nullptr;
    features11.pNext = &features12;
    VkPhysicalDeviceVulkan13Features features13;
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.pNext = nullptr;
    features12.pNext = &features13;
    VkPhysicalDeviceFeatures2 features;
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features11;
    std::memset(&features.features, 0, sizeof(features.features));
    vkGetPhysicalDeviceFeatures2(runtime.physicalDevice, &features);

    std::uint32_t extensionCount = 0U;
    vkEnumerateDeviceExtensionProperties(
        runtime.physicalDevice, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    if (extensionCount > 0U) {
        vkEnumerateDeviceExtensionProperties(
            runtime.physicalDevice, nullptr, &extensionCount, extensions.data());
    }

    DeviceCaps caps{};
    caps.apiVersion = decodeApiVersion(properties.properties.apiVersion);
    caps.computeQueue = true;
    // Storage buffers and storage images are core capabilities on Vulkan
    // 1.1 devices; per-format support is decided at plan/probe time.
    caps.storageBuffer = true;
    caps.commonStorageImages = true;
    caps.timestampQueries =
        properties.properties.limits.timestampComputeAndGraphics != 0U;
    caps.androidHardwareBufferExternalMemory =
        hasExtension(kAndroidExternalMemoryExtension, extensions);
    // R16_SFLOAT-class storage is universally required from Vulkan 1.2; the
    // definitive answer for exotic formats stays a plan-time format probe.
    caps.fp16Storage = isAtLeast(caps.apiVersion, 1U, 2U);
    caps.fp16Arithmetic = features12.shaderFloat16 != 0U;
    caps.timelineSemaphore = features12.timelineSemaphore != 0U;
    caps.synchronization2 = features13.synchronization2 != 0U;
    caps.subgroupOperations =
        (properties11.subgroupSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0U &&
        (properties11.subgroupSupportedOperations &
         VK_SUBGROUP_FEATURE_BASIC_BIT) != 0U;
    caps.descriptorIndexing = features12.descriptorIndexing != 0U;
    caps.shaderFloatControls =
        isAtLeast(caps.apiVersion, 1U, 2U) ||
        hasExtension("VK_KHR_shader_float_controls", extensions);
    caps.int16Arithmetic = features.features.shaderInt16 != 0U;
    caps.integerDotProduct = hasExtension(
        VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME, extensions);
    caps.cooperativeMatrix = hasExtension(
        VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, extensions);

    const float priority = 1.0F;
    const std::uint32_t queueFamily = findComputeQueueFamily(runtime.physicalDevice);
    VkDeviceQueueCreateInfo queueInfo;
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.pNext = nullptr;
    queueInfo.flags = 0U;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1U;
    queueInfo.pQueuePriorities = &priority;

    VkDeviceCreateInfo deviceInfo;
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = nullptr;
    deviceInfo.flags = 0U;
    deviceInfo.queueCreateInfoCount = 1U;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledLayerCount = 0U;
    deviceInfo.ppEnabledLayerNames = nullptr;
    deviceInfo.enabledExtensionCount = 0U;
    deviceInfo.ppEnabledExtensionNames = nullptr;
    deviceInfo.pEnabledFeatures = nullptr;

    runtime.caps = caps;
    runtime.initialized = true;

    if (vkCreateDevice(runtime.physicalDevice, &deviceInfo, nullptr,
                       &runtime.device) != VK_SUCCESS) {
        runtime.device = VK_NULL_HANDLE;
        runtime.detail =
            "VkDevice creation failed; capability reporting remains valid";
        availability.deviceCreated = false;
        availability.detail = runtime.detail;
        return availability;
    }
    availability.deviceCreated = true;

    return availability;
}

bool VulkanRuntime::available() {
    return state().initialized && state().device != VK_NULL_HANDLE;
}

const DeviceCaps& VulkanRuntime::deviceCaps() {
    return state().caps;
}

VkInstance VulkanRuntime::instanceHandle() {
    return state().instance;
}

VkPhysicalDevice VulkanRuntime::physicalDeviceHandle() {
    return state().physicalDevice;
}

VkDevice VulkanRuntime::deviceHandle() {
    return state().device;
}

std::uint32_t VulkanRuntime::computeQueueFamily() {
    return findComputeQueueFamily(state().physicalDevice);
}

void VulkanRuntime::shutdown() {
    auto& runtime = state();
    if (runtime.device != VK_NULL_HANDLE) {
        vkDestroyDevice(runtime.device, nullptr);
        runtime.device = VK_NULL_HANDLE;
    }
    if (runtime.instance != VK_NULL_HANDLE) {
        vkDestroyInstance(runtime.instance, nullptr);
        runtime.instance = VK_NULL_HANDLE;
    }
    runtime.physicalDevice = VK_NULL_HANDLE;
    runtime.caps = DeviceCaps{};
    runtime.detail.clear();
    runtime.initialized = false;
    volkFinalize();
}

}  // namespace latent::vulkan
