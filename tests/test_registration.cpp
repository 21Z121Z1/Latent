#include "latent/reference/TemporalReconstruct.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

namespace {
using namespace latent;
// Continuous, band-limited irradiance, evaluated at each physical sensor site.
// No warping/interpolation of a discrete reference is used to generate truth.
float irradiance(float x, float y, std::size_t channel) {
    const float luma = 0.3F + 0.06F * std::sin(0.17F*x + 0.11F*y) +
        0.05F * std::cos(0.09F*x - 0.19F*y) + 0.04F * std::sin(0.31F*x - 0.07F*y) +
        0.03F * std::cos(0.13F*x + 0.29F*y);
    return luma * std::array<float,4>{0.85F,1.0F,1.0F,0.7F}[channel];
}
reference::NormalizedRaw observation(imaging::CfaPattern cfa, float dx, float dy,
                                     float shot, float read, std::uint32_t seed) {
    reference::NormalizedRaw raw{};
    raw.extent = {192,160}; raw.cfa = cfa;
    raw.samples.resize(raw.extent.pixelCount());
    std::mt19937 random(seed);
    std::normal_distribution<float> gaussian(0,1);
    for (std::uint32_t y = 0; y < raw.extent.height; ++y) for (std::uint32_t x = 0; x < raw.extent.width; ++x) {
        const float truth = irradiance(static_cast<float>(x)-dx,static_cast<float>(y)-dy,
            static_cast<std::size_t>(imaging::cfaChannelAt(cfa,x,y)));
        float value = truth;
        if (shot > 0) value = static_cast<float>(std::poisson_distribution<int>(truth/shot)(random)) * shot;
        if (read > 0) value += std::sqrt(read)*gaussian(random);
        raw.samples[static_cast<std::size_t>(y)*raw.extent.width+x] = {value,shot*truth+read,1,0};
    }
    return raw;
}
void geometryChecks() {
    using namespace latent;
    const auto check=[](bool good,const char* message) { if (!good) throw std::runtime_error(message); };
    reference::TemporalPolicy policy{};
    policy.maximumDisplacement=24;
    auto r=observation(imaging::CfaPattern::RGGB,0,0,0,0,1);
    auto s=r;
    for (std::uint32_t y=0; y<r.extent.height; ++y) for (std::uint32_t x=0; x<r.extent.width; ++x) {
        const auto index=static_cast<std::size_t>(y)*r.extent.width+x;
        constexpr float period=16;
        const auto periodic=[](float xx,float yy) { return 0.3F+0.1F*std::sin(6.28318530718F*xx/period)+0.1F*std::cos(6.28318530718F*yy/period); };
        r.samples[index]={periodic(static_cast<float>(x),static_cast<float>(y)),0.000004F,1,0};
        s.samples[index]={periodic(static_cast<float>(x)-0.37F,static_cast<float>(y)+0.63F),0.000004F,1,0};
    }
    auto a=reference::alignTemporalRaw(r,s,imaging::FrameId{1},imaging::FrameId{2},policy);
    std::cout << "geometry periodic_gap=" << a.globalEvidence.distinctCostGap << " texture=" << a.globalEvidence.textureSupport << " issues=" << a.globalEvidence.issues << '\n';
    check((a.globalEvidence.issues & reference::GeometryIssue::Ambiguous)!=0,"periodic alternatives must not claim unique geometry");
    for (unsigned seed=0; seed<6; ++seed) {
        std::mt19937 random(seed+113);
        std::normal_distribution<float> noise(0,0.01F);
        for (auto& value:r.samples) value={0.2F+noise(random),0.0001F,1,0};
        for (auto& value:s.samples) value={0.2F+noise(random),0.0001F,1,0};
        a=reference::alignTemporalRaw(r,s,imaging::FrameId{1},imaging::FrameId{2},policy);
        check((a.globalEvidence.issues & reference::GeometryIssue::IdentityPrior)!=0,"noise must not supply observable 2D geometry");
        check(a.global.dx==0 && a.global.dy==0,"unobservable global motion uses an explicit identity prior");
    }
    // A one-dimensional edge constrains only one component (aperture problem).
    for (std::uint32_t y=0; y<r.extent.height; ++y) for (std::uint32_t x=0; x<r.extent.width; ++x) {
        const auto index=static_cast<std::size_t>(y)*r.extent.width+x;
        r.samples[index]={0.3F+0.1F*std::sin(0.13F*static_cast<float>(x)),0.00001F,1,0};
        s.samples[index]={0.3F+0.1F*std::sin(0.13F*(static_cast<float>(x)-0.7F)),0.00001F,1,0};
    }
    a=reference::alignTemporalRaw(r,s,imaging::FrameId{1},imaging::FrameId{2},policy);
    check((a.globalEvidence.issues & reference::GeometryIssue::Underconstrained)!=0,"aperture ambiguity must remain explicit");
    r=observation(imaging::CfaPattern::RGGB,0,0,0,0,1); s=r;
    for (std::uint32_t y=0; y<r.extent.height; y+=2) for (std::uint32_t x=0; x<r.extent.width; x+=2)
        s.samples[static_cast<std::size_t>(y)*r.extent.width+x].usable=0;
    a=reference::alignTemporalRaw(r,s,imaging::FrameId{1},imaging::FrameId{2},policy);
    check((a.globalEvidence.issues & reference::GeometryIssue::InsufficientOverlap)!=0,"partial CFA clipping cannot alter proxy spectral weights");
    check(a.global.confidence==0,"incomplete spectral support cannot certify a warp");
    s=r; s.samples[10].value=std::numeric_limits<float>::quiet_NaN();
    bool rejected=false;
    try { (void)reference::alignTemporalRaw(r,s,imaging::FrameId{1},imaging::FrameId{2},policy); }
    catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"non-finite normalized input must fail before search");
    s=r;
    for (std::uint32_t y=0; y<s.extent.height; ++y) for (std::uint32_t x=0; x<s.extent.width; ++x) {
        const float dx=y<80 ? 2.37F : -1.5F;
        const auto c=static_cast<std::size_t>(imaging::cfaChannelAt(s.cfa,x,y));
        s.samples[static_cast<std::size_t>(y)*s.extent.width+x]={irradiance(static_cast<float>(x)-dx,static_cast<float>(y)-0.3F,c),0,1,0};
    }
    a=reference::alignTemporalRaw(r,s,imaging::FrameId{1},imaging::FrameId{2},policy);
    float localMaximum=0;
    for (std::uint32_t ty : {1U,3U}) for (std::uint32_t tx=1; tx+1<a.columns; ++tx) {
        const auto index=static_cast<std::size_t>(ty)*a.columns+tx;
        const auto& tile=a.tiles[index];
        const float dx=ty==1 ? 2.37F : -1.5F;
        const float error=std::hypot(tile.dx-dx,tile.dy-0.3F);
        localMaximum=std::max(localMaximum,error);
        check(std::isfinite(a.tileEvidence[index].cycleErrorPixels),"finite cycle evidence");
    }
    std::cout << "geometry local_motion_max_error=" << localMaximum << '\n';
    check(localMaximum<0.3F,"local motion interiors must preserve distinct translations");
}
}
int main() {
    try {
        using namespace latent;
        geometryChecks();
        double errorSquared = 0, noisyErrorSquared = 0;
        float maximum = 0, noisyMaximum = 0;
        std::size_t cleanCount = 0, noisyCount = 0;
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned p = 0; p < 4; ++p) for (unsigned n = 0; n < 2; ++n) {
            const float shot = n ? 0.0005F : 0, read = n ? 0.00001F : 0;
            const auto cfa = static_cast<imaging::CfaPattern>(p);
            const auto r = observation(cfa,0,0,shot,read,100+p*13+n);
            for (const auto& [dx,dy] : std::array<std::pair<float,float>,4>{{{0.37F,-0.63F},{1.15F,2.43F},{-3.33F,1.72F},{20.4F,-12.2F}}}) {
                const auto s = observation(cfa,dx,dy,shot,read,907+p*11+n);
                const auto a = reference::alignTemporalRaw(r,s,imaging::FrameId{1},imaging::FrameId{2},{});
                const float error = std::hypot(a.global.dx-dx,a.global.dy-dy);
                std::cout << "registration cfa=" << p << " noisy=" << n << " dx=" << dx << " dy=" << dy
                          << " actual_dx=" << a.global.dx << " actual_dy=" << a.global.dy
                          << " endpoint_error=" << error << " confidence=" << a.global.confidence << '\n';
                if (n) { noisyErrorSquared += static_cast<double>(error)*error; noisyMaximum = std::max(noisyMaximum,error); ++noisyCount; }
                else { errorSquared += static_cast<double>(error)*error; maximum = std::max(maximum,error); ++cleanCount; }
            }
        }
        const double rmse = std::sqrt(errorSquared/static_cast<double>(cleanCount));
        const double noisyRmse = std::sqrt(noisyErrorSquared/static_cast<double>(noisyCount));
        std::cout << "registration_summary clean_rmse=" << rmse << " clean_max=" << maximum
                  << " poisson_read_rmse=" << noisyRmse << " poisson_read_max=" << noisyMaximum
                  << " elapsed_ms=" << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count() << '\n';
        // Sensor-pixel endpoint error, not a test against the implementation's own warp.
        // These gates require useful accuracy below the old quarter-pixel grid.
        if (rmse >= 0.08 || maximum >= 0.15 || noisyRmse >= 0.15 || noisyMaximum >= 0.3)
            throw std::runtime_error("continuous subpixel accuracy gate");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
