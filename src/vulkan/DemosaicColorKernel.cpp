#include "latent/vulkan/DemosaicColorKernel.h"

#include "demosaic_color_spv.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace latent::vulkan {
namespace {

#pragma pack(push, 4)
struct PushParams {
    std::uint32_t sampleCount;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t cfaPattern;
    std::uint32_t demosaicMethod;
    std::uint32_t pad0;
    std::uint32_t pad1;
    std::uint32_t pad2;
    std::array<float, 9> matrixValues;   // column-major
    float sceneScale;
    std::uint32_t pad3;
    std::uint32_t pad4;
    std::uint32_t pad5;
};
#pragma pack(pop)

static_assert(sizeof(PushParams) == 84U);
static_assert(offsetof(PushParams, matrixValues) == 32U);
static_assert(offsetof(PushParams, sceneScale) == 68U);

std::array<float, 9> toColumnMajor(const std::array<float, 9>& rowMajor) {
    std::array<float, 9> column{};
    for (std::size_t row = 0; row < 3U; ++row) {
        for (std::size_t col = 0; col < 3U; ++col) {
            column[col * 3U + row] = rowMajor[row * 3U + col];
        }
    }
    return column;
}

}  // namespace

DemosaicColorKernel::DemosaicColorKernel(ComputeRunner& runner)
    : runner_(runner)
{
    const std::vector<std::uint32_t> spirv(
        demosaic_color_spv,
        demosaic_color_spv + sizeof(demosaic_color_spv) / sizeof(demosaic_color_spv[0]));
    pipeline_ = runner_.createComputePipeline(spirv, 2U, sizeof(PushParams));
}

std::vector<std::uint16_t> DemosaicColorKernel::run(
    const DemosaicColorParams& params,
    const std::vector<std::uint16_t>& bayerHalfBits)
{
    if (params.extent.width == 0U || params.extent.height == 0U) {
        throw std::invalid_argument("image extent must be non-zero");
    }
    const auto pixelCount64 = static_cast<std::uint64_t>(params.extent.width) *
                              params.extent.height;
    if (pixelCount64 > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(
            "demosaic kernel extent exceeds the current single-dispatch limit");
    }
    if (bayerHalfBits.size() !=
        static_cast<std::size_t>(params.extent.pixelCount())) {
        throw std::invalid_argument("Bayer sample count must match the extent");
    }

    PushParams push{};
    push.sampleCount = static_cast<std::uint32_t>(bayerHalfBits.size());
    push.width = params.extent.width;
    push.height = params.extent.height;
    push.cfaPattern = static_cast<std::uint32_t>(params.cfa);
    push.demosaicMethod = params.demosaicMethod ==
                                  imaging::DemosaicMethod::MalvarHeCutler2004
                              ? 1U
                              : 0U;
    push.matrixValues = toColumnMajor(params.cameraToScene);
    push.sceneScale = params.sceneScale;

    // Pack the uint16 half-bit samples into little-endian uint32 pairs,
    // duplicating the final sample on odd counts so the tail pair is whole.
    const std::size_t pairCount = (bayerHalfBits.size() + 1U) / 2U;
    std::vector<std::uint32_t> inputPairs(pairCount, 0U);
    for (std::size_t i = 0; i < bayerHalfBits.size(); ++i) {
        auto& target = inputPairs[i / 2U];
        if (i % 2U == 0U) {
            target |= static_cast<std::uint32_t>(bayerHalfBits[i]);
        } else {
            target |= static_cast<std::uint32_t>(bayerHalfBits[i]) << 16U;
        }
    }

    auto inputBuffer = runner_.createStorageBuffer(
        static_cast<VkDeviceSize>(inputPairs.size() * sizeof(std::uint32_t)));
    auto outputBuffer = runner_.createStorageBuffer(
        static_cast<VkDeviceSize>(
            static_cast<std::size_t>(params.extent.pixelCount()) * 2U *
            sizeof(std::uint32_t)));

    try {
        runner_.upload(inputBuffer, inputPairs.data(),
                       inputPairs.size() * sizeof(std::uint32_t));

        const std::array<VkDescriptorBufferInfo, 2> bindings{
            VkDescriptorBufferInfo{inputBuffer.handle, 0U, VK_WHOLE_SIZE},
            VkDescriptorBufferInfo{outputBuffer.handle, 0U, VK_WHOLE_SIZE},
        };

        const std::uint32_t pixelCount =
            static_cast<std::uint32_t>(pixelCount64);
        const std::uint32_t groupCount = (pixelCount + 63U) / 64U;

        runner_.dispatch(pipeline_, bindings, &push, sizeof(PushParams),
                         groupCount);

        std::vector<std::uint32_t> outputWords(
            static_cast<std::size_t>(pixelCount) * 2U, 0U);
        runner_.download(outputBuffer, outputWords.data(),
                         outputWords.size() * sizeof(std::uint32_t));

        std::vector<std::uint16_t> result(
            static_cast<std::size_t>(pixelCount) * 4U, 0U);
        for (std::size_t i = 0; i < outputWords.size(); ++i) {
            result[i * 2U] = static_cast<std::uint16_t>(
                outputWords[i] & 0xFFFFU);
            result[i * 2U + 1U] = static_cast<std::uint16_t>(
                outputWords[i] >> 16U);
        }

        runner_.destroyBuffer(outputBuffer);
        runner_.destroyBuffer(inputBuffer);
        return result;
    } catch (...) {
        runner_.destroyBuffer(outputBuffer);
        runner_.destroyBuffer(inputBuffer);
        throw;
    }
}

}  // namespace latent::vulkan
