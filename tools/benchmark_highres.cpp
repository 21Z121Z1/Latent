#include "latent/testing/SyntheticRaw.h"
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#ifdef __linux__
#include <sys/resource.h>
#endif
using namespace latent;
namespace {
struct Options {std::uint32_t width=512,height=384,frames=4,group=1,tile=128;bool gpu=false,estimated=false,chart=false;};
// Noise-free colored constant RAW. The source is cheap, but reconstruction is
// NOT bypassed; every output pixel executes the normal kernels/halos/fusion.
class FlatRaw final:public runtime::RawTileSource {
public:
    explicit FlatRaw(const imaging::RawBurst& burst):burst_(burst) {
        const auto& s=*burst_.members.front().observations.sampling;
        px_=s.buffer.groupX*2;py_=s.buffer.groupY*2;
        pattern_.resize(static_cast<std::size_t>(px_)*py_);
        for(std::uint32_t y=0;y<py_;++y)for(std::uint32_t x=0;x<px_;++x) {
            const auto c=static_cast<std::size_t>(imaging::samplingChannelAt(s,x,y));
            const float v=c==0?0.2F:(c==3?0.4F:0.3F);
            const float black=burst_.members[0].observations.staticBlack.value->cfa[c];
            pattern_[static_cast<std::size_t>(y)*px_+x]=static_cast<std::uint16_t>(std::lround(black+(4095-black)*v));
        }
    }
    void read(imaging::FrameId id,imaging::SensorRect r,std::span<std::uint16_t> output) override {
        if(!id.value||id.value>burst_.members.size()||r.width>burst_.extent.width-r.x||r.height>burst_.extent.height-r.y||
            output.size()!=static_cast<std::size_t>(r.width)*r.height)throw std::invalid_argument("benchmark footprint");
        for(std::uint32_t y=0;y<r.height;++y) {
            const auto row=static_cast<std::size_t>((r.y+y)%py_)*px_;
            for(std::uint32_t x=0;x<r.width;) {
                const auto offset=(r.x+x)%px_,count=std::min(px_-offset,r.width-x);
                std::copy_n(pattern_.data()+row+offset,count,output.data()+static_cast<std::size_t>(y)*r.width+x);x+=count;
            }
        }
    }
    std::uint64_t residentBytes() const override {return pattern_.capacity()*sizeof(std::uint16_t);}
private:
    const imaging::RawBurst& burst_;std::uint32_t px_=0,py_=0;std::vector<std::uint16_t> pattern_;
};
struct ChecksumSink:runtime::ReconstructionTileSink {
    double checksum=0;std::uint64_t count=0;
    std::uint64_t residentBytes() const override {return 0;}
    void write(imaging::SensorRect,std::span<const reference::ReconstructedPixel> pixels) override {
        for(const auto& p:pixels)for(std::size_t c=0;c<3;++c) {
            if(!std::isfinite(p.rgb[c]) || !std::isfinite(p.variance[c]) || p.confidence[c]<=0)
                throw std::runtime_error("invalid benchmark reconstruction");
            checksum+=p.rgb[c];++count;
        }
    }
};
std::int64_t peakRss() {
#ifdef __linux__
    rusage usage{};if(getrusage(RUSAGE_SELF,&usage)==0)return static_cast<std::int64_t>(usage.ru_maxrss)*1024;
#endif
    return -1;
}
void run(Options o) {
    const auto burst=testing::syntheticBurst({o.width,o.height},o.frames,o.group,o.group,imaging::CfaPattern::RGGB,0,0,0,1e-5F);
    const auto motion=testing::translations(o.frames,o.chart?static_cast<float>(o.group):0.0F);
    std::unique_ptr<runtime::RawTileSource> source;
    if(o.chart)source=std::make_unique<testing::SyntheticRawSource>(burst,motion);
    else source=std::make_unique<FlatRaw>(burst);
    ChecksumSink sink;
    runtime::TiledReconstructionPolicy policy{};policy.tileSize=o.tile;policy.maximumFrames=o.frames;
    runtime::ReconstructionCapabilities caps{};caps.hostBudgetBytes=128U*1024U*1024U;caps.preferVulkan=o.gpu;caps.collectGpuTimings=true;
    const auto before=peakRss();
    const auto trace=runtime::reconstructRawTiles(burst,*source,{burst.extent},policy,caps,sink,
        o.estimated?std::span<const runtime::ReconstructionMotion>{}:std::span(motion),imaging::FrameId{1});
    if(o.gpu&&!trace.plan.vulkan)throw std::runtime_error("requested GPU benchmark fell back: "+runtime::directReconstructionJson(trace));
    if(sink.count!=burst.extent.pixelCount()*3)throw std::runtime_error("benchmark output count mismatch");
    std::cout<<"{\"width\":"<<o.width<<",\"height\":"<<o.height<<",\"group\":"<<o.group
        <<",\"source\":\""<<(o.chart?"analytic_area_chart":"periodic_flat_raw16")<<"\",\"registration\":\""
        <<(o.estimated?"estimated":"supplied")<<"\",\"peak_rss_before_bytes\":"<<before<<",\"peak_rss_after_bytes\":"<<peakRss()
        <<",\"checksum\":"<<sink.checksum<<",\"trace\":"<<runtime::directReconstructionJson(trace)<<"}\n"<<std::flush;
}
}
int main(int argc,char** argv) {
    try {
        Options o;bool suite=false;
        for(int i=1;i<argc;++i) {
            const std::string key=argv[i];
            if(key=="--suite"){suite=true;continue;}if(key=="--gpu"){o.gpu=true;continue;}
            if(key=="--estimated"){o.estimated=true;continue;}if(key=="--chart"){o.chart=true;continue;}
            if(i+1>=argc)throw std::invalid_argument("missing numeric option value");
            std::size_t consumed=0;const std::string value=argv[++i];const auto n=std::stoul(value,&consumed);
            if(consumed!=value.size()||!n||n>1000000)throw std::invalid_argument("invalid benchmark integer");
            const auto number=static_cast<std::uint32_t>(n);
            if(key=="--width")o.width=number;else if(key=="--height")o.height=number;else if(key=="--frames")o.frames=number;
            else if(key=="--group")o.group=number;else if(key=="--tile")o.tile=number;else throw std::invalid_argument("unknown option");
        }
        if(o.frames>32||o.group>16)throw std::invalid_argument("benchmark policy exceeds supported bounds");
        if(suite) {for(auto g:{1U,2U,3U,4U})for(auto n:{1U,4U}){o.group=g;o.frames=n;run(o);}}
        else run(o);
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
