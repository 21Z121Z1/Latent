#include "latent/runtime/TiledReconstruction.h"
#include "latent/reference/NoisePropagation.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>

namespace latent::runtime {
namespace {
void validRead(imaging::Extent e,imaging::SensorRect r,std::size_t count) {
    if(r.x>=e.width||r.y>=e.height||r.width==0||r.height==0||r.width>e.width-r.x||r.height>e.height-r.y||
       static_cast<std::uint64_t>(r.width)*r.height!=count) throw std::invalid_argument("invalid RAW tile read");
}
class HostSource final:public RawTileSource {
public:
    HostSource(const imaging::RawBurst& b,const HostRawBindings& h):burst_(b),bindings_(h) {
        const auto v=h.validate(b);if(!v.valid)throw std::invalid_argument(v.message);
        for(const auto& m:b.members) bytes_+=h.view(b,m.id).storage.pixels.size()*sizeof(std::uint16_t);
    }
    void read(imaging::FrameId id,imaging::SensorRect r,std::span<std::uint16_t> o) override {
        validRead(burst_.extent,r,o.size()); const auto v=bindings_.view(burst_,id);
        for(std::uint32_t y=0;y<r.height;++y) std::copy_n(v.storage.pixels.begin()+
            static_cast<std::ptrdiff_t>(static_cast<std::size_t>(r.y+y)*v.storage.rowStridePixels+r.x),r.width,
            o.begin()+static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y)*r.width));
    }
    std::uint64_t residentBytes() const override {return bytes_;}
private:
    const imaging::RawBurst& burst_;const HostRawBindings& bindings_;std::uint64_t bytes_=0;
};
class FileSource final:public RawTileSource {
public:
    struct File {RawFileBinding binding;std::ifstream stream;};
    FileSource(const imaging::RawBurst& b,std::span<const RawFileBinding> files):extent_(b.extent) {
        const auto v=imaging::validateRawBurst(b);if(!v.valid())throw std::invalid_argument(v.message);
        if(files.size()!=b.members.size())throw std::invalid_argument("RAW file membership mismatch");
        for(const auto& f:files) {
            if(f.rowStridePixels<b.extent.width || f.offsetBytes>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())/2 ||
                std::none_of(b.members.begin(),b.members.end(),[&](const auto& m){return m.id==f.frame;}))
                throw std::invalid_argument("invalid RAW file binding");
            if(files_.contains(f.frame))throw std::invalid_argument("duplicate RAW file identity");
            File file{f,std::ifstream(f.path,std::ios::binary)};
            if(!file.stream)throw std::runtime_error("cannot open RAW tile source");
            file.stream.seekg(0,std::ios::end);
            const auto size=file.stream.tellg();
            const auto needed=f.offsetBytes+2*(static_cast<std::uint64_t>(f.rowStridePixels)*(b.extent.height-1)+b.extent.width);
            if(size<0 || static_cast<std::uint64_t>(size)<needed)throw std::invalid_argument("RAW file is truncated");
            files_.emplace(f.frame,std::move(file));
        }
    }
    void read(imaging::FrameId id,imaging::SensorRect r,std::span<std::uint16_t> o) override {
        validRead(extent_,r,o.size());auto it=files_.find(id);
        if(it==files_.end())throw std::invalid_argument("unknown RAW file identity");
        auto& f=it->second;
        for(std::uint32_t y=0;y<r.height;++y) {
            const auto offset=f.binding.offsetBytes+2*(static_cast<std::uint64_t>(r.y+y)*f.binding.rowStridePixels+r.x);
            f.stream.clear();f.stream.seekg(static_cast<std::streamoff>(offset));
            auto* p=o.data()+static_cast<std::size_t>(y)*r.width;
            f.stream.read(reinterpret_cast<char*>(p),static_cast<std::streamsize>(r.width)*2);
            if(f.stream.gcount()!=static_cast<std::streamsize>(r.width)*2)throw std::runtime_error("short RAW tile read");
            if constexpr(std::endian::native==std::endian::big) for(std::uint32_t x=0;x<r.width;++x)
                p[x]=static_cast<std::uint16_t>((p[x]>>8U)|(p[x]<<8U));
        }
    }
    std::uint64_t residentBytes() const override {return static_cast<std::uint64_t>(files_.size())*8192U;}
private:
    imaging::Extent extent_;std::map<imaging::FrameId,File> files_;
};
bool measured(const imaging::ExposureCalibration& c) {
    return c.source==imaging::MetadataSource::MeasuredCalibration||c.source==imaging::MetadataSource::DeviceProfile;
}
float shadingAt(const imaging::LensShadingMap& map,const imaging::SensorSampling& s,std::uint32_t x,std::uint32_t y,std::size_t c) {
    const auto& t=s.bufferToCalibration;
    const auto& r=s.calibration;
    const double sx=t.translateX+static_cast<double>(x)*t.scaleX;
    const double sy=t.translateY+static_cast<double>(y)*t.scaleY;
    const float u=static_cast<float>(std::clamp((sx-r.x)/std::max(1U,r.width-1U),0.0,1.0))*static_cast<float>(map.gridColumns-1);
    const float v=static_cast<float>(std::clamp((sy-r.y)/std::max(1U,r.height-1U),0.0,1.0))*static_cast<float>(map.gridRows-1);
    const auto ix=static_cast<std::uint32_t>(u),iy=static_cast<std::uint32_t>(v);
    const float fx=u-static_cast<float>(ix),fy=v-static_cast<float>(iy);
    float value=0;
    for(std::uint32_t j=0;j<2;++j)for(std::uint32_t i=0;i<2;++i) {
        const auto cx=std::min(map.gridColumns-1,ix+i),cy=std::min(map.gridRows-1,iy+j);
        value+=(i?fx:1-fx)*(j?fy:1-fy)*map.gains[(static_cast<std::size_t>(cy)*map.gridColumns+cx)*4+c];
    }
    return value;
}
}
std::unique_ptr<RawTileSource> makeHostTileSource(const imaging::RawBurst& b,const HostRawBindings& h) {return std::make_unique<HostSource>(b,h);}
std::unique_ptr<RawTileSource> makeFileTileSource(const imaging::RawBurst& b,std::span<const RawFileBinding> f) {return std::make_unique<FileSource>(b,f);}
void normalizeMosaicTile(const imaging::RawFrameMetadata& m,const imaging::RawFrameMetadata& ref,
    imaging::Extent e,imaging::SensorRect r,std::span<const std::uint16_t> input,const TiledReconstructionPolicy& p,
    std::span<reference::MosaicEvidence> out) {
    validRead(e,r,input.size());if(out.size()!=input.size())throw std::invalid_argument("normalized tile size mismatch");
    const auto v=imaging::validateRawMetadata(m);if(!v.valid)throw std::invalid_argument(v.message);
    const auto s=m.sampling.value_or(imaging::regularSampling(e,m.cfa));
    const auto sv=imaging::validateSampling(s,e);if(!sv.valid)throw std::invalid_argument(sv.message);
    const auto levels=reference::selectRawLevels(m),refLevels=reference::selectRawLevels(ref);
    imaging::NoiseModel noise{};
    if(m.noiseProfile.usable())noise=reference::normalizeNoiseModel(*m.noiseProfile.value,levels);
    else noise.read.fill(p.missingNoiseVariance);
    std::array<float,4> scale{};
    for(std::size_t c=0;c<4;++c) {
        const float gain=measured(m.exposureCalibration)&&measured(ref.exposureCalibration)?
            ref.exposureCalibration.effectiveGain[c]/m.exposureCalibration.effectiveGain[c]:
            (p.allowNominalIsoGainEstimate?ref.sensitivityIso/m.sensitivityIso:1.0F);
        scale[c]=static_cast<float>(ref.exposureTimeNs)/static_cast<float>(m.exposureTimeNs)*gain*
            (levels.white-levels.black.cfa[c])/(refLevels.white-refLevels.black.cfa[c]);
        if(!std::isfinite(scale[c])||scale[c]<=0)throw std::invalid_argument("invalid radiometric scaling");
    }
    // G0/G1 are independently calibrated channels, even when the output is RGB.
    // Balance both to their arithmetic-mean reference WB gain BEFORE merging.
    // R/B remain un-white-balanced; downstream RGB WB uses that green mean.
    // The REFERENCE observation is fixed over the burst, avoiding per-frame AWB drift.
    if(ref.colorCorrectionGains.usable()) {
        const auto& wb=*ref.colorCorrectionGains.value;
        const float mean=0.5F*(wb[1]+wb[2]);
        if(!std::isfinite(mean)||mean<=0)throw std::invalid_argument("invalid reference green calibration");
        scale[1]*=wb[1]/mean;scale[2]*=wb[2]/mean;
    }
    for(std::uint32_t y=0;y<r.height;++y)for(std::uint32_t x=0;x<r.width;++x) {
        const auto i=static_cast<std::size_t>(y)*r.width+x;
        const auto c=static_cast<std::size_t>(imaging::samplingChannelAt(s,r.x+x,r.y+y));
        const float code=static_cast<float>(input[i]);
        const float value=reference::normalizeSensorCode(code,levels.black.cfa[c],levels.white);
        const float gain=scale[c]*(p.applyLensShading&&m.lensShading.usable()?shadingAt(*m.lensShading.value,s,r.x+x,r.y+y,c):1.0F);
        out[i]={value*gain,(noise.shot[c]*std::max(value,0.0F)+noise.read[c])*gain*gain,static_cast<std::uint32_t>(c),code<levels.white?1U:0U};
        if(!std::isfinite(out[i].value)||!std::isfinite(out[i].variance)||out[i].variance<0)
            throw std::invalid_argument("non-finite normalized evidence");
    }
    for(const auto& d:m.defects) {
        if(d.x>=e.width||d.y>=e.height)throw std::invalid_argument("defect outside RAW extent");
        if(d.x>=r.x&&d.x-r.x<r.width&&d.y>=r.y&&d.y-r.y<r.height)out[static_cast<std::size_t>(d.y-r.y)*r.width+d.x-r.x].valid=0;
    }
}
}  // namespace latent::runtime
