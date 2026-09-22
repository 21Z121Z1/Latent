#include "AnalyticBurst.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

int main() {
    using namespace latent;
    try {
        reference::TemporalPolicy policy{}; policy.maximumDisplacement = 12;
        // Fixed independent phase sweep. Not aligned to the implementation's
        // old quarter-pixel search grid; includes negative displacements.
        constexpr std::array<std::array<float, 2>, 8> shifts{{
            {0.13F, -0.27F}, {0.41F, 0.59F}, {0.73F, -0.87F},
            {-1.11F, 1.37F}, {1.59F, -1.73F}, {-0.07F, -0.91F},
            {3.21F, -2.43F}, {-4.63F, 3.81F}}};
        float maximum = 0, squared = 0; std::size_t count = 0;
        for (const auto cfa : {imaging::CfaPattern::RGGB, imaging::CfaPattern::GRBG,
                              imaging::CfaPattern::GBRG, imaging::CfaPattern::BGGR}) {
            const auto ref = test::analytic::observation(0, 0, cfa);
            for (const auto& shift : shifts) {
                const auto src = test::analytic::observation(shift[0], shift[1], cfa);
                const auto field = reference::alignTemporalRaw(ref, src, imaging::FrameId{1}, imaging::FrameId{2}, policy);
                const float error = std::hypot(field.global.dx - shift[0], field.global.dy - shift[1]);
                squared += error * error; maximum = std::max(maximum, error); ++count;
                std::cout << "registration cfa=" << static_cast<int>(cfa) << " dx=" << shift[0] << " dy=" << shift[1]
                          << " estimate=" << field.global.dx << ',' << field.global.dy << " error_px=" << error << '\n';
            }
        }
        const float rms = std::sqrt(squared / static_cast<float>(count));
        std::cout << "phase_sweep n=" << count << " max_px=" << maximum << " rms_px=" << rms << '\n';
        if (maximum > 0.06F || rms > 0.035F) throw std::runtime_error("continuous phase accuracy gate failed");
        const auto flat = [](std::uint32_t seed, bool noisy) {
            auto image = test::analytic::observation(0, 0);
            std::mt19937 engine(seed); std::normal_distribution<float> noise(0, 0.003F);
            for (auto& sample : image.samples) sample = {0.02F+(noisy ? noise(engine) : 0.0F), noisy ? 9.0e-6F : 0, 1, 1};
            return image;
        };
        for (const bool noisy : {false,true}) {
            const auto field = reference::alignTemporalRaw(flat(31,noisy),flat(47,noisy),imaging::FrameId{1},imaging::FrameId{2},policy);
            if (field.globalEvidence.status != reference::GeometryStatus::Unobservable)
                throw std::runtime_error("flat/noisy-flat field must not claim observable geometry");
        }
        auto periodic = test::analytic::observation(0,0);
        for (std::uint32_t y=0;y<periodic.extent.height;++y) for (std::uint32_t x=0;x<periodic.extent.width;++x) {
            // Repeated 8x8 photosite texture: exact ambiguity, not a random
            // pattern compared to another copy of our alignment algorithm.
            const float value = 0.3F+0.06F*std::sin(std::numbers::pi_v<float>*static_cast<float>(x%8U)/4.0F)
                +0.04F*std::cos(std::numbers::pi_v<float>*static_cast<float>(y%8U)/4.0F);
            periodic.samples[static_cast<std::size_t>(y)*periodic.extent.width+x] = {value,1.0e-8F,1,1};
        }
        const auto ambiguous = reference::alignTemporalRaw(periodic,periodic,imaging::FrameId{1},imaging::FrameId{2},policy);
        if (ambiguous.globalEvidence.status != reference::GeometryStatus::Ambiguous)
            throw std::runtime_error("periodic texture must retain competing-motion ambiguity");
        float noiseSquared=0, noiseMaximum=0; std::size_t covered=0;
        constexpr std::uint32_t realizations=16;
        for (std::uint32_t k=0;k<realizations;++k) {
            const auto ref = test::analytic::observation(0,0,imaging::CfaPattern::RGGB,{129,113},0.12F,0.0001F,1.0e-7F,101U+2U*k);
            const auto src = test::analytic::observation(0.63F,-0.81F,imaging::CfaPattern::RGGB,{129,113},0.12F,0.0001F,1.0e-7F,102U+2U*k);
            const auto field = reference::alignTemporalRaw(ref,src,imaging::FrameId{1},imaging::FrameId{2},policy);
            const float error=std::hypot(field.global.dx-0.63F,field.global.dy+0.81F);
            noiseSquared+=error*error; noiseMaximum=std::max(noiseMaximum,error);
            if (std::isfinite(field.globalEvidence.localizationStdDevPixels) && error <= 3.0F*field.globalEvidence.localizationStdDevPixels) ++covered;
        }
        const float noiseRms=std::sqrt(noiseSquared/static_cast<float>(realizations));
        std::cout << "poisson_read n=" << realizations << " rms_px=" << noiseRms << " max_px=" << noiseMaximum << " within_linearized_3sigma=" << covered << '\n';
        if (noiseRms > 0.15F || noiseMaximum > 0.3F || covered < 14U)
            throw std::runtime_error("Poisson/read-noise registration gate failed");
        auto invalid = test::analytic::observation(0,0);
        invalid.samples[0].variance = std::numeric_limits<float>::quiet_NaN();
        bool rejected = false;
        try { (void)reference::alignTemporalRaw(flat(1,false),invalid,imaging::FrameId{1},imaging::FrameId{2},policy); }
        catch (const std::invalid_argument&) { rejected = true; }
        if (!rejected) throw std::runtime_error("non-finite alignment evidence was accepted");
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
