#include "latent/runtime/JointScene.h"
#include "latent/render/ReferenceRenderer.h"
#include "latent/testing/SyntheticRaw.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace latent;
namespace {
std::size_t checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F&& f, const char* message) {
    bool threw = false;
    try { f(); } catch (const std::exception&) { threw = true; }
    check(threw, message);
}
void close(double actual, double expected, double tolerance, const char* message) {
    check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}
reference::ReconstructionConfig colorConfig() {
    reference::ReconstructionConfig c{};
    c.cameraToAcescg.values = {1.1F, -0.2F, 0.1F, -0.1F, 1.3F, -0.2F, 0.1F, -0.3F, 1.2F};
    c.whiteBalanceGains = {2.0F, 1.0F, 1.0F, 1.5F};
    c.sceneScaleEV = 2;
    c.whiteBalanceConfidence = 0.75F;
    return c;
}
std::array<double, 3> expectedColor(const std::array<float, 4>& rgb,
    const reference::ReconstructionConfig& c) {
    const std::array<double, 3> gains{c.whiteBalanceGains[0],
        (static_cast<double>(c.whiteBalanceGains[1]) + c.whiteBalanceGains[2]) / 2, c.whiteBalanceGains[3]};
    std::array<double, 3> result{};
    for (std::size_t j = 0; j < 3; ++j) {
        for (std::size_t k = 0; k < 3; ++k) result[j] += static_cast<double>(c.cameraToAcescg.values[j * 3 + k]) * gains[k] * rgb[k];
        result[j] *= std::exp2(static_cast<double>(c.sceneScaleEV));
    }
    return result;
}
void tileColorAndBounds() {
    const auto c = colorConfig();
    std::array<reference::ReconstructedPixel, 2> pixels{};
    pixels[0].rgb = {-0.25F, 0.5F, 1.5F, 0};
    pixels[1].rgb = {1.25F, -0.1F, 0.3F, 0};
    for (auto& p : pixels) { p.confidence = {1, 0.2F, 0.01F, 0}; p.variance = {0.04F, 0.01F, 0.09F, 0}; }
    std::array<float, 6> rgb{}, variance{};
    reference::finishDirectSceneTile(pixels, c, rgb, variance);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const auto expected = expectedColor(pixels[i].rgb, c);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            // Independent double-precision matrix oracle vs sequential FP32 WB,
            // matrix accumulation and scene scale: five rounded operations.
            close(rgb[i * 3 + channel], expected[channel], 2e-6 * std::max(1.0, std::abs(expected[channel])), "camera to scene color composition");
            // Enumerate every perfectly correlated +/- noise realization. The
            // worst realization must attain the bound, not a diagonal-only sum.
            double maximum = 0;
            for (unsigned signs = 0; signs < 8; ++signs) {
                std::array<float, 4> perturbation{};
                for (std::size_t k = 0; k < 3; ++k) perturbation[k] = std::sqrt(pixels[i].variance[k]) * ((signs & (1U << k)) ? 1.0F : -1.0F);
                const double delta = expectedColor(perturbation, c)[channel];
                maximum = std::max(maximum, delta * delta);
            }
            close(variance[i * 3 + channel], maximum, 2e-6 * maximum, "unknown cross-channel covariance upper bound");
        }
    }
    check(rgb[0] < 0 && rgb[2] > 1, "negative and above-nominal scene coordinates preserved");
    for (const float gain : {0.0001F, 0.03125F, 3.5F, 64.0F}) {
        auto scaled = pixels;
        for (auto& p : scaled) for (std::size_t k = 0; k < 3; ++k) { p.rgb[k] *= gain; p.variance[k] *= gain * gain; }
        std::array<float, 6> scaledRgb{}, scaledVariance{};
        reference::finishDirectSceneTile(scaled, c, scaledRgb, scaledVariance);
        for (std::size_t k = 0; k < rgb.size(); ++k) {
            close(scaledRgb[k] / gain, rgb[k], 4e-6, "finishing intensity-scale equivariance");
            close(scaledVariance[k] / (gain * gain), variance[k], 4e-6, "finishing variance-square scaling");
        }
    }
    auto noNoise = c; noNoise.propagateNoise = false;
    std::array<float, 6> justRgb{};
    reference::finishDirectSceneTile(pixels, noNoise, justRgb, {});
    check(justRgb == rgb, "uncertainty storage policy cannot change image intent");
}
// Composition with the OLD CFA->demosaic->scene boundary on a constant
// spectral field. Both legitimate DNG matrix routes must resolve identically;
// this catches a bypass of the canonical resolver or a second white balance.
void dngComposition() {
    for (bool forward : {false, true}) {
        auto c = colorConfig(); c.colorPath = reference::ColorPath::DngProfile;
        c.dngProfile.colorMatrix1.values = {0.8F, 0.1F, 0.05F, 0.02F, 1.0F, 0.03F, 0.1F, 0.04F, 1.1F};
        c.dngProfile.colorMatrix2 = c.dngProfile.colorMatrix1;
        c.dngProfile.forwardMatricesPresent = forward;
        if (forward) {
            const auto d50 = imaging::xyToXyz(imaging::kIlluminantD50);
            c.dngProfile.forwardMatrix1.values = {d50[0], 0, 0, 0, d50[1], 0, 0, 0, d50[2]};
            c.dngProfile.forwardMatrix2 = c.dngProfile.forwardMatrix1;
        }
        reference::SensorLinearFrameF32 sensor{}; sensor.extent = {12, 10};
        sensor.samples.resize(sensor.extent.pixelCount());
        constexpr std::array<float, 4> camera{-0.1F, 0.3F, 1.2F, 0};
        const auto sampling = imaging::regularSampling(sensor.extent, sensor.cfa);
        for (std::uint32_t y = 0; y < sensor.extent.height; ++y) for (std::uint32_t x = 0; x < sensor.extent.width; ++x) {
            const auto channel = imaging::samplingChannelAt(sampling, x, y);
            const std::size_t rgbChannel = channel == imaging::CfaChannel::R ? 0U : channel == imaging::CfaChannel::B ? 2U : 1U;
            sensor.samples[static_cast<std::size_t>(y) * sensor.extent.width + x] = camera[rgbChannel];
        }
        reference::ReconstructedPixel p{}; p.rgb = camera; p.confidence = {1, 1, 1, 0};
        std::array<float, 3> direct{}, variance{};
        reference::finishDirectSceneTile(std::span(&p, 1), c, direct, variance);
        const auto legacy = reference::reconstructSensorLinear(sensor, c);
        for (std::uint32_t y = 3; y + 3 < sensor.extent.height; ++y) for (std::uint32_t x = 3; x + 3 < sensor.extent.width; ++x) {
            const auto index = (static_cast<std::size_t>(y) * sensor.extent.width + x) * 3U;
            for (std::size_t k = 0; k < 3; ++k) close(direct[k], legacy.image.rgb[index + k], 4e-6 * std::max(1.0F, std::abs(direct[k])),
                "DNG direct finishing equals existing demosaic color boundary on constant spectral field");
        }
    }
}
void invalidTiles() {
    auto c = colorConfig();
    reference::ReconstructedPixel p{}; p.rgb = {0.2F, 0.3F, 0.4F, 0}; p.confidence = {1, 1, 1, 0};
    std::array<float, 3> rgb{}, variance{};
    const auto finish = [&] { reference::finishDirectSceneTile(std::span(&p, 1), c, rgb, variance); };
    const auto good = p;
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        p = good; p.rgb[0] = bad; rejects(finish, "non-finite camera color rejected");
        p = good; p.variance[1] = bad; rejects(finish, "non-finite camera variance rejected");
        p = good; p.confidence[2] = bad; rejects(finish, "non-finite camera coverage rejected");
    }
    p = good; p.confidence[2] = 0; rejects(finish, "missing camera color cannot become scene black");
    p = good; p.variance[1] = -0.1F; rejects(finish, "negative variance rejected");
    p = good;
    c.applyLensShading = true; rejects(finish, "duplicate shading rejected"); c.applyLensShading = false;
    c.defectCorrection = reference::DefectCorrectionMode::FromMetadataMap; rejects(finish, "duplicate defect correction rejected");
    c = colorConfig(); c.colorPath = static_cast<reference::ColorPath>(255); rejects(finish, "unknown color path rejected");
    c = colorConfig(); c.colorPath = reference::ColorPath::DngProfile; c.dngProfile.analogBalance[0] = 0; rejects(finish, "invalid DNG calibration rejected");
    c = colorConfig(); c.sceneScaleEV = -1000; rejects(finish, "underflow to zero is not a scene coordinate");
    c.sceneScaleEV = 1000; rejects(finish, "scene coordinate overflow rejected");
    c = colorConfig(); c.whiteBalanceGains[0] = 0; rejects(finish, "invalid WB rejected");
    c = colorConfig(); c.whiteBalanceGains[1] = c.whiteBalanceGains[2] = std::numeric_limits<float>::max();
    rejects(finish, "common green overflow rejected before silently changing color");
    c = colorConfig(); p.rgb[0] = std::numeric_limits<float>::max(); rejects(finish, "color arithmetic overflow rejected");
    p = good; p.variance[0] = std::numeric_limits<float>::max(); rejects(finish, "variance bound overflow rejected");
    p = good;
    rejects([&] { reference::finishDirectSceneTile(std::span(&p, 1), c, std::span(rgb).first(2), variance); }, "scene extent mismatch rejected");
    rejects([&] { reference::finishDirectSceneTile(std::span(&p, 1), c, rgb, {}); }, "missing requested uncertainty rejected");
    rejects([&] { reference::finishDirectSceneTile({}, c, {}, {}); }, "empty finishing tile rejected");
}
runtime::ReconstructionCapabilities caps() {
    runtime::ReconstructionCapabilities c{};
#ifdef LATENT_JOINT_TEST_VULKAN
    c.preferVulkan = true;
#endif
    return c;
}
void checkDomains(const runtime::JointSceneResult& r, const imaging::RawBurst& b) {
    const auto& s = r.scene;
    check(s.reference == imaging::ReferenceDomain::Scene && s.primaries == imaging::Primaries::ACEScgAP1 &&
        s.whitePoint == imaging::WhitePoint::D60 && s.transfer == imaging::TransferFunction::Linear &&
        s.range == imaging::RangeSemantics::Unbounded && s.allowNegative, "scene domain invariants");
    check(!s.propagatedNoise.valid, "conditional variance must not become single-frame affine noise");
    check(s.lineage && s.lineage->burst == b.id && s.lineage->sequence == b.sequence &&
        s.lineage->calibration == b.calibration && s.lineage->reference == r.trace.reference, "scene provenance identity");
    check(s.lineage->inputs.size() == b.members.size(), "lineage keeps all captured frames");
    for (std::size_t i = 0; i < b.members.size(); ++i) check(s.lineage->inputs[i] == b.members[i].id, "lineage preserves capture order");
    check(s.lineage->contributions.empty(), "unmeasured contribution counts stay unavailable");
    check(s.sourceRawId == (b.members.size() == 1 ? r.trace.reference.value : 0), "no scalar multi-frame provenance");
#ifdef LATENT_JOINT_TEST_VULKAN
    check(r.trace.plan.vulkan, "required real Vulkan reconstruction path");
#endif
}
void latticeSceneComposition() {
    // Different CFA order, grouped period and origin are resolved ONLY by RAW
    // normalization. Scene finishing sees no Bayer parity or sensor mode enum.
    const auto color = colorConfig();
    double maxDifference = 0;
    for (std::uint32_t group = 1; group <= 4; ++group) for (unsigned phase = 0; phase < 4; ++phase) {
        auto b = testing::syntheticBurst({19, 17}, 3, group, group, static_cast<imaging::CfaPattern>(phase), 3, 1, 0, 0.00001F);
        const auto m = testing::translations(3);
        const testing::Signal signal = [](float x, float y, std::size_t c, std::size_t) {
            return 0.14F + 0.07F * static_cast<float>(c) + 0.04F * std::sin(0.3F * x) + 0.03F * std::cos(0.2F * y);
        };
        testing::SyntheticRawSource source(b, m, signal);
        const float step = phase % 2U ? 0.5F : 1.0F;
        const runtime::ReconstructionGrid grid{step == 0.5F ? imaging::Extent{37, 33} : b.extent, 0, 0, step, step};
        runtime::TiledReconstructionPolicy p{}; p.tileSize = 16; p.maximumDisplacement = 4;
        p.kernel = reference::DirectKernelPolicy::superResolution();
        const auto scene = runtime::reconstructRawScene(b, source, grid, b.members[0].id, color, p, caps(), m);
        checkDomains(scene, b);
        check(scene.scene.image.extent == grid.extent && scene.scene.sceneScaleEV == color.sceneScaleEV &&
            scene.scene.whiteBalanceConfidence == color.whiteBalanceConfidence, "explicit scene request survives reconstruction");
        check(scene.trace.plan.sinkResidentBytes == scene.scene.image.rgb.capacity() * sizeof(float) +
            scene.conditionalVarianceUpperBound.capacity() * sizeof(float), "all materialized outputs charged to sink");
        testing::ImageSink camera(grid.extent);
        (void)runtime::reconstructRawTiles(b, source, grid, p, {}, camera, m, b.members[0].id);
        for (std::size_t i = 0; i < camera.pixels.size(); ++i) {
            const auto expected = expectedColor(camera.pixels[i].rgb, color);
            for (std::size_t c = 0; c < 3; ++c) {
                const double difference = std::abs(scene.scene.image.rgb[3 * i + c] - expected[c]);
                maxDifference = std::max(maxDifference, difference);
                // Existing CPU/Vulkan camera budget 3e-5 propagated by absolute
                // WB/color row norm and sceneScale; do not assume cancellation.
                double rowGain = 0;
                const std::array<double, 3> wb{color.whiteBalanceGains[0], 1.0, color.whiteBalanceGains[3]};
                for (std::size_t k = 0; k < 3; ++k) rowGain += std::abs(color.cameraToAcescg.values[3 * c + k]) * wb[k] * 4;
                close(scene.scene.image.rgb[3 * i + c], expected[c], 3e-5 * rowGain, "direct RAW to scene vs independent color composition");
                // Propagate the EXISTING per-camera variance differential
                // interval through the monotone covariance-bound formula. This
                // accounts for sqrt sensitivity near zero without enlarging the
                // raw oracle budget (1e-7 + 1e-3 * variance).
                double sigma = 0, lowerSigma = 0, upperSigma = 0;
                for (std::size_t k = 0; k < 3; ++k) {
                    const double v = camera.pixels[i].variance[k];
                    const double error = 1e-7 + 1e-3 * v;
                    const double factor = std::abs(color.cameraToAcescg.values[3 * c + k]) * wb[k] * 4;
                    sigma += factor * std::sqrt(v);
                    lowerSigma += factor * std::sqrt(std::max(0.0, v - error));
                    upperSigma += factor * std::sqrt(v + error);
                }
                const double value = scene.conditionalVarianceUpperBound[3 * i + c];
                const double roundoff = 4e-6 * sigma * sigma;
                check(std::isfinite(value) && value >= std::max(0.0, lowerSigma * lowerSigma - roundoff) &&
                    value <= upperSigma * upperSigma + roundoff, "CPU/Vulkan scene variance interval composition");
            }
        }
    }
    std::cout << "joint-scene composition_max_abs=" << maxDifference << '\n';
}
void tilingRenderingAndLineage() {
    const auto b = testing::syntheticBurst({35, 29}, 5, 2, 2, imaging::CfaPattern::GBRG, 1, 3);
    auto motion = testing::translations(5);
    // Explicitly choose a reference beyond the budget prefix; its observations
    // remain reference coordinates and the untouched burst remains provenance.
    motion[4].affine = {1, 0, 0, 0, 1, 0};
    testing::SyntheticRawSource source(b, motion);
    const runtime::ReconstructionGrid grid{{39, 31}, 4, 3, 0.75F, 0.75F};
    runtime::TiledReconstructionPolicy p{}; p.tileSize = 16; p.maximumDisplacement = 4; p.maximumFrames = 3;
    const auto c = colorConfig();
    const auto tiled = runtime::reconstructRawScene(b, source, grid, b.members[4].id, c, p, caps(), motion);
    p.tileSize = 64;
    const auto full = runtime::reconstructRawScene(b, source, grid, b.members[4].id, c, p, caps(), motion);
    checkDomains(tiled, b); checkDomains(full, b);
    check(tiled.scene.image.rgb == full.scene.image.rgb && tiled.conditionalVarianceUpperBound == full.conditionalVarianceUpperBound,
        "tiled scene and variance are seam-free including odd extents and non-2x grid");
    check(imaging::sameLineage(tiled.scene.lineage, full.scene.lineage), "tiling cannot alter provenance");
    check(tiled.trace.plan.frames == 3 && tiled.scene.lineage->inputs.size() == 5, "reduced execution retains excluded input identities");
    const auto copy = tiled.scene.image.rgb;
    const auto sdr = render::renderReference(tiled.scene, render::makeSdrRenderConfig());
    const auto hdr = render::renderReference(tiled.scene, render::makeHdrPqRenderConfig());
    check(imaging::sameLineage(sdr.lineage, tiled.scene.lineage) && imaging::sameLineage(hdr.lineage, tiled.scene.lineage), "rendered SDR/HDR preserve joint RAW lineage");
    check(tiled.scene.image.rgb == copy, "rendering leaves scene master unchanged");
    for (float x : sdr.image.rgb) check(std::isfinite(x), "finite SDR rendition");
    for (float x : hdr.image.rgb) check(std::isfinite(x), "finite HDR rendition");
    auto shifted = c; shifted.sceneScaleEV += 1;
    const auto other = runtime::reconstructRawScene(b, source, grid, b.members[4].id, shifted, p, caps(), motion);
    const auto otherSdr = render::renderReference(other.scene, render::makeSdrRenderConfig());
    for (std::size_t i = 0; i < sdr.image.rgb.size(); ++i) close(otherSdr.image.rgb[i], sdr.image.rgb[i], 2e-6, "scene representation coordinate is not render exposure");
}
class CountingSource final : public runtime::RawTileSource {
public:
    explicit CountingSource(runtime::RawTileSource& source) : source_(source) {}
    void read(imaging::FrameId id, imaging::SensorRect r, std::span<std::uint16_t> out) override { ++reads; source_.read(id, r, out); }
    std::uint64_t residentBytes() const override { return source_.residentBytes(); }
    std::size_t reads = 0;
private:
    runtime::RawTileSource& source_;
};
void calibrationAndFailure() {
    auto b = testing::syntheticBurst({29, 25}, 1);
    b.members[0].observations.colorCorrectionGains = {std::array<float, 4>{2, 1.25F, 1.0F / 1.2F, 1.5F},
        imaging::MetadataSource::MeasuredCalibration, imaging::MetadataValidity::Valid, 1};
    const auto m = testing::translations(1);
    const testing::Signal flat = [](float, float, std::size_t c, std::size_t) { return std::array<float, 3>{0.11F, 0.18F, 0.29F}[c]; };
    testing::SyntheticRawConfig raw{}; raw.greenResponse = {0.8F, 1.2F};
    testing::SyntheticRawSource source(b, m, flat, raw); CountingSource counted(source);
    runtime::ReconstructionGrid grid{b.extent};
    runtime::TiledReconstructionPolicy p{}; p.maximumDisplacement = 4; p.tileSize = 16;
    reference::ReconstructionConfig color{};
    rejects([&] { (void)runtime::reconstructRawScene(b, counted, grid, b.members[0].id, color, p, caps(), m); }, "inconsistent green calibration rejected");
    check(counted.reads == 0, "calibration mismatch rejected before RAW reads");
    color.whiteBalanceGains = *b.members[0].observations.colorCorrectionGains.value;
    auto budget = caps(); budget.hostBudgetBytes = b.extent.pixelCount() * 24U;
    rejects([&] { (void)runtime::reconstructRawScene(b, counted, grid, b.members[0].id, color, p, budget, m); }, "output and arena jointly admitted");
    check(counted.reads == 0, "memory rejection precedes RAW reads");
    rejects([&] { (void)runtime::reconstructRawScene(b, counted, grid, imaging::FrameId{99}, color, p, caps(), m); }, "missing reference rejected");
    const auto scene = runtime::reconstructRawScene(b, counted, grid, b.members[0].id, color, p, caps(), m);
    checkDomains(scene, b);
    check(scene.trace.kernel.quadraticStrength == 0, "K1 remains Gaussian not claimed SR");
    for (std::size_t i = 0; i < scene.scene.image.rgb.size(); ++i) close(scene.scene.image.rgb[i], std::array<double, 3>{0.22, 0.18, 0.435}[i % 3], 4e-4,
        "unequal greens balanced before fusion and common green applied once");
    color.propagateNoise = false;
    const auto without = runtime::reconstructRawScene(b, counted, grid, b.members[0].id, color, p, caps(), m);
    check(without.conditionalVarianceUpperBound.empty() && without.scene.image.rgb == scene.scene.image.rgb, "optional scene variance does not change RGB");
    std::size_t checkpoints = 0;
    rejects([&] { (void)runtime::reconstructRawScene(b, counted, grid, b.members[0].id, color, p, caps(), m,
        [&] { return ++checkpoints < 4; }); }, "cancellation discards incomplete scene");
    const testing::Signal clipped = [](float, float, std::size_t, std::size_t) { return 2.0F; };
    testing::SyntheticRawSource saturated(b, m, clipped);
    rejects([&] { (void)runtime::reconstructRawScene(b, saturated, grid, b.members[0].id, color, p, caps(), m); }, "all-clipped burst cannot supply a scene");
    testing::ImageSink rawResult(grid.extent);
    const auto trace = runtime::reconstructRawTiles(b, saturated, grid, p, {}, rawResult, m, b.members[0].id);
    check(trace.invalidChannels == grid.extent.pixelCount() * 3U, "camera-linear evidence still available with explicit invalid mask");
}
}
int main() {
    try {
        tileColorAndBounds(); dngComposition(); invalidTiles(); latticeSceneComposition(); tilingRenderingAndLineage(); calibrationAndFailure();
        std::cout << "joint-scene checks=" << checks << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << "joint-scene failure: " << e.what() << '\n'; return 1; }
}
