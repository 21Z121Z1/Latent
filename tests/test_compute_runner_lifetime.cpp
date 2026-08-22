#include "latent/imaging/Half.h"
#include "latent/vulkan/ComputeRunner.h"
#include "latent/vulkan/SensorPreprocessKernel.h"
#include "latent/vulkan/VulkanRuntime.h"

#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void testTransferBounds(latent::vulkan::ComputeRunner& runner) {
    auto buffer = runner.createStorageBuffer(4U);
    const std::uint32_t value = 0x12345678U;

    bool uploadRejected = false;
    try {
        runner.upload(buffer, &value, sizeof(value) + 1U);
    } catch (const std::invalid_argument&) {
        uploadRejected = true;
    }
    check(uploadRejected, "upload beyond buffer capacity must be rejected");

    bool downloadRejected = false;
    try {
        std::uint64_t out = 0U;
        runner.download(buffer, &out, sizeof(out));
    } catch (const std::invalid_argument&) {
        downloadRejected = true;
    }
    check(downloadRejected, "download beyond buffer capacity must be rejected");

    runner.destroyBuffer(buffer);
    check(buffer.handle == VK_NULL_HANDLE && buffer.memory == VK_NULL_HANDLE &&
              buffer.size == 0U,
          "destroyBuffer must reset the full buffer record");

    bool zeroSizeRejected = false;
    try {
        auto impossible = runner.createStorageBuffer(0U);
        runner.destroyBuffer(impossible);
    } catch (const std::invalid_argument&) {
        zeroSizeRejected = true;
    }
    check(zeroSizeRejected, "zero-sized storage buffers must be rejected");
}

void testDescriptorSetsAreRecycled(latent::vulkan::ComputeRunner& runner) {
    using namespace latent::imaging;
    using namespace latent::vulkan;

    SensorPreprocessKernel kernel(runner);

    PreprocessParams params{};
    params.extent = {1U, 1U};
    params.cfa = CfaPattern::RGGB;
    params.whiteLevel = 1000.0F;
    params.black = {0.0F, 0.0F, 0.0F, 0.0F};
    params.wbGains = {1.0F, 1.0F, 1.0F, 1.0F};

    const std::vector<std::uint16_t> raw{500U};

    // The descriptor pool deliberately caps simultaneously allocated sets at
    // 256. Running well past that count proves dispatch recycles completed
    // submission sets instead of leaking one set per image/kernel dispatch.
    constexpr std::uint32_t kDispatches = 320U;
    for (std::uint32_t i = 0U; i < kDispatches; ++i) {
        const auto output = kernel.run(params, raw);
        check(output.size() == 1U,
              "preprocess stress dispatch must produce one output sample");
        if (output.size() == 1U) {
            const float decoded = halfBitsToFloat(output[0]);
            check(std::fabs(decoded - 0.5F) <= 5.0e-4F,
                  "stress dispatch output must remain numerically stable");
        }
    }
}

}  // namespace

int main() {
    std::string detail;
    auto runner = latent::vulkan::ComputeRunner::tryCreate(&detail);
    if (runner == nullptr) {
        std::cout << "compute runner unavailable (" << detail
                  << "); lifetime stress test skipped\n";
        return 0;
    }

    try {
        testTransferBounds(*runner);
        testDescriptorSetsAreRecycled(*runner);
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
    }

    runner.reset();
    latent::vulkan::VulkanRuntime::shutdown();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "compute runner lifetime tests passed\n";
    return 0;
}
