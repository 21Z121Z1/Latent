#pragma once
// Deterministic analytic fixtures. No captured images or vendor code. The
// forward model integrates a continuous irradiance signal over pixel area,
// applies exposure/gain, shot+read noise, ADC quantization and sensor clipping.
#include "latent/runtime/TiledReconstruction.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>

namespace latent::testing {
using Signal=std::function<float(float,float,std::size_t,std::size_t)>;
inline float chart(float x,float y,std::size_t c,std::size_t) {
    return 0.28F+0.08F*static_cast<float>(c)+0.09F*std::sin(0.12F*x+0.03F*y)+
        0.08F*std::cos(0.041F*x-0.19F*y)+0.06F*std::sin(0.55F*x+0.44F*y+static_cast<float>(c));
}
inline float uniform(std::uint32_t& state) {
    state^=state<<13U;state^=state>>17U;state^=state<<5U;
    return (static_cast<float>(state&0xffffffU)+0.5F)/16777216.0F;
}
inline float gaussian(std::uint32_t& state) {return std::sqrt(-2*std::log(uniform(state)))*std::cos(6.283185307F*uniform(state));}
struct SyntheticRawConfig {
    float shot = 0, read = 0;
    std::uint32_t seed = 419U;
    float sceneOriginX = 0, sceneOriginY = 0;
    bool clip = true;
    std::array<float,2> greenResponse{1,1};
};
class SyntheticRawSource final:public runtime::RawTileSource {
public:
    SyntheticRawSource(imaging::RawBurst burst,std::vector<runtime::ReconstructionMotion> motion,
        Signal signal=chart,SyntheticRawConfig config={}):burst_(std::move(burst)),motion_(std::move(motion)),signal_(std::move(signal)),config_(config) {
        if(motion_.size()!=burst_.members.size())throw std::invalid_argument("synthetic motion mismatch");
    }
    void read(imaging::FrameId id,imaging::SensorRect r,std::span<std::uint16_t> o) override {
        const auto it=std::find_if(burst_.members.begin(),burst_.members.end(),[&](const auto& m){return m.id==id;});
        if(it==burst_.members.end() || r.x>=burst_.extent.width || r.y>=burst_.extent.height ||
            !r.width||!r.height||r.width>burst_.extent.width-r.x||r.height>burst_.extent.height-r.y||
            o.size()!=static_cast<std::size_t>(r.width)*r.height)throw std::invalid_argument("invalid synthetic read");
        const auto frame=static_cast<std::size_t>(it-burst_.members.begin());const auto& meta=it->observations;
        const auto sampling=meta.sampling.value_or(imaging::regularSampling(burst_.extent,meta.cfa));
        const auto& m=motion_[frame];
        // Row-linear rolling shutter is affine in this fixture. General/local
        // object deformation belongs to signal(frame), not fake camera motion.
        const float a=m.affine[0],b=m.affine[1]+m.rowDx/static_cast<float>(std::max(1U,burst_.extent.height-1));
        const float c=m.affine[3],d=m.affine[4]+m.rowDy/static_cast<float>(std::max(1U,burst_.extent.height-1));
        const float tx=m.affine[2]-0.5F*m.rowDx,ty=m.affine[5]-0.5F*m.rowDy,det=a*d-b*c;
        if(std::abs(det)<0.1F || m.tileSize)throw std::invalid_argument("unsupported synthetic camera warp");
        for(std::uint32_t y=0;y<r.height;++y)for(std::uint32_t x=0;x<r.width;++x) {
            const auto sx=x+r.x,sy=y+r.y;
            const auto channel=static_cast<std::size_t>(imaging::samplingChannelAt(sampling,sx,sy));
            const auto rgb=channel==3?2U:(channel==0?0U:1U);
            float value=0;
            // 2x2 quadrature of unit pixel footprint; the truth uses the same
            // finite aperture, not an impossible delta-sampled optical target.
            for(float oy:{-0.25F,0.25F})for(float ox:{-0.25F,0.25F}) {
                const float u=static_cast<float>(sx)+ox-tx,v=static_cast<float>(sy)+oy-ty;
                const float rx=(d*u-b*v)/det,ry=(-c*u+a*v)/det;
                value+=signal_(rx+config_.sceneOriginX,ry+config_.sceneOriginY,rgb,frame)*0.25F;
            }
            if(channel==1||channel==2)value*=config_.greenResponse[channel-1];
            value*=static_cast<float>(meta.exposureTimeNs)/10000000.0F*meta.exposureCalibration.effectiveGain[channel];
            std::uint32_t rng=config_.seed^(sx*0x9e3779b9U)^(sy*0x85ebca6bU)^(static_cast<std::uint32_t>(id.value)*0xc2b2ae35U);
            if(rng==0)rng=1;
            const float variance=config_.shot*std::max(value,0.0F)+config_.read;
            if(variance>0)value+=std::sqrt(variance)*gaussian(rng);
            const float black=meta.staticBlack.value->cfa[channel];
            const float white=*meta.staticWhite.value;
            const float code=std::clamp(std::round(black+(white-black)*value),0.0F,config_.clip?white:65535.0F);
            o[static_cast<std::size_t>(y)*r.width+x]=static_cast<std::uint16_t>(code);
        }
    }
    std::uint64_t residentBytes() const override {return 0;}
    const imaging::RawBurst& burst() const {return burst_;}
private:
    imaging::RawBurst burst_;std::vector<runtime::ReconstructionMotion> motion_;Signal signal_;SyntheticRawConfig config_;
};
inline imaging::RawBurst syntheticBurst(imaging::Extent e,std::uint32_t frames,std::uint32_t gx=1,std::uint32_t gy=1,
    imaging::CfaPattern phase=imaging::CfaPattern::RGGB,std::uint32_t originX=0,std::uint32_t originY=0,
    float shot=0,float read=0) {
    imaging::RawBurst b{imaging::BurstId{71},imaging::CaptureSequenceId{3},imaging::CalibrationId{12},e,{}};
    auto sampling=imaging::regularSampling(e,phase);
    sampling.buffer.groupX=gx;sampling.buffer.groupY=gy;sampling.originX=originX;sampling.originY=originY;
    sampling.representation=(gx==1&&gy==1)?imaging::RawRepresentation::Bayer:imaging::RawRepresentation::GroupedBayer;
    sampling.physical=sampling.buffer;sampling.binning=imaging::ProcessingState::NotApplied;sampling.remosaic=imaging::ProcessingState::NotApplied;
    for(std::uint32_t i=0;i<frames;++i) {
        imaging::RawFrameMetadata m{};m.cameraId="synthetic";m.sensorMode="analytic-area-integrated-v1";
        m.sensorTimestampNs=static_cast<std::int64_t>(i+1)*100000000;m.exposureTimeNs=10000000;m.sensitivityIso=100;
        m.cfa=phase;m.sampling=sampling;
        m.staticBlack={imaging::BlackLevel{{64,65,66,67}},imaging::MetadataSource::MeasuredCalibration,imaging::MetadataValidity::Valid,1};
        m.staticWhite={4095,imaging::MetadataSource::MeasuredCalibration,imaging::MetadataValidity::Valid,1};
        imaging::NoiseModel n{};n.coordinate=imaging::NoiseCoordinate::NormalizedBlackSubtracted;n.shot.fill(shot);n.read.fill(read);
        m.noiseProfile={n,imaging::MetadataSource::MeasuredCalibration,imaging::MetadataValidity::Valid,1};
        m.exposureCalibration.source=imaging::MetadataSource::MeasuredCalibration;m.exposureCalibration.gainUncertainty=0;
        b.members.push_back({imaging::FrameId{i+1},std::move(m)});
    }
    return b;
}
inline std::vector<runtime::ReconstructionMotion> translations(std::uint32_t n,float span=1.5F) {
    std::vector<runtime::ReconstructionMotion> m(n);
    for(std::uint32_t i=0;i<n;++i){m[i].frame=imaging::FrameId{i+1};if(i){m[i].affine[2]=std::fmod(static_cast<float>(i)*0.754877666F,1.0F)*span;
        m[i].affine[5]=std::fmod(static_cast<float>(i)*0.569840296F,1.0F)*span;}}
    return m;
}
inline float truth(const Signal& signal,float x,float y,std::size_t c) {
    float result=0;for(float oy:{-0.25F,0.25F})for(float ox:{-0.25F,0.25F})result+=0.25F*signal(x+ox,y+oy,c,0);return result;
}
struct ImageSink:runtime::ReconstructionTileSink {
    imaging::Extent extent;std::vector<reference::ReconstructedPixel> pixels;
    explicit ImageSink(imaging::Extent e):extent(e),pixels(static_cast<std::size_t>(e.pixelCount())){}
    std::uint64_t residentBytes() const override {return pixels.capacity()*sizeof(reference::ReconstructedPixel);}
    void write(imaging::SensorRect r,std::span<const reference::ReconstructedPixel> tile) override {
        if(r.x>=extent.width||r.y>=extent.height||r.width>extent.width-r.x||r.height>extent.height-r.y||tile.size()!=static_cast<std::size_t>(r.width)*r.height)
            throw std::invalid_argument("invalid output tile");
        for(std::uint32_t y=0;y<r.height;++y)std::copy_n(tile.begin()+static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y)*r.width),r.width,
            pixels.begin()+static_cast<std::ptrdiff_t>(static_cast<std::size_t>(r.y+y)*extent.width+r.x));
    }
};
inline double psnr(const ImageSink& s,const Signal& signal=chart,std::uint32_t border=8,float step=1) {
    double error=0,count=0;
    for(std::uint32_t y=border;y+border<s.extent.height;++y)for(std::uint32_t x=border;x+border<s.extent.width;++x)for(std::size_t c=0;c<3;++c) {
        const double d=s.pixels[static_cast<std::size_t>(y)*s.extent.width+x].rgb[c]-truth(signal,static_cast<float>(x)*step,static_cast<float>(y)*step,c);
        error+=d*d;count+=1;
    }
    if(count==0)throw std::invalid_argument("PSNR requires interior pixels");
    return -10*std::log10(std::max(1e-20,error/count));
}
} // namespace latent::testing
