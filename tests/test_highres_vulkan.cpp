#include "latent/testing/SyntheticRaw.h"
#include "latent/vulkan/DirectReconstruction.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
using namespace latent;
int main() {
    try {
        std::string detail;
        auto available=vulkan::makeVulkanDirectExecutor(4096,256,{},&detail,true);
        if(!available) {
            if(std::getenv("LATENT_REQUIRE_VULKAN")) throw std::runtime_error("Vulkan is mandatory: "+detail);
            std::cout<<"Vulkan highres test skipped: "<<detail<<'\n';return 0;
        }
        available.reset();
        double maxRgb=0,maxVar=0,maxEff=0,maxConfidence=0,gpuMs=0;
        std::uint64_t compared=0;
        for(unsigned scenario=0;scenario<3;++scenario)
        for(std::uint32_t group=1;group<=4;++group)for(std::uint32_t phase=0;phase<4;++phase) {
            const imaging::Extent e{51,37};
            auto burst=testing::syntheticBurst(e,5,group,group,static_cast<imaging::CfaPattern>(phase),3,5,scenario?0.00003F:0.0F,scenario?0.000004F:0.0F);
            if(scenario==2 && group==1)for(auto& member:burst.members) {
                auto& s=*member.observations.sampling;s.representation=imaging::RawRepresentation::RemosaicedBayer;
                s.remosaic=imaging::ProcessingState::Applied;s.physical=imaging::CfaTopology{s.buffer.base,4,4};
            }
            auto motion=testing::translations(5,static_cast<float>(group)*2);
            motion[2].rowDx=0.2F;motion[2].affine[1]=0.001F;
            motion[3].confidence=0.2F;
            burst.members[1].observations.exposureTimeNs*=2;
            testing::SyntheticRawSource source(burst,motion);
            runtime::TiledReconstructionPolicy policy{};policy.tileSize=16;
            runtime::ReconstructionCapabilities cpu{},gpu{};gpu.preferVulkan=true;gpu.collectGpuTimings=true;
            testing::ImageSink a(e),b(e);
            (void)runtime::reconstructRawTiles(burst,source,{e},policy,cpu,a,motion,burst.members[0].id);
            auto trace=runtime::reconstructRawTiles(burst,source,{e},policy,gpu,b,motion,burst.members[0].id);
            if(!trace.plan.vulkan || trace.mappedAccessBytes==0 || trace.transferBytes!=0 || trace.plan.deviceWorkingBytes==0)
                throw std::runtime_error("GPU test silently fell back");
            if(trace.gpuMilliseconds>=0)gpuMs+=trace.gpuMilliseconds;
            for(std::size_t i=0;i<a.pixels.size();++i)for(std::size_t c=0;c<3;++c) {
                auto diff=[](float x,float y){return static_cast<double>(std::abs(x-y));};
                const double rgb=diff(a.pixels[i].rgb[c],b.pixels[i].rgb[c]);
                const double var=diff(a.pixels[i].variance[c],b.pixels[i].variance[c]);
                const double eff=diff(a.pixels[i].effectiveFrames[c],b.pixels[i].effectiveFrames[c]);
                const double confidence=diff(a.pixels[i].confidence[c],b.pixels[i].confidence[c]);
                maxRgb=std::max(maxRgb,rgb);maxVar=std::max(maxVar,var);maxEff=std::max(maxEff,eff);maxConfidence=std::max(maxConfidence,confidence);
                if(rgb>3e-5 || var>1e-7+1e-3*a.pixels[i].variance[c] || eff>2e-3 || confidence>5e-4)
                    throw std::runtime_error("CPU/GPU mismatch group="+std::to_string(group)+" rgb="+std::to_string(rgb)+
                        " variance="+std::to_string(var)+" effective="+std::to_string(eff)+" confidence="+std::to_string(confidence));
                ++compared;
            }
        }
        std::cout<<"direct CPU/GPU channels="<<compared<<" max_rgb_error="<<maxRgb<<" max_variance_error="<<maxVar
            <<" max_effective_error="<<maxEff<<" max_confidence_error="<<maxConfidence<<" gpu_ms="<<gpuMs<<" PASS\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
