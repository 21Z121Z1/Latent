#include "TemporalFixture.h"
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>

namespace {
using namespace latent;
void require(bool condition, const std::string& detail) {
    if (!condition) throw std::runtime_error(detail);
}
void near(float actual, float expected, float tolerance, const std::string& detail) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
        detail + ": actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
}
template<class F> void rejects(F action, const std::string& detail) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(detail);
}
void singleAndLineage() {
    std::vector<imaging::RawFrame> frames{test::frame(1)};
    const auto expected = reference::reconstructSingleRaw(frames[0], {});
    const auto actual = test::run(frames);
    require(actual.scene.image.rgb == expected.image.rgb, "N=1 image equivalence");
    require(actual.scene.lineage->inputs.size() == 1 && actual.scene.lineage->reference == imaging::FrameId{1}, "N=1 lineage");
    frames.push_back(test::frame(2));
    const auto a = test::run(frames), b = test::run(frames);
    require(a.scene.image.rgb == b.scene.image.rgb, "deterministic replay");
    require(imaging::sameLineage(a.scene.lineage, b.scene.lineage), "deterministic contributions");
    require(a.scene.sourceRawId == 0U && a.scene.lineage->inputs.size() == 2, "multi-source not scalar provenance");
    for (float n : a.fused.uncertainty.effectiveSampleCount) near(n, 2.0F, 1e-5F, "identical static support");
}
void cfaAndBorders() {
    for (auto cfa : {imaging::CfaPattern::RGGB, imaging::CfaPattern::GRBG, imaging::CfaPattern::GBRG, imaging::CfaPattern::BGGR}) {
        auto raw = test::frame(1, {13, 11}, cfa, 0, 0, 0, 1,
            [](float, float, std::size_t c) { return 0.05F + 0.2F * static_cast<float>(c); });
        const auto source = reference::normalizeTemporalRaw(runtime::viewRawFrame(raw), raw, {}, false);
        for (std::uint32_t y = 0; y < 11U; ++y) for (std::uint32_t x = 0; x < 13U; ++x) {
            const auto identity = reference::sampleCfa(source, x, y, 0, 0);
            require(identity.has_value(), "integer border identity must not fetch unused taps");
            const auto expected = source.samples[static_cast<std::size_t>(y) * 13U + x].value;
            near(identity->value, expected, 0, "identity CFA");
            for (float d : {-1.25F, -1.0F, 0.5F, 1.0F, 2.75F}) {
                const auto sample = reference::sampleCfa(source, x, y, d, -d);
                if (sample) near(sample->value, expected, 1e-6F, "fractional CFA cross-color contamination");
            }
        }
        require(!reference::sampleCfa(source, 0, 0, -0.1F, 0), "unsafe border interpolation");
    }
}
void radiometry() {
    auto ref = test::frame(1, {33,31}, imaging::CfaPattern::RGGB, 0,0,0.01F);
    auto src = test::frame(2, {33,31}, imaging::CfaPattern::RGGB, 0,0,0.01F,0.5F);
    const auto n = reference::normalizeTemporalRaw(runtime::viewRawFrame(src), ref, {}, false);
    near(n.radiometricScale[0], 2, 0, "exposure scale");
    near(n.samples[0].variance, 0.0004F, 1e-9F, "variance scales by a squared");
    imaging::LensShadingMap map{1,1,{2,2,2,2}};
    src.lensShading = {map, imaging::MetadataSource::DynamicCaptureResult, imaging::MetadataValidity::Valid, 1};
    const auto lsc = reference::normalizeTemporalRaw(runtime::viewRawFrame(src), ref, {}, true);
    near(lsc.samples[0].value, n.samples[0].value * 2, 1e-7F, "LSC radiometry");
    near(lsc.samples[0].variance, n.samples[0].variance * 4, 1e-8F, "LSC variance");
    src.exposureCalibration.source = imaging::MetadataSource::Unknown;
    src.sensitivityIso = 200;
    const auto uncertain = reference::normalizeTemporalRaw(runtime::viewRawFrame(src), ref, {}, false);
    require(uncertain.gainEstimated && uncertain.radiometricConfidence == 0, "unknown differing gain must fail closed");
    src.sensitivityIso = ref.sensitivityIso;
    require(reference::normalizeTemporalRaw(runtime::viewRawFrame(src), ref, {}, false).radiometricConfidence < 1, "equal nominal ISO is still an estimate");
    src.noiseProfile = {};
    require(reference::normalizeTemporalRaw(runtime::viewRawFrame(src), ref, {}, false).noiseEstimated, "missing noise provenance");
    ref = test::frame(1); src = test::frame(2, {97,81}, imaging::CfaPattern::RGGB,0,0,0,0.5F);
    std::vector<imaging::RawFrame> frames{ref,src};
    const auto out = test::run(frames);
    const auto one = reference::normalizeRaw(ref);
    float mean = 0;
    for (std::size_t i = 0; i < one.samples.size(); ++i) mean += out.fused.sensor.samples[i] - one.samples[i];
    near(mean / static_cast<float>(one.samples.size()), 0, 1e-4F, "no radiometric mean drift");
}
void translation() {
    for (const auto& [dx,dy] : {std::pair{2.0F, -4.0F}, std::pair{20.0F, 12.0F}}) {
        auto r = test::frame(1, {161,129}, imaging::CfaPattern::RGGB,0,0,0.001F);
        auto s = test::frame(2, {161,129}, imaging::CfaPattern::RGGB,dx,dy,0.001F);
        const auto rn = reference::normalizeTemporalRaw(runtime::viewRawFrame(r), r, {}, false);
        const auto sn = reference::normalizeTemporalRaw(runtime::viewRawFrame(s), r, {}, false);
        const auto a = reference::alignTemporalRaw(rn,sn,imaging::FrameId{1},imaging::FrameId{2},{});
        std::cout << "alignment expected=" << dx << ',' << dy << " actual=" << a.global.dx << ',' << a.global.dy << " confidence=" << a.global.confidence << '\n';
        near(a.global.dx,dx,0.1F,"integer/large horizontal displacement gate");
        near(a.global.dy,dy,0.1F,"integer/large vertical displacement gate");
    }
    {
        auto r = test::frame(1, {161,129}, imaging::CfaPattern::RGGB,0,0,0.001F);
        auto s = test::frame(2, {161,129}, imaging::CfaPattern::RGGB,0.75F,-1.25F,0.001F);
        const auto rn = reference::normalizeTemporalRaw(runtime::viewRawFrame(r), r, {}, false);
        const auto sn = reference::normalizeTemporalRaw(runtime::viewRawFrame(s), r, {}, false);
        const auto a = reference::alignTemporalRaw(rn,sn,imaging::FrameId{1},imaging::FrameId{2},{});
        std::cout << "nonlinear texture subpixel=" << a.global.dx << ',' << a.global.dy << '\n';
        near(a.global.dx,0.75F,0.6F,"nonlinear-texture horizontal displacement");
        near(a.global.dy,-1.25F,0.6F,"nonlinear-texture vertical displacement");
    }
    const auto bandLimited = [](float x, float y, std::size_t) {
        return 0.35F + 0.08F * std::sin(0.035F*x + 0.021F*y) +
            0.05F * std::cos(0.027F*x - 0.031F*y) + 0.04F * std::sin(0.009F*x + 0.015F*y);
    };
    auto r = test::frame(1, {161,129}, imaging::CfaPattern::RGGB,0,0,0.002F,1,bandLimited);
    auto s = test::frame(2, {161,129}, imaging::CfaPattern::RGGB,0.75F,-1.25F,0.002F,1,bandLimited);
    const auto rn = reference::normalizeTemporalRaw(runtime::viewRawFrame(r), r, {}, false);
    const auto sn = reference::normalizeTemporalRaw(runtime::viewRawFrame(s), r, {}, false);
    const auto subpixel = reference::alignTemporalRaw(rn,sn,imaging::FrameId{1},imaging::FrameId{2},{});
    std::cout << "subpixel expected=0.75,-1.25 actual=" << subpixel.global.dx << ',' << subpixel.global.dy
              << " confidence=" << subpixel.global.confidence << '\n';
    near(subpixel.global.dx,0.75F,0.30F,"subpixel horizontal displacement gate");
    near(subpixel.global.dy,-1.25F,0.30F,"subpixel vertical displacement gate");
    require(subpixel.global.confidence > 0.5F,"supported subpixel fixture must remain alignment-usable");
}
void staticNoiseReduction() {
    constexpr std::size_t frameCount = 8U;
    constexpr float sigma = 0.02F;
    const imaging::Extent extent{97,81};
    std::vector<imaging::RawFrame> frames;
    for (std::uint64_t id = 1; id <= frameCount; ++id)
        frames.push_back(test::frame(id,extent,imaging::CfaPattern::RGGB,0,0,sigma));
    const auto out = test::run(frames);
    const auto cleanFrame = test::frame(99,extent,imaging::CfaPattern::RGGB,0,0,0);
    const auto truth = reference::normalizeRaw(cleanFrame);
    const auto one = reference::normalizeRaw(frames.front());
    double oneSquared = 0, fusedSquared = 0, meanError = 0, meanVariance = 0, meanSupport = 0;
    std::size_t count = 0;
    for (std::uint32_t y=8; y+8<extent.height; ++y) for (std::uint32_t x=8; x+8<extent.width; ++x) {
        const auto i = static_cast<std::size_t>(y)*extent.width+x;
        const double a = static_cast<double>(one.samples[i]-truth.samples[i]);
        const double b = static_cast<double>(out.fused.sensor.samples[i]-truth.samples[i]);
        oneSquared += a*a; fusedSquared += b*b; meanError += b;
        meanVariance += out.fused.uncertainty.marginalVariance[i];
        meanSupport += out.fused.uncertainty.effectiveSampleCount[i];
        require(std::isfinite(out.fused.uncertainty.marginalVariance[i]) && out.fused.uncertainty.marginalVariance[i] >= 0,
                "full-pipeline uncertainty must be finite/non-negative");
        ++count;
    }
    const double mseRatio = fusedSquared / oneSquared;
    const double expectedVariance = static_cast<double>(sigma*sigma) / static_cast<double>(frameCount);
    const double varianceRatio = (meanVariance/static_cast<double>(count)) / expectedVariance;
    std::cout << "static 8-frame mse_ratio=" << mseRatio << " variance_ratio=" << varianceRatio
              << " effective_n=" << meanSupport/static_cast<double>(count) << '\n';
    require(mseRatio < 0.20,"static full-pipeline noise reduction must approach 1/N without bias amplification");
    require(std::abs(meanError/static_cast<double>(count)) < 0.001,"static fusion mean must not systematically drift");
    require(std::abs(varianceRatio-1.0) < 0.15,"full-pipeline conditional variance calibration");
    require(meanSupport/static_cast<double>(count) > 7.0,"static full-pipeline effective support");
}
void monteCarlo() {
    constexpr std::size_t repetitions = 20000, frames = 8;
    constexpr float sigma = 0.02F;
    double sum = 0, squared = 0;
    std::uint32_t state = 123456789U;
    for (std::size_t k = 0; k < repetitions; ++k) {
        reference::FusionAccumulator accumulator{};
        for (std::size_t n = 0; n < frames; ++n) {
            const float sample = 0.2F + sigma * test::uniformNoise(state);
            reference::accumulateTemporalSample(accumulator,{sample,sigma*sigma,1,0},{1,1,{}},1);
        }
        const float value = accumulator.weightedValue / accumulator.weight;
        sum += value; squared += static_cast<double>(value) * value;
        near(accumulator.weightedVariance / (accumulator.weight * accumulator.weight), sigma*sigma/static_cast<float>(frames),1e-10F,"conditional analytic variance");
    }
    const double mean = sum / static_cast<double>(repetitions);
    const double variance = squared / static_cast<double>(repetitions) - mean * mean;
    const double expected = sigma * sigma / static_cast<double>(frames);
    std::cout << "fixed-weight Monte Carlo mean=" << mean << " variance ratio=" << variance/expected << '\n';
    require(std::abs(mean - 0.2) < 0.0003 && std::abs(variance/expected - 1) < 0.04, "fixed-weight 1/N Monte Carlo calibration");
}
void motionAndClipping() {
    const imaging::Extent extent{97,81};
    auto r = test::frame(1,extent,imaging::CfaPattern::RGGB,0,0,0.003F);
    auto s = test::frame(2,extent,imaging::CfaPattern::RGGB,0,0,0.003F,1,
        [](float x,float y,std::size_t c) { return x>36 && x<60 && y>28 && y<52 ? 0.8F : test::texture(x,y,c); });
    std::vector<imaging::RawFrame> frames{r,s};
    const auto out = test::run(frames);
    const auto ref = reference::normalizeRaw(r);
    const std::size_t center = 40U*97U+48U;
    near(out.fused.sensor.samples[center],ref.samples[center],1e-6F,"moving foreground must not ghost");
    near(out.fused.uncertainty.effectiveSampleCount[center],1,1e-5F,"motion effective support");
    require(out.fused.uncertainty.robustnessConfidence[center] < 0.8F,"motion robustness confidence decreases");
    float support = 0;
    for (std::uint32_t y=4;y<20;++y) for(std::uint32_t x=4;x<20;++x) support += out.fused.uncertainty.effectiveSampleCount[static_cast<std::size_t>(y)*97U+x];
    require(support / 256 > 1.5F,"static background retains temporal support");
    frames[1] = test::frame(2,extent,imaging::CfaPattern::RGGB,0,0,0,1,
        [](float,float,std::size_t) { return 1.2F; });
    const auto clipped = test::run(frames);
    require(clipped.fused.sensor.samples == ref.samples,"clipped candidate cannot contaminate reference");
    frames[0] = frames[1]; frames[0].id=1; frames[0].sensorTimestampNs=100000000;
    const auto allClipped = test::run(frames);
    require(allClipped.fused.sensor.samples[0] > 1 && allClipped.fused.uncertainty.effectiveSampleCount[0] == 0,"all clipped preserves observation but invents no support");
    frames = {test::frame(1,extent,imaging::CfaPattern::BGGR,0,0,0,1,[](float,float,std::size_t){return -0.025F;})};
    require(test::run(frames).fused.sensor.samples[0] < 0,"sub-black must survive");
}
void validationAndPlan() {
    std::vector<imaging::RawFrame> frames{test::frame(1),test::frame(2)};
    const auto burst = imaging::describeRawBurst(imaging::BurstId{3},imaging::CaptureSequenceId{4},imaging::CalibrationId{5},frames);
    const auto a = runtime::compileTemporalPlan(burst,{}, {}, {}, {});
    require(a.stages().size() == graph::temporalSchemas.size() && a.version()==1U,"canonical stage plan");
    require(a.fallback() == runtime::TemporalFallback::VulkanUnavailable,"visible capability fallback");
    runtime::TemporalExecutionPolicy memory{}; memory.memoryBudgetBytes=1;
    rejects([&]{ (void)runtime::compileTemporalPlan(burst,{}, {},memory,{}); },"memory limit must reject before allocation");
    auto bad = frames; bad[1].sensorTimestampNs=bad[0].sensorTimestampNs;
    rejects([&]{(void)test::run(bad);},"invalid timestamp");
    rejects([&]{(void)test::run(frames,imaging::FrameId{999});},"foreign reference");
    reference::TemporalPolicy p{};p.varianceFloor=0;
    rejects([&]{reference::validateTemporalPolicy(p);},"invalid variance floor");
}
}
int main() {
    try {
        singleAndLineage(); cfaAndBorders(); radiometry(); translation(); staticNoiseReduction(); monteCarlo(); motionAndClipping(); validationAndPlan();
        std::cout << "temporal reference properties passed\n";
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
