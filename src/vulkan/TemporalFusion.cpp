#include "latent/vulkan/TemporalFusion.h"
#include "latent/vulkan/ComputeRunner.h"
#include "latent/vulkan/VulkanRuntime.h"
#include "temporal_fusion_spv.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace latent::vulkan {
namespace {
struct Push {
    std::uint32_t width, height, tileSize, columns, isReference;
    float varianceFloor, cutoffSigma, minimumConfidence, radiometricConfidence;
    std::uint32_t basePixel;
};
static_assert(sizeof(Push) == 40U);
struct RegionWords { std::array<std::uint32_t, 10> words{}; };
static_assert(sizeof(RegionWords) == 40U);

bool supports(imaging::Extent extent, std::string* detail) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(VulkanRuntime::physicalDeviceHandle(), &properties);
    const auto n = extent.pixelCount();
    const bool ok = extent.width >= 2U && extent.height >= 2U &&
        n <= std::numeric_limits<std::uint32_t>::max() / 8U &&
        n <= properties.limits.maxStorageBufferRange / sizeof(reference::FusionAccumulator) &&
        properties.limits.maxPerStageDescriptorStorageBuffers >= 4U &&
        properties.limits.maxComputeWorkGroupInvocations >= 128U &&
        properties.limits.maxComputeWorkGroupSize[0] >= 128U &&
        properties.limits.maxPushConstantsSize >= sizeof(Push);
    if (!ok && detail) *detail = "temporal buffers or compute layout exceed device limits";
    return ok;
}
class Session final : public reference::TemporalFusionSession {
public:
    Session(const reference::NormalizedRaw& ref, const reference::TemporalPolicy& policy)
        : reference_(ref), policy_(policy) {
        reference::validateTemporalPolicy(policy);
        std::string detail;
        runner_ = ComputeRunner::tryCreate(&detail);
        if (!runner_ || !supports(ref.extent, &detail)) throw std::runtime_error(detail);
        const auto n = ref.extent.pixelCount();
        if (ref.samples.size() != n) throw std::invalid_argument("invalid Vulkan reference payload");
        columns_ = (ref.extent.width + policy.tileSize - 1U) / policy.tileSize;
        rows_ = (ref.extent.height + policy.tileSize - 1U) / policy.tileSize;
        regions_.resize(static_cast<std::size_t>(columns_) * rows_);
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(VulkanRuntime::physicalDeviceHandle(), &properties);
        maxGroups_ = properties.limits.maxComputeWorkGroupCount[0];
        try {
            pipeline_ = runner_->createComputePipeline(
                {std::begin(temporal_fusion_spv), std::end(temporal_fusion_spv)}, 4U, sizeof(Push));
            buffers_[0] = runner_->createStorageBuffer(n * sizeof(reference::TemporalSample));
            buffers_[1] = runner_->createStorageBuffer(n * sizeof(reference::TemporalSample));
            buffers_[2] = runner_->createStorageBuffer(n * sizeof(reference::FusionAccumulator));
            buffers_[3] = runner_->createStorageBuffer(regions_.size() * sizeof(RegionWords));
            runner_->upload(buffers_[0], ref.samples.data(), ref.samples.size() * sizeof(reference::TemporalSample));
            // Initial upload is bounded and released before processing a source.
            std::vector<reference::FusionAccumulator> zero(static_cast<std::size_t>(n));
            runner_->upload(buffers_[2], zero.data(), zero.size() * sizeof(reference::FusionAccumulator));
        } catch (...) { release(); throw; }
    }
    ~Session() override { release(); }
    imaging::FrameContribution add(const reference::NormalizedRaw& source,
        const reference::AlignmentField& field, bool isReference) override {
        if (finished_) throw std::logic_error("fusion session already finalized");
        if (source.extent.width != reference_.extent.width || source.extent.height != reference_.extent.height ||
            source.cfa != reference_.cfa || source.samples.size() != reference_.samples.size() ||
            field.extent.width != source.extent.width || field.extent.height != source.extent.height ||
            field.tileSize != policy_.tileSize || field.columns != columns_ || field.rows != rows_ ||
            field.tiles.size() != regions_.size() || !std::isfinite(source.radiometricConfidence) ||
            source.radiometricConfidence < 0 || source.radiometricConfidence > 1) {
            throw std::invalid_argument("invalid Vulkan temporal source/field binding");
        }
        for (std::size_t i = 0; i < regions_.size(); ++i) {
            const auto& t = field.tiles[i];
            if (!std::isfinite(t.dx) || !std::isfinite(t.dy) || !std::isfinite(t.confidence) ||
                t.confidence < 0 || t.confidence > 1) throw std::invalid_argument("invalid motion tile");
            regions_[i].words = {std::bit_cast<std::uint32_t>(t.dx), std::bit_cast<std::uint32_t>(t.dy),
                std::bit_cast<std::uint32_t>(t.confidence), std::bit_cast<std::uint32_t>(t.residual), 0,0,0,0,0,0};
        }
        const bool boundReference = &source == &reference_;
        if (!boundReference) runner_->upload(buffers_[1], source.samples.data(), source.samples.size() * sizeof(reference::TemporalSample));
        runner_->upload(buffers_[3], regions_.data(), regions_.size() * sizeof(RegionWords));
        const std::array bindings{
            VkDescriptorBufferInfo{buffers_[0].handle, 0, VK_WHOLE_SIZE},
            VkDescriptorBufferInfo{buffers_[boundReference ? 0U : 1U].handle, 0, VK_WHOLE_SIZE},
            VkDescriptorBufferInfo{buffers_[2].handle, 0, VK_WHOLE_SIZE},
            VkDescriptorBufferInfo{buffers_[3].handle, 0, VK_WHOLE_SIZE}};
        Push p{source.extent.width, source.extent.height, policy_.tileSize, columns_, isReference ? 1U : 0U,
            policy_.varianceFloor, policy_.residualCutoffSigma, policy_.minimumAlignmentConfidence,
            source.radiometricConfidence, 0U};
        const auto count = static_cast<std::uint32_t>(source.samples.size());
        while (p.basePixel < count) {
            const auto groups = std::min(maxGroups_, (count - p.basePixel + 127U) / 128U);
            runner_->dispatch(pipeline_, bindings, &p, sizeof(p), groups);
            p.basePixel += groups * 128U;
        }
        runner_->download(buffers_[3], regions_.data(), regions_.size() * sizeof(RegionWords));
        imaging::FrameContribution contribution{}; contribution.frame = field.source;
        contribution.regions.resize(regions_.size());
        for (std::uint32_t y = 0; y < rows_; ++y) for (std::uint32_t x = 0; x < columns_; ++x) {
            const auto i = static_cast<std::size_t>(y) * columns_ + x;
            auto& region = contribution.regions[i]; const auto& words = regions_[i].words;
            region.x = x * policy_.tileSize; region.y = y * policy_.tileSize;
            region.width = std::min(policy_.tileSize, source.extent.width - region.x);
            region.height = std::min(policy_.tileSize, source.extent.height - region.y);
            region.accepted = words[9]; contribution.acceptedSamples += region.accepted;
            std::copy_n(words.begin() + 4, 5, region.rejected.begin());
        }
        return contribution;
    }
    std::vector<reference::FusionAccumulator> finish() override {
        if (finished_) throw std::logic_error("fusion session already finalized");
        std::vector<reference::FusionAccumulator> result(reference_.samples.size());
        runner_->download(buffers_[2], result.data(), result.size() * sizeof(reference::FusionAccumulator));
        finished_ = true;
        return result;
    }
private:
    void release() noexcept {
        if (!runner_) return;
        for (auto& buffer : buffers_) runner_->destroyBuffer(buffer);
        if (pipeline_ != VK_NULL_HANDLE) runner_->destroyPipeline(pipeline_);
        pipeline_ = VK_NULL_HANDLE;
    }
    const reference::NormalizedRaw& reference_;
    reference::TemporalPolicy policy_;
    std::unique_ptr<ComputeRunner> runner_;
    std::array<ComputeRunner::Buffer, 4> buffers_{};
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::vector<RegionWords> regions_;
    std::uint32_t columns_ = 0, rows_ = 0, maxGroups_ = 0;
    bool finished_ = false;
};
}  // namespace
bool temporalFusionAvailable(imaging::Extent extent, std::string* detail) {
    const auto runner = ComputeRunner::tryCreate(detail);
    return runner && supports(extent, detail);
}
std::unique_ptr<reference::TemporalFusionSession> makeTemporalFusionSession(
    const reference::NormalizedRaw& ref, const reference::TemporalPolicy& policy) {
    return std::make_unique<Session>(ref, policy);
}
}  // namespace latent::vulkan
