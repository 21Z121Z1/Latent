#include "latent/testing/SyntheticRaw.h"
#include "latent/reference/RawNormalize.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace latent;
namespace {
std::size_t assertions=0;
void check(bool v,const char* message){++assertions;if(!v)throw std::runtime_error(message);}
template<class F>void rejects(F&& f,const char* message){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,message);}
float maximumDifference(const testing::ImageSink& a,const testing::ImageSink& b) {
    float difference=0;
    for(std::size_t i=0;i<a.pixels.size();++i)for(std::size_t c=0;c<3;++c){
        difference=std::max(difference,std::abs(a.pixels[i].rgb[c]-b.pixels[i].rgb[c]));
        difference=std::max(difference,std::abs(a.pixels[i].variance[c]-b.pixels[i].variance[c]));}
    return difference;
}
runtime::DirectReconstructionTrace run(const imaging::RawBurst& b,std::span<const runtime::ReconstructionMotion> motion,
    runtime::RawTileSource& src,testing::ImageSink& sink,std::uint32_t tile=32) {
    runtime::TiledReconstructionPolicy p{};p.tileSize=tile;p.maximumFrames=32;
    runtime::ReconstructionGrid grid{sink.extent};
    if(sink.extent.width!=b.extent.width){grid.stepX=0.5F;grid.stepY=0.5F;}
    return runtime::reconstructRawTiles(b,src,grid,p,{},sink,motion,imaging::FrameId{1});
}
void samplingProperties() {
    for(std::uint32_t gx=1;gx<=4;++gx)for(std::uint32_t gy=1;gy<=4;++gy)for(unsigned phase=0;phase<4;++phase) {
        auto b=testing::syntheticBurst({59,47},1,gx,gy,static_cast<imaging::CfaPattern>(phase));
        auto s=*b.members[0].observations.sampling;
        for(std::uint32_t cy=0;cy<2*gy;++cy)for(std::uint32_t cx=0;cx<2*gx;++cx) {
            const auto crop=imaging::cropSampling(s,b.extent,{cx,cy,31,29});
            for(std::uint32_t y=0;y<13;++y)for(std::uint32_t x=0;x<13;++x)
                check(imaging::samplingChannelAt(crop,x,y)==imaging::samplingChannelAt(s,x+cx,y+cy),"crop+phase invariance");
            check(crop.bufferToCalibration.translateX==cx&&crop.bufferToCalibration.translateY==cy,"crop calibration mapping");
        }
        s.representation=imaging::RawRepresentation::Unknown;check(!imaging::validateSampling(s,b.extent).valid,"unknown representation must fail closed");
    }
    auto b=testing::syntheticBurst({32,24},2,4,4);
    b.members[1].observations.sampling->originX=1;
    check(!imaging::validateRawBurst(b).valid(),"mixed phase burst rejected");
    b.members[1].observations=b.members[0].observations;b.members[1].observations.sensorTimestampNs+=100;
    b.members[1].observations.sampling->zoomRatio=2;
    check(!imaging::validateRawBurst(b).valid(),"mixed zoom burst rejected");
}
void tiledProperties() {
    const testing::Signal flat=[](float,float,std::size_t c,std::size_t){return 0.18F+0.12F*static_cast<float>(c);};
    for(std::uint32_t group=1;group<=4;++group)for(unsigned phase=0;phase<4;++phase) {
        const auto b=testing::syntheticBurst({39,35},4,group,group,static_cast<imaging::CfaPattern>(phase),3,5);
        const auto motions=testing::translations(4,static_cast<float>(group));
        testing::SyntheticRawSource source(b,motions,flat);
        testing::ImageSink tiled(b.extent),full(b.extent);
        const auto t=run(b,motions,source,tiled,16);(void)run(b,motions,source,full,64);
        check(maximumDifference(tiled,full)==0,"full-frame/tiled exact equivalence");
        check(t.plan.hostWorkingBytes<64U*1024U*1024U,"bounded arena");
        for(const auto& pixel:tiled.pixels)for(std::size_t c=0;c<3;++c)check(std::abs(pixel.rgb[c]-flat(0,0,c,0))<0.0003F,"constant field preservation, phase and edges");
    }
}
void quality() {
    for(std::uint32_t group=1;group<=4;++group) {
        auto b=testing::syntheticBurst({97,81},8,group,group,imaging::CfaPattern::GBRG,3,1,0.0001F,0.00004F);
        auto m=testing::translations(8,static_cast<float>(group)*2);
        testing::SyntheticRawConfig c{};c.shot=0.0001F;c.read=0.00004F;
        testing::SyntheticRawSource source(b,m,testing::chart,c);
        testing::ImageSink fused(b.extent),single(b.extent);const auto trace=run(b,m,source,fused);
        b.members.resize(1);m.resize(1);testing::SyntheticRawSource source1(b,m,testing::chart,c);(void)run(b,m,source1,single);
        const auto multiPsnr=testing::psnr(fused),singlePsnr=testing::psnr(single);
        std::cout<<"quality group="<<group<<" single_psnr="<<singlePsnr<<" direct_burst_psnr="<<multiPsnr<<" effective_frames="<<trace.meanEffectiveFrames<<'\n';
        check(multiPsnr>singlePsnr,"burst quality must improve measured chart PSNR");
    }
}
void exposureClippingAndMotion() {
    const testing::Signal flat=[](float,float,std::size_t c,std::size_t){return c==0?0.3F:(c==1?0.4F:0.5F);};
    auto b=testing::syntheticBurst({45,39},3,2,2);auto m=testing::translations(3,2);
    b.members[1].observations.exposureTimeNs=5000000;b.members[2].observations.exposureTimeNs=20000000;
    b.members[1].observations.sensitivityIso=200;b.members[1].observations.exposureCalibration.effectiveGain.fill(2);
    testing::SyntheticRawSource src(b,m,flat);testing::ImageSink image(b.extent);
    (void)run(b,m,src,image);
    for(std::uint32_t y=8;y<30;++y)for(std::uint32_t x=8;x<35;++x)for(std::size_t c=0;c<3;++c)
        check(std::abs(image.pixels[static_cast<std::size_t>(y)*b.extent.width+x].rgb[c]-flat(0,0,c,0))<0.0003F,"exposure/gain normalization");
    const testing::Signal clipped=[](float,float,std::size_t,std::size_t){return 2.0F;};
    b=testing::syntheticBurst({32,24},1);m=testing::translations(1);
    testing::SyntheticRawSource saturation(b,m,clipped);testing::ImageSink invalid(b.extent);const auto t=run(b,m,saturation,invalid);
    check(t.invalidChannels==b.extent.pixelCount()*3,"saturated data must not be invented");
    // Foreground appearing in only the source must not ghost into a static base.
    b=testing::syntheticBurst({64,48},4);m=testing::translations(4,0);
    const testing::Signal moving=[](float x,float y,std::size_t,std::size_t f){return f>0&&x>20&&x<42&&y>14&&y<34?0.8F:0.2F;};
    testing::SyntheticRawSource dynamic(b,m,moving);testing::ImageSink result(b.extent);(void)run(b,m,dynamic,result);
    for(std::uint32_t y=18;y<30;++y)for(std::uint32_t x=24;x<38;++x)for(std::size_t c=0;c<3;++c)
        check(std::abs(result.pixels[static_cast<std::size_t>(y)*64+x].rgb[c]-0.2F)<0.0003F,"moving-object/occlusion rejection");
    // Small affine rotation + scale + row skew are admitted observations.
    b=testing::syntheticBurst({80,64},2);m=testing::translations(2);
    m[1].affine={1.002F,-0.004F,0.2F,0.004F,1.002F,-0.2F};m[1].rowDx=0.4F;
    testing::SyntheticRawSource rotation(b,m);testing::ImageSink rotated(b.extent);(void)run(b,m,rotation,rotated);
    check(testing::psnr(rotated)>32,"affine and row-skew reconstruction");
    m[1].affine[2]=1000;rejects([&]{(void)run(b,m,rotation,rotated);},"motion footprint must obey admission halo");
}
void budgetsAndFailures() {
    for(auto e:{imaging::Extent{4000,3000},imaging::Extent{8192,6144},imaging::Extent{16384,12288}}) {
        auto b=testing::syntheticBurst(e,8,4,4);runtime::TiledReconstructionPolicy p{};runtime::ReconstructionCapabilities caps{};
        caps.hostBudgetBytes=64U*1024U*1024U;
        const auto plan=runtime::planTiledReconstruction(b,{e},p,caps,0);
        check(plan.hostWorkingBytes<=caps.hostBudgetBytes,"12/50/200MP bounded working set");
        caps.thermalSeverity=2;check(runtime::planTiledReconstruction(b,{e},p,caps,0).frames==1,"thermal degradation");
        p.allowFrameReduction=false;rejects([&]{(void)runtime::planTiledReconstruction(b,{e},p,caps,0);},"fixed membership cannot be silently changed");
    }
    const auto b=testing::syntheticBurst({32,24},1);auto m=testing::translations(1);testing::SyntheticRawSource src(b,m);testing::ImageSink sink(b.extent);
    runtime::ReconstructionCapabilities caps{};caps.hostBudgetBytes=1;
    rejects([&]{(void)runtime::reconstructRawTiles(b,src,{b.extent},{},caps,sink,m,imaging::FrameId{1});},"OOM admission");
    rejects([&]{(void)runtime::reconstructRawTiles(b,src,{b.extent},{},{},sink,m,imaging::FrameId{1},[]{return false;});},"cancellation");
    auto bad=b;bad.members[0].observations.sampling->representation=imaging::RawRepresentation::Unknown;
    rejects([&]{(void)run(bad,m,src,sink);},"unknown representation fail closed");
}
void automaticRegistration() {
    const testing::Signal smooth=[](float x,float y,std::size_t c,std::size_t){return 0.3F+0.02F*static_cast<float>(c)+
        0.1F*std::sin(x*0.035F+y*0.017F)+0.08F*std::cos(y*0.052F-x*0.012F);};
    for(std::uint32_t group=1;group<=4;++group) {
        const auto burst=testing::syntheticBurst({192,144},2,group,group,imaging::CfaPattern::GRBG,3,1,0,1e-5F);
        auto m=testing::translations(2);m[1].affine[2]=1.73F;m[1].affine[5]=-2.27F;
        testing::SyntheticRawSource source(burst,m,smooth);testing::ImageSink sink(burst.extent);
        runtime::TiledReconstructionPolicy policy{};policy.maximumDisplacement=12;
        const auto trace=runtime::reconstructRawTiles(burst,source,{burst.extent},policy,{},sink,{},imaging::FrameId{1});
        const auto& field=trace.motion[1].residual;
        check(!field.empty(),"automatic registration did not produce a field");
        const auto& estimate=field.front();
        const auto error=std::hypot(estimate.dx-1.73F,estimate.dy+2.27F);
        std::cout<<"registration group="<<group<<" dx="<<estimate.dx<<" dy="<<estimate.dy
            <<" epe="<<error<<" confidence="<<estimate.confidence<<'\n';
        check(error<0.65F,"grouped RAW guide subpixel registration error");
        check(estimate.confidence>0.15F,"observable guide unexpectedly rejected");
        check(testing::psnr(sink,smooth)>38,"automatic registration image quality");
    }
}
void greenCalibration() {
    for(std::uint32_t g=1;g<=4;++g)for(unsigned phase=0;phase<4;++phase) {
        auto b=testing::syntheticBurst({35,31},2,g,g,static_cast<imaging::CfaPattern>(phase),3,1);
        // Deliberate 10% split, much larger than the quantization floor.
        for(auto& m:b.members)m.observations.colorCorrectionGains={std::array<float,4>{2,1.1F,0.9F,3},
            imaging::MetadataSource::MeasuredCalibration,imaging::MetadataValidity::Valid,1};
        auto motion=testing::translations(2,static_cast<float>(g));testing::SyntheticRawConfig config{};
        config.greenResponse={1/1.1F,1/0.9F};
        const testing::Signal flat=[](float,float,std::size_t,std::size_t){return 0.3F;};
        testing::SyntheticRawSource src(b,motion,flat,config);testing::ImageSink sink(b.extent);
        (void)run(b,motion,src,sink);
        for(const auto& p:sink.pixels)for(std::size_t c=0;c<3;++c)
            check(std::abs(p.rgb[c]-0.3F)<0.0003F,"G0/G1 calibration must precede RGB merge");
    }
}
void fileSource() {
    const auto b=testing::syntheticBurst({35,27},1);const auto m=testing::translations(1);testing::SyntheticRawSource src(b,m);
    std::vector<std::uint16_t> raw(static_cast<std::size_t>(b.extent.pixelCount()));src.read(imaging::FrameId{1},{0,0,35,27},raw);
    const auto path=std::filesystem::temp_directory_path()/"latent-highres-fixture.raw16";
    {std::ofstream f(path,std::ios::binary);for(auto v:raw){f.put(static_cast<char>(v&255));f.put(static_cast<char>(v>>8));}}
    const std::array<runtime::RawFileBinding,1> files{{{imaging::FrameId{1},path.string(),0,35}}};
    auto disk=runtime::makeFileTileSource(b,files);testing::ImageSink a(b.extent),d(b.extent);
    (void)run(b,m,src,a);(void)run(b,m,*disk,d);check(maximumDifference(a,d)==0,"file/analytic source equivalence");
    std::filesystem::resize_file(path,4);rejects([&]{(void)runtime::makeFileTileSource(b,files);},"truncated file admission");
    rejects([&]{std::vector<std::uint16_t> row(35);disk->read(imaging::FrameId{1},{0,26,35,1},row);},"short read after file mutation");
    std::filesystem::remove(path);
}
}
int main(){try{samplingProperties();tiledProperties();quality();exposureClippingAndMotion();budgetsAndFailures();automaticRegistration();greenCalibration();fileSource();
    std::cout<<"highres assertions="<<assertions<<" PASS\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
