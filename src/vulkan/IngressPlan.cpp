#include "latent/vulkan/IngressPlan.h"

namespace latent::vulkan {

IngressDecision planIngress(const DeviceCaps& caps, const IngressRequest& request) {
    const auto baseline = assessProductionSupport(caps);
    if (baseline.support == ProductionSupport::Unsupported) {
        IngressDecision decision{};
        decision.reason = "device lacks the Vulkan 1.1 compute production baseline";
        return decision;
    }

    if (!caps.androidHardwareBufferExternalMemory) {
        IngressDecision decision{};
        decision.path = IngressPath::PortableCopy;
        decision.reason =
            "VK_ANDROID_external_memory_android_hardware_buffer is unavailable; "
            "ingress copies into Vulkan-owned canonical storage";
        return decision;
    }

    switch (request.buffer.format) {
        case kAhbFormatRawPrivate:
            // The layout is implementation-defined; neither a direct import
            // nor a portable copy can interpret it without a device profile.
            return {IngressPath::Unsupported, false, false, SharingGuarantee::None,
                    "RAW_PRIVATE has an implementation-defined pixel layout"};

        case kAhbFormatRaw16:
        case kAhbFormatRaw10:
        case kAhbFormatRaw12:
            // Camera RAW buffers have no standard VkFormat equivalent for
            // Bayer compute access; the portable unpack is the correctness
            // baseline. A runtime format probe may still be attempted before
            // falling back to the copy.
            return {IngressPath::PortableCopy, true, false, SharingGuarantee::None,
                    "camera RAW uses the portable unpack baseline; runtime "
                    "format probe is advisory only"};

        case kAhbFormatYuv420_888:
            return {IngressPath::DirectImportCandidate, true, true,
                    SharingGuarantee::HandleImportOnly,
                    "YUV import requires the external-format/YCbCr sampler "
                    "path confirmed by a runtime probe"};

        case kAhbFormatBlob:
            if ((request.buffer.usage & kAhbUsageGpuDataBuffer) != 0U) {
                return {IngressPath::DirectImportCandidate, false, false,
                        SharingGuarantee::HandleImportOnly,
                        "BLOB with GPU_DATA_BUFFER imports as VkBuffer memory"};
            }
            return {IngressPath::Unsupported, false, false, SharingGuarantee::None,
                    "BLOB buffer lacks AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER"};

        default:
            break;
    }

    if (request.buffer.format == 0U) {
        return {IngressPath::Unsupported, false, false, SharingGuarantee::None,
                "buffer format must be specified"};
    }

    return {IngressPath::DirectImportCandidate, true, false,
            SharingGuarantee::HandleImportOnly,
            "known-format import still requires a runtime properties probe"};
}

}  // namespace latent::vulkan
