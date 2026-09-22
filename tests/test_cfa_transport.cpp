#include "latent/runtime/CfaTransport.h"
#include "latent/reference/RawNormalize.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace latent;
void require(bool valid, const char* why) { if (!valid) throw std::runtime_error(why); }
int main() {
    try {
        for (unsigned p = 0; p < 4; ++p) for (unsigned oy = 0; oy < 2; ++oy) for (unsigned ox = 0; ox < 2; ++ox) {
            const auto original = static_cast<imaging::CfaPattern>(p);
            const auto map = runtime::cfaTransportMap(original, ox, oy);
            auto permutation = map.layoutIndex;
            std::sort(permutation.begin(), permutation.end());
            require(permutation == std::array<std::size_t,4>{0,1,2,3}, "layout permutation");
            permutation = map.sensorChannel;
            std::sort(permutation.begin(), permutation.end());
            require(permutation == std::array<std::size_t,4>{0,1,2,3}, "channel permutation");
            const std::array<float,4> black{64,71,87,98}, signal{0.125F,0.25F,0.375F,0.5F};
            imaging::RawFrame raw{};
            raw.cfa = map.pattern; raw.exposureTimeNs = 1000000; raw.sensitivityIso = 100;
            raw.storage.extent = {7,5}; raw.storage.rowStridePixels = 11;
            raw.storage.pixels.resize(55, 65535);
            imaging::BlackLevel level{};
            for (std::size_t c = 0; c < 4; ++c) level.cfa[c] = black[map.layoutIndex[c]];
            raw.staticBlack = {level,imaging::MetadataSource::StaticCharacteristic,imaging::MetadataValidity::Valid,1};
            raw.staticWhite = {4096,imaging::MetadataSource::StaticCharacteristic,imaging::MetadataValidity::Valid,1};
            for (unsigned y = 0; y < 5; ++y) for (unsigned x = 0; x < 7; ++x) {
                const auto channel = static_cast<std::size_t>(imaging::cfaChannelAt(original,x+ox,y+oy));
                const auto layout = ((y+oy)&1U)*2U+((x+ox)&1U);
                const auto view = static_cast<std::size_t>(imaging::cfaChannelAt(map.pattern,x,y));
                require(map.sensorChannel[view] == channel && map.layoutIndex[view] == layout, "view channel matches physical sensor site");
                raw.storage.pixels[y*11+x] = static_cast<std::uint16_t>(black[layout]+signal[channel]*(4096-black[layout]));
            }
            const auto normalized=reference::normalizeRaw(raw);
            for (unsigned y = 0; y < 5; ++y) for (unsigned x = 0; x < 7; ++x) {
                const auto channel=static_cast<std::size_t>(imaging::cfaChannelAt(original,x+ox,y+oy));
                require(std::abs(normalized.samples[y*7+x]-signal[channel]) < 0.00026F, "nonuniform black and odd strided extent preserve channels");
            }
        }
        bool rejected=false;
        try { (void)runtime::cfaTransportMap(static_cast<imaging::CfaPattern>(255),0,0); }
        catch(const std::invalid_argument&) { rejected=true; }
        require(rejected,"invalid CFA rejected");
        std::cout << "all Bayer layouts and crop phases preserve layout/channel metadata\n";
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
