#pragma once

#include "latent/imaging/RawPacking.h"
#include "latent/vulkan/DeviceCaps.h"

#include <cstdint>
#include <string>

namespace latent::vulkan {

// AHardwareBuffer format codes relevant to camera ingress (AIMAGE_FORMAT_* /
// AHARDWAREBUFFER_FORMAT_* numeric values).
constexpr std::uint32_t kAhbFormatBlob = 1U;
constexpr std::uint32_t kAhbFormatRaw16 = 0x20U;
constexpr std::uint32_t kAhbFormatPrivate = 0x22U;
constexpr std::uint32_t kAhbFormatYuv420_888 = 0x23U;
constexpr std::uint32_t kAhbFormatRawPrivate = 0x24U;
constexpr std::uint32_t kAhbFormatRaw10 = 0x25U;
constexpr std::uint32_t kAhbFormatRaw12 = 0x26U;

// AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER (blob buffers imported as VkBuffer).
constexpr std::uint64_t kAhbUsageGpuDataBuffer = 1ULL << 24ULL;

// Declared here because the header macro is gated behind Android platform
// defines that desktop builds do not set.
inline constexpr const char* kAndroidExternalMemoryExtension =
    "VK_ANDROID_external_memory_android_hardware_buffer";

enum class IngressPath : std::uint8_t {
    Unsupported,
    DirectImportCandidate,
    PortableCopy,
};

enum class SharingGuarantee : std::uint8_t {
    None,
    HandleImportOnly,
    MeasuredZeroCopy,
};

struct AhbDescriptor {
    std::uint32_t format = 0;
    std::uint64_t usage = 0;
};

struct IngressRequest {
    AhbDescriptor buffer{};
    imaging::RawPacking packing = imaging::RawPacking::Raw16;
    imaging::Extent extent{};
};

struct IngressDecision {
    IngressPath path = IngressPath::Unsupported;
    bool requiresRuntimeFormatProbe = false;
    bool externalFormatExpected = false;
    SharingGuarantee sharing = SharingGuarantee::None;
    std::string reason;
};

[[nodiscard]] IngressDecision planIngress(const DeviceCaps& caps, const IngressRequest& request);

}  // namespace latent::vulkan
