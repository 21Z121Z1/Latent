#include "latent/vulkan/SensorPreprocessKernel.h"

#include "sensor_preprocess_spv.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace latent::vulkan {
namespace {

#pragma pack(push, 4)
struct PushParams {
    std::uint32_t sampleCount;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t cfaPattern;
    float whiteLevel;
    std::uint32_t pad0[3];
    std::array<float, 4> blackLevels;
    std::array<float, 4> wbGains;
    std::uint32_t gridColumns;
    std::uint32_t gridRows;
    std::uint32_t lscEnabled;
    std::uint32_t pad1;
};
#pragma pack(pop)

static_assert(sizeof(PushParams) == 80U);
static_assert(offsetof(PushParams, blackLevels) == 32U);
static_assert(offsetof(PushParams, wbGains) == 48U);
static_assert(offsetof(PushParams, gridColumns) == 64U);

std::vector<std::uint32_t> packSamplePairs(const std::vector<std::uint16_t>& samples) {
    const std::size_t pairCount = (samples.size() + 1U) / 2U;
    std::vector<std::uint32_t> pairs(pairCount, 0U);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        auto& target = pairs[i / 2U];
        if (i % 2U == 0U) {
            target |= static_cast<std::uint32_t>(samples[i]);
        } else {
            target |= static_cast<std::uint32_t>(samples[i]) << 16U;
        }
    }
    return pairs;
}

}  // namespace

SensorPreprocessKernel::SensorPreprocessKernel(ComputeRunner& runner)
    : runner_(runner)
{
    const std::vector<std::uint32_t> spirv(
        sensor_preprocess_spv,
        sensor_preprocess_spv +
            sizeof(sensor_preprocess_spv) / sizeof(sensor_preprocess_spv[0]));
    pipeline_ = runner_.createComputePipeline(spirv, 3U, sizeof(PushParams));
}

std::vector<std::uint16_t> SensorPreprocessKernel::run(
    const PreprocessParams& params,
    const std::vector<std::uint16_t>& canonicalRaw) {
    if (canonicalRaw.size() !=
        static_cast<std::size_t>(params.extent.pixelCount())) {
        throw std::invalid_argument("canonical RAW sample count must match the extent");
    }

    const PushParams push{
        static_cast<std::uint32_t>(canonicalRaw.size()),
        params.extent.width,
        params.extent.height,
        static_cast<std::uint32_t>(params.cfa),
        params.whiteLevel,
        {0U, 0U, 0U},
        params.black,
        params.wbGains,
        params.lensShading.gridColumns,
        params.lensShading.gridRows,
        params.lensShading.gains.empty() ? 0U : 1U,
        0U,
    };

    const auto inputPairs = packSamplePairs(canonicalRaw);

    auto inputBuffer = runner_.createStorageBuffer(
        static_cast<VkDeviceSize>(inputPairs.size() * sizeof(std::uint32_t)));
    auto outputBuffer = runner_.createStorageBuffer(
        static_cast<VkDeviceSize>(inputPairs.size() * sizeof(std::uint32_t)));

    std::vector<float> lscData(4U, 1.0F);
    if (!params.lensShading.gains.empty()) {
        lscData = params.lensShading.gains;
    }
    auto lscBuffer = runner_.createStorageBuffer(
        static_cast<VkDeviceSize>(lscData.size() * sizeof(float)));

    try {
        runner_.upload(inputBuffer, inputPairs.data(),
                       inputPairs.size() * sizeof(std::uint32_t));
        runner_.upload(lscBuffer, lscData.data(),
                       lscData.size() * sizeof(float));

        const std::array<VkDescriptorBufferInfo, 3> bindings{
            VkDescriptorBufferInfo{inputBuffer.handle, 0U, VK_WHOLE_SIZE},
            VkDescriptorBufferInfo{lscBuffer.handle, 0U, VK_WHOLE_SIZE},
            VkDescriptorBufferInfo{outputBuffer.handle, 0U, VK_WHOLE_SIZE},
        };

        const std::uint32_t pairCount =
            static_cast<std::uint32_t>((canonicalRaw.size() + 1U) / 2U);
        const std::uint32_t groupCount = (pairCount + 255U) / 256U;

        runner_.dispatch(pipeline_, bindings, &push, sizeof(PushParams),
                         groupCount);

        std::vector<std::uint32_t> outputPairs(inputPairs.size(), 0U);
        runner_.download(outputBuffer, outputPairs.data(),
                         outputPairs.size() * sizeof(std::uint32_t));

        std::vector<std::uint16_t> result(canonicalRaw.size(), 0U);
        for (std::size_t i = 0; i < result.size(); ++i) {
            const auto pair = outputPairs[i / 2U];
            result[i] = static_cast<std::uint16_t>(
                i % 2U == 0U ? (pair & 0xFFFFU) : (pair >> 16U));
        }

        runner_.destroyBuffer(lscBuffer);
        runner_.destroyBuffer(outputBuffer);
        runner_.destroyBuffer(inputBuffer);
        return result;
    } catch (...) {
        runner_.destroyBuffer(lscBuffer);
        runner_.destroyBuffer(outputBuffer);
        runner_.destroyBuffer(inputBuffer);
        throw;
    }
}

}  // namespace latent::vulkan
