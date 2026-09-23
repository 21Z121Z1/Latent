#include "latent/runtime/TiledReconstruction.h"
#ifdef LATENT_ENABLE_VULKAN_RUNTIME
#include "latent/vulkan/DirectReconstruction.h"
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace latent::runtime {
namespace {
using Clock=std::chrono::steady_clock;
double ms(Clock::time_point start) {return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
std::uint32_t ceilDiv(std::uint32_t n,std::uint32_t d) {return n/d+(n%d!=0?1U:0U);}
std::uint64_t square(std::uint32_t n) {return static_cast<std::uint64_t>(n)*n;}
void checkpoint(const std::function<bool()>& f) {if(f&&!f())throw std::runtime_error("direct reconstruction cancelled");}
imaging::SensorSampling sampling(const imaging::RawBurst& b) {
    const auto& m=b.members.front().observations;return m.sampling.value_or(imaging::regularSampling(b.extent,m.cfa));
}
reference::ReconstructionWarp warpAt(const ReconstructionMotion& m,float x,float y,imaging::Extent e) {
    reference::ReconstructionWarp w{};
    w.x=m.affine[0]*x+m.affine[1]*y+m.affine[2]+m.rowDx*(y/static_cast<float>(std::max(1U,e.height-1U))-0.5F);
    w.y=m.affine[3]*x+m.affine[4]*y+m.affine[5]+m.rowDy*(y/static_cast<float>(std::max(1U,e.height-1U))-0.5F);
    w.confidence=m.confidence;
    if(m.tileSize) {
        const auto tx=std::min(m.columns-1,static_cast<std::uint32_t>(std::max(x,0.0F))/m.tileSize);
        const auto ty=std::min(m.rows-1,static_cast<std::uint32_t>(std::max(y,0.0F))/m.tileSize);
        const auto& r=m.residual[static_cast<std::size_t>(ty)*m.columns+tx];
        w.x+=r.dx;w.y+=r.dy;w.confidence*=r.confidence;
    }
    return w;
}
void validateMotion(const ReconstructionMotion& m,imaging::Extent e) {
    for(float a:m.affine)if(!std::isfinite(a))throw std::invalid_argument("non-finite affine motion");
    if(!std::isfinite(m.rowDx)||!std::isfinite(m.rowDy)||!std::isfinite(m.confidence)||m.confidence<0||m.confidence>1)
        throw std::invalid_argument("invalid row motion/confidence");
    const float det=m.affine[0]*m.affine[4]-m.affine[1]*m.affine[3];
    if(det<0.25F||det>4)throw std::invalid_argument("singular or excessive motion scale");
    if(m.tileSize) {
        if(m.tileSize>4096||m.columns!=ceilDiv(e.width,m.tileSize)||m.rows!=ceilDiv(e.height,m.tileSize)||
            m.residual.size()!=static_cast<std::size_t>(m.columns)*m.rows)throw std::invalid_argument("invalid local motion grid");
        for(const auto& r:m.residual)if(!std::isfinite(r.dx)||!std::isfinite(r.dy)||!std::isfinite(r.confidence)||r.confidence<0||r.confidence>1)
            throw std::invalid_argument("invalid local motion observation");
    } else if(!m.residual.empty()||m.columns||m.rows)throw std::invalid_argument("local motion without coordinate grid");
}
reference::AlignmentGuide makeGuide(const imaging::RawBurst& b,const imaging::RawBurstMember& member,
    RawTileSource& source,const TiledReconstructionPolicy& policy,std::uint32_t step,std::vector<std::uint16_t>& raw,
    std::span<reference::MosaicEvidence> normalized,DirectReconstructionTrace& trace,const std::function<bool()>& cancel) {
    reference::AlignmentGuide guide{{b.extent.width/step,b.extent.height/step},step,{}};
    guide.samples.resize(static_cast<std::size_t>(guide.extent.pixelCount()));
    // Each cell covers whole CFA periods, even for an odd or grouped crop origin.
    const auto cellsPerTile=std::max(1U,static_cast<std::uint32_t>(std::sqrt(static_cast<double>(raw.size())))/step);
    for(std::uint32_t gy=0;gy<guide.extent.height;gy+=cellsPerTile)for(std::uint32_t gx=0;gx<guide.extent.width;gx+=cellsPerTile) {
        checkpoint(cancel);
        const auto cw=std::min(cellsPerTile,guide.extent.width-gx),ch=std::min(cellsPerTile,guide.extent.height-gy);
        const imaging::SensorRect r{gx*step,gy*step,cw*step,ch*step};
        const auto n=static_cast<std::size_t>(r.width)*r.height;
        source.read(member.id,r,std::span(raw).first(n));trace.rawBytesRead+=2*n;
        normalizeMosaicTile(member.observations,b.members.front().observations,b.extent,r,std::span(raw).first(n),policy,std::span(normalized).first(n));
        for(std::uint32_t cy=0;cy<ch;++cy)for(std::uint32_t cx=0;cx<cw;++cx) {
            std::array<float,4> sum{},variance{},count{};
            for(std::uint32_t y=0;y<step;++y)for(std::uint32_t x=0;x<step;++x) {
                const auto& a=normalized[static_cast<std::size_t>(cy*step+y)*r.width+cx*step+x];
                if(a.valid){sum[a.channel]+=a.value;variance[a.channel]+=a.variance;count[a.channel]+=1;}
            }
            auto& a=guide.samples[static_cast<std::size_t>(gy+cy)*guide.extent.width+gx+cx];
            a.usable=1;
            for(std::size_t c=0;c<4;++c) {
                if(count[c]==0){a.usable=0;continue;}
                a.value+=sum[c]/count[c]*0.25F;a.variance+=variance[c]/(count[c]*count[c])*0.0625F;
            }
        }
    }
    return guide;
}
}
TiledReconstructionPlan planTiledReconstruction(const imaging::RawBurst& b,const ReconstructionGrid& grid,
    const TiledReconstructionPolicy& p,const ReconstructionCapabilities& caps,std::uint64_t resident,std::uint64_t sinkResident) {
    const auto v=imaging::validateRawBurst(b);if(!v.valid())throw std::invalid_argument(v.message);
    reference::validateDirectKernelPolicy(p.kernel);
    if(b.extent.width>1000000||b.extent.height>1000000||p.maximumFrames==0||p.maximumFrames>32||
        p.tileSize<16||p.tileSize>512||p.maximumDisplacement>1024||p.guideBudgetBytes<65536||
        !std::isfinite(p.missingNoiseVariance)||p.missingNoiseVariance<p.kernel.varianceFloor||p.missingNoiseVariance>1||
        caps.thermalSeverity>2)throw std::invalid_argument("invalid tiled reconstruction policy");
    if(!grid.extent.width||!grid.extent.height||grid.extent.width>1000000||grid.extent.height>1000000||
        !std::isfinite(grid.originX)||!std::isfinite(grid.originY)||grid.originX<0||grid.originY<0||
        !std::isfinite(grid.stepX)||!std::isfinite(grid.stepY)||grid.stepX<0.5F||grid.stepY<0.5F||grid.stepX>8||grid.stepY>8||
        grid.originX+static_cast<float>(grid.extent.width-1)*grid.stepX>static_cast<float>(b.extent.width-1)||
        grid.originY+static_cast<float>(grid.extent.height-1)*grid.stepY>static_cast<float>(b.extent.height-1))
        throw std::invalid_argument("output grid must be explicit and lie inside reference RAW");
    const auto s=sampling(b);
    if(s.buffer.groupX>16||s.buffer.groupY>16)throw std::invalid_argument("group factor exceeds direct backend support (16)");
    TiledReconstructionPlan plan{};
    plan.frames=static_cast<std::uint32_t>(std::min<std::size_t>(b.members.size(),p.maximumFrames));
    if(caps.thermalSeverity==2)plan.frames=1;
    else if(caps.thermalSeverity==1)plan.frames=std::min(plan.frames,4U);
    if(plan.frames<b.members.size()) {
        if(!p.allowFrameReduction)throw std::invalid_argument("resource policy may not reduce fixed burst membership");
        plan.decisions.emplace_back("frame_count_reduced_by_delegated_budget_or_thermal_policy");
    }
    plan.radiusX=s.buffer.groupX+1;plan.radiusY=s.buffer.groupY+1;
    const auto period=std::lcm(2*s.buffer.groupX,2*s.buffer.groupY);
    plan.guideStep=period;
    const auto guideBytes=[&] {return static_cast<std::uint64_t>(b.extent.width/plan.guideStep)*(b.extent.height/plan.guideStep)*16*(plan.frames+6U);};
    while(guideBytes()>p.guideBudgetBytes && plan.guideStep<4096)plan.guideStep*=2;
    if(guideBytes()>p.guideBudgetBytes)throw std::invalid_argument("guide budget too small");
    plan.vulkan=caps.preferVulkan;
#ifndef LATENT_ENABLE_VULKAN_RUNTIME
    if(plan.vulkan){plan.vulkan=false;plan.decisions.emplace_back("vulkan_not_built_cpu_fallback");}
#endif
    plan.tileSize=p.tileSize;
    plan.sourceResidentBytes=resident;plan.sinkResidentBytes=sinkResident;
    for(;;) {
        plan.sourceTileSide=static_cast<std::uint32_t>(std::ceil(static_cast<float>(plan.tileSize-1)*std::max(grid.stepX,grid.stepY)))+
            2*p.maximumDisplacement+2*std::max(plan.radiusX,plan.radiusY)+4;
        plan.sourceTileSide=std::max(plan.sourceTileSide,plan.guideStep);
        const auto n=square(plan.tileSize),src=square(plan.sourceTileSide);
        const auto motion=static_cast<std::uint64_t>(ceilDiv(b.extent.width,256))*ceilDiv(b.extent.height,256)*16*plan.frames;
        // Coherent GPU buffers can consume the same mobile RAM. Charge both
        // CPU arenas and GPU allocations conservatively, not just C++ heap.
        plan.hostWorkingBytes=src*18U+n*240U+guideBytes()+motion+1048576;
        plan.deviceWorkingBytes=plan.vulkan?(src*16+n*240):0;
        const auto largest=std::max(src*16,n*160);
        const bool hostFits=resident<=caps.hostBudgetBytes && sinkResident<=caps.hostBudgetBytes-resident &&
            plan.hostWorkingBytes<=caps.hostBudgetBytes-resident-sinkResident;
        const bool deviceFits=!plan.vulkan || (plan.deviceWorkingBytes<=caps.deviceBudgetBytes&&largest<=caps.maxStorageBufferRange);
        if(hostFits&&deviceFits)break;
        if(plan.tileSize>16){plan.tileSize=std::max(16U,plan.tileSize/2);continue;}
        if(plan.vulkan&&hostFits){plan.vulkan=false;plan.decisions.emplace_back("device_arena_budget_cpu_fallback");continue;}
        throw std::invalid_argument("bounded reconstruction arena exceeds memory budget; release RAW leases or use a file tile source");
    }
    if(plan.tileSize!=p.tileSize)plan.decisions.emplace_back("spatial_tile_reduced_for_memory_budget");
    if(plan.guideStep!=period)plan.decisions.emplace_back("coarser_registration_guide_for_bounded_memory");
    return plan;
}

DirectReconstructionTrace reconstructRawTiles(const imaging::RawBurst& b,RawTileSource& source,
    const ReconstructionGrid& grid,const TiledReconstructionPolicy& p,const ReconstructionCapabilities& caps,
    ReconstructionTileSink& sink,std::span<const ReconstructionMotion> supplied,std::optional<imaging::FrameId> requested,
    std::function<bool()> cancel) {
    const auto start=Clock::now();checkpoint(cancel);
    DirectReconstructionTrace trace{};trace.plan=planTiledReconstruction(b,grid,p,caps,source.residentBytes(),sink.residentBytes());
    const auto& plan=trace.plan;const auto s=sampling(b);
    if(!supplied.empty()) {
        // Charge caller-owned observations AND the copy retained in the trace.
        // Refuse dense fields before copying; a tile planner must not hide O(N) flow.
        std::uint64_t motionBytes=0;
        for(const auto& m:supplied) {
            if(m.residual.capacity()>caps.hostBudgetBytes/(2*sizeof(reference::MotionTile)))
                throw std::invalid_argument("external motion exceeds bounded memory budget");
            const auto bytes=static_cast<std::uint64_t>(m.residual.capacity())*2*sizeof(reference::MotionTile);
            if(bytes>caps.hostBudgetBytes-motionBytes)throw std::invalid_argument("external motion budget exceeded");
            motionBytes+=bytes;
            validateMotion(m,b.extent);
        }
        if(motionBytes>caps.hostBudgetBytes-plan.sourceResidentBytes-plan.sinkResidentBytes-plan.hostWorkingBytes)
            throw std::invalid_argument("external motion residency exceeds admitted host budget");
        trace.plan.hostWorkingBytes+=motionBytes;
    }
    std::vector<std::size_t> members(plan.frames);std::iota(members.begin(),members.end(),0);
    std::size_t ri=0;
    if(requested) {
        const auto it=std::find_if(b.members.begin(),b.members.end(),[&](const auto& m){return m.id==*requested;});
        if(it==b.members.end())throw std::invalid_argument("reference not in burst");
        ri=static_cast<std::size_t>(it-b.members.begin());
        if(ri>=plan.frames)members.back()=ri;
    }
    const auto maxSource=static_cast<std::size_t>(square(plan.sourceTileSide)),maxOutput=static_cast<std::size_t>(square(plan.tileSize));
    std::unique_ptr<reference::DirectTileExecutor> executor;
#ifdef LATENT_ENABLE_VULKAN_RUNTIME
    if(plan.vulkan) {
        std::string detail;
        executor=vulkan::makeVulkanDirectExecutor(maxSource,maxOutput,p.kernel,&detail,caps.collectGpuTimings);
        if(!executor){trace.plan.vulkan=false;trace.plan.deviceWorkingBytes=0;trace.plan.decisions.emplace_back("vulkan_unavailable_cpu_fallback:"+detail);}
    }
#endif
    if(!executor){executor=reference::makeReferenceDirectExecutor(maxOutput,p.kernel);trace.arenaAllocations+=2;}
    else {
        trace.arenaAllocations+=4;
        const auto actual=executor->deviceBytes();
        const auto extra=actual>plan.deviceWorkingBytes?actual-plan.deviceWorkingBytes:0;
        if(actual>caps.deviceBudgetBytes || extra>caps.hostBudgetBytes-plan.hostWorkingBytes-plan.sourceResidentBytes-plan.sinkResidentBytes)
            throw std::invalid_argument("actual Vulkan allocations exceed admitted memory budget");
        trace.plan.hostWorkingBytes+=extra;trace.plan.deviceWorkingBytes=actual;
    }
    std::vector<std::uint16_t> raw(maxSource);++trace.arenaAllocations;
    std::vector<reference::MosaicEvidence> normalizedStorage;
    std::vector<reference::ReconstructionWarp> warpStorage;
    std::vector<reference::ReconstructedPixel> outputStorage;
    auto normalized=executor->inputArena();auto warps=executor->warpArena();auto output=executor->outputArena();
    const bool mapped=!normalized.empty()&&!warps.empty()&&!output.empty();
    if(normalized.empty()){normalizedStorage.resize(maxSource);normalized=normalizedStorage;++trace.arenaAllocations;}
    if(warps.empty()){warpStorage.resize(maxOutput);warps=warpStorage;++trace.arenaAllocations;}
    if(output.empty()){outputStorage.resize(maxOutput);output=outputStorage;++trace.arenaAllocations;}
    if(normalized.size()<maxSource||warps.size()<maxOutput||output.size()<maxOutput)
        throw std::logic_error("backend exposes an undersized coherent arena");
    if(supplied.empty()) {
        const auto guideStart=Clock::now();
        std::vector<reference::AlignmentGuide> guides;
        guides.reserve(members.size());
        for(auto index:members)guides.push_back(makeGuide(b,b.members[index],source,p,plan.guideStep,raw,normalized,trace,cancel));
        if(!requested) {
            double best=-1e300;
            for(std::size_t i=0;i<guides.size();++i) {
                const auto& g=guides[i];double sharp=0,noise=0,valid=0;
                for(std::uint32_t y=0;y<g.extent.height;++y)for(std::uint32_t x=0;x<g.extent.width;++x) {
                    const auto k=static_cast<std::size_t>(y)*g.extent.width+x;const auto& a=g.samples[k];
                    if(a.usable==0)continue;
                    valid+=1;noise+=a.variance;
                    if(x>0&&g.samples[k-1].usable!=0){const double d=a.value-g.samples[k-1].value;sharp+=std::max(0.0,d*d-a.variance-g.samples[k-1].variance);}
                }
                const double n=static_cast<double>(std::max<std::uint64_t>(1,g.extent.pixelCount()));
                const double score=valid/n*100+std::log1p(sharp/n)-std::log1p(noise/n);
                if(score>best){best=score;ri=members[i];}
            }
        }
        trace.guideMilliseconds=ms(guideStart);
        const auto alignStart=Clock::now();
        const auto refIndex=static_cast<std::size_t>(std::find(members.begin(),members.end(),ri)-members.begin());
        reference::TemporalPolicy alignPolicy{};alignPolicy.tileSize=256;alignPolicy.maximumDisplacement=p.maximumDisplacement;
        for(std::size_t i=0;i<members.size();++i) {
            checkpoint(cancel);ReconstructionMotion m{};m.frame=b.members[members[i]].id;
            if(i!=refIndex && guides[i].extent.width>=2 && guides[i].extent.height>=2) {
                const auto field=reference::alignRawGuides(guides[refIndex],guides[i],b.extent,b.members[ri].id,m.frame,alignPolicy);
                m.tileSize=field.tileSize;m.columns=field.columns;m.rows=field.rows;m.residual=field.tiles;
                // MotionTile values are absolute translations in this lowering;
                // the affine remains identity, never add global translation twice.
            } else if(i!=refIndex){m.confidence=0;trace.rejectedFrames.emplace_back("registration_guide_too_small:"+std::to_string(m.frame.value));}
            trace.motion.push_back(std::move(m));
        }
        trace.alignmentMilliseconds=ms(alignStart);
    } else {
        if(supplied.size()!=b.members.size() || !requested)throw std::invalid_argument("external motion requires complete burst and explicit reference identity");
        for(auto index:members) {
            const auto id=b.members[index].id;
            const auto it=std::find_if(supplied.begin(),supplied.end(),[&](const auto& m){return m.frame==id;});
            if(it==supplied.end() || std::count_if(supplied.begin(),supplied.end(),[&](const auto& m){return m.frame==id;})!=1)
                throw std::invalid_argument("external motion membership mismatch");
            trace.motion.push_back(*it);
        }
    }
    trace.reference=b.members[ri].id;
    for(auto& m:trace.motion) {
        validateMotion(m,b.extent);
        if(m.frame==trace.reference) {
            if(m.affine!=std::array<float,6>{1,0,0,0,1,0} || m.rowDx!=0 || m.rowDy!=0 || m.tileSize || m.confidence!=1)
                throw std::invalid_argument("reference motion must be identity in output coordinates");
        }
        const auto it=std::find_if(b.members.begin(),b.members.end(),[&](const auto& v){return v.id==m.frame;});
        if(it->observations.sensitivityIso!=b.members[ri].observations.sensitivityIso && !p.allowNominalIsoGainEstimate) {
            const auto calibrated=[](const auto& c){return c.source==imaging::MetadataSource::MeasuredCalibration||c.source==imaging::MetadataSource::DeviceProfile;};
            if(!calibrated(it->observations.exposureCalibration)||!calibrated(b.members[ri].observations.exposureCalibration)) {
                m.confidence=0;trace.rejectedFrames.emplace_back("uncalibrated_ISO_transition:"+std::to_string(m.frame.value));
            }
        }
    }
    std::vector<std::size_t> order{ri};for(auto i:members)if(i!=ri)order.push_back(i);
    const auto reconstructionStart=Clock::now();
    for(std::uint32_t oy=0;oy<grid.extent.height;oy+=plan.tileSize)for(std::uint32_t ox=0;ox<grid.extent.width;ox+=plan.tileSize) {
        checkpoint(cancel);
        const imaging::SensorRect dst{ox,oy,std::min(plan.tileSize,grid.extent.width-ox),std::min(plan.tileSize,grid.extent.height-oy)};
        const auto count=static_cast<std::size_t>(dst.width)*dst.height;
        for(auto index:order) {
            checkpoint(cancel);
            const auto& member=b.members[index];
            const auto& motion=*std::find_if(trace.motion.begin(),trace.motion.end(),[&](const auto& m){return m.frame==member.id;});
            const bool isRef=index==ri;
            if(!isRef&&motion.confidence==0)continue;
            float minX=static_cast<float>(b.extent.width),minY=static_cast<float>(b.extent.height),maxX=0,maxY=0;
            for(std::uint32_t y=0;y<dst.height;++y)for(std::uint32_t x=0;x<dst.width;++x) {
                const float rx=grid.originX+static_cast<float>(dst.x+x)*grid.stepX;
                const float ry=grid.originY+static_cast<float>(dst.y+y)*grid.stepY;
                auto w=warpAt(motion,rx,ry,b.extent);
                if(!std::isfinite(w.x)||!std::isfinite(w.y)||std::abs(w.x-rx)>static_cast<float>(p.maximumDisplacement)+1e-4F||
                    std::abs(w.y-ry)>static_cast<float>(p.maximumDisplacement)+1e-4F)throw std::invalid_argument("motion exceeds admitted RAW tile halo");
                minX=std::min(minX,w.x);minY=std::min(minY,w.y);maxX=std::max(maxX,w.x);maxY=std::max(maxY,w.y);
                warps[static_cast<std::size_t>(y)*dst.width+x]=w;
            }
            const auto bound=[](float a,std::uint32_t e){return static_cast<std::uint32_t>(std::clamp(a,0.0F,static_cast<float>(e-1)));};
            const auto x0=bound(std::floor(minX)-static_cast<float>(plan.radiusX),b.extent.width);
            const auto y0=bound(std::floor(minY)-static_cast<float>(plan.radiusY),b.extent.height);
            const auto x1=bound(std::floor(maxX)+static_cast<float>(plan.radiusX)+1,b.extent.width);
            const auto y1=bound(std::floor(maxY)+static_cast<float>(plan.radiusY)+1,b.extent.height);
            const imaging::SensorRect footprint{x0,y0,x1-x0+1,y1-y0+1};
            const auto n=static_cast<std::size_t>(footprint.width)*footprint.height;
            if(n>maxSource)throw std::logic_error("source footprint exceeds admitted arena");
            source.read(member.id,footprint,std::span(raw).first(n));trace.rawBytesRead+=n*2;
            normalizeMosaicTile(member.observations,b.members[ri].observations,b.extent,footprint,std::span(raw).first(n),p,std::span(normalized).first(n));
            const reference::DirectKernelGeometry kg{footprint,plan.radiusX,plan.radiusY,
                std::max(1.0F,static_cast<float>(std::max(s.buffer.groupX,s.buffer.groupY))*0.75F),isRef,
                s.representation==imaging::RawRepresentation::RemosaicedBayer};
            if(mapped)trace.mappedAccessBytes+=n*sizeof(reference::MosaicEvidence)+count*sizeof(reference::ReconstructionWarp);
            if(isRef)executor->begin(std::span(normalized).first(n),std::span(warps).first(count),kg);
            else executor->add(std::span(normalized).first(n),std::span(warps).first(count),kg);
        }
        executor->finish(std::span(output).first(count));
        if(mapped)trace.mappedAccessBytes+=count*sizeof(reference::ReconstructedPixel);
        for(std::size_t i=0;i<count;++i)for(std::size_t c=0;c<3;++c) {
            const auto& v=output[i];
            if(!std::isfinite(v.rgb[c])||!std::isfinite(v.variance[c]))throw std::runtime_error("non-finite reconstruction result");
            trace.meanEffectiveFrames+=v.effectiveFrames[c];trace.meanConfidence+=v.confidence[c];trace.meanVariance+=v.variance[c];
            if(v.confidence[c]==0)++trace.invalidChannels;
            if(v.confidence[c]==0.01F)++trace.referenceFallbackChannels;
        }
        sink.write(dst,std::span(output).first(count));++trace.tiles;trace.outputPixels+=count;
    }
    trace.reconstructionMilliseconds=ms(reconstructionStart);trace.totalMilliseconds=ms(start);
    trace.meanEffectiveFrames/=static_cast<double>(trace.outputPixels)*3;
    trace.meanConfidence/=static_cast<double>(trace.outputPixels)*3;trace.meanVariance/=static_cast<double>(trace.outputPixels)*3;
    trace.transferBytes=executor->transferBytes();trace.gpuMilliseconds=executor->gpuMilliseconds();
    if(trace.plan.vulkan)trace.plan.deviceWorkingBytes=executor->deviceBytes();
    return trace;
}
std::string directReconstructionJson(const DirectReconstructionTrace& t) {
    std::ostringstream o;o << std::setprecision(12);
    o << "{\"algorithm\":\"" << kDirectReconstructionVersion << "\",\"reference\":" << t.reference.value
      << ",\"tile_size\":" << t.plan.tileSize << ",\"guide_step\":" << t.plan.guideStep << ",\"frames\":" << t.plan.frames
      << ",\"halo\":[" << t.plan.radiusX << ',' << t.plan.radiusY << "],\"host_arena_bound\":" << t.plan.hostWorkingBytes
      << ",\"sink_resident_bytes\":" << t.plan.sinkResidentBytes
      << ",\"source_resident_bytes\":" << t.plan.sourceResidentBytes << ",\"device_arena_bytes\":" << t.plan.deviceWorkingBytes
      << ",\"backend\":\"" << (t.plan.vulkan?"vulkan":"reference") << "\",\"arena_allocations\":" << t.arenaAllocations
      << ",\"raw_bytes_read\":" << t.rawBytesRead << ",\"transfer_bytes\":" << t.transferBytes << ",\"mapped_access_bytes\":" << t.mappedAccessBytes << ",\"tiles\":" << t.tiles
      << ",\"output_pixels\":" << t.outputPixels << ",\"invalid_channels\":" << t.invalidChannels
      << ",\"reference_fallback_channels\":" << t.referenceFallbackChannels << ",\"mean_effective_frames\":" << t.meanEffectiveFrames
      << ",\"mean_confidence\":" << t.meanConfidence << ",\"mean_variance\":" << t.meanVariance
      << ",\"guide_ms\":" << t.guideMilliseconds << ",\"alignment_ms\":" << t.alignmentMilliseconds
      << ",\"reconstruction_ms\":" << t.reconstructionMilliseconds << ",\"total_ms\":" << t.totalMilliseconds << ",\"gpu_ms\":";
    if(t.gpuMilliseconds<0)o<<"null";else o<<t.gpuMilliseconds;
    const auto strings=[&o](const char* key,const std::vector<std::string>& values){o<<",\""<<key<<"\":[";bool comma=false;
        for(const auto& s:values){if(comma)o<<',';comma=true;o<<'\"';for(char c:s){if(c=='\"'||c=='\\')o<<'\\';if(static_cast<unsigned char>(c)>=32)o<<c;}o<<'\"';}o<<']';};
    strings("decisions",t.plan.decisions);strings("rejected_frames",t.rejectedFrames);
    o<<",\"motion\":[";bool comma=false;
    for(const auto& m:t.motion){if(comma)o<<',';comma=true;o<<"{\"frame\":"<<m.frame.value<<",\"affine\":[";
        for(std::size_t i=0;i<6;++i){if(i)o<<',';o<<m.affine[i];}o<<"],\"row\":["<<m.rowDx<<','<<m.rowDy<<"],\"confidence\":"<<m.confidence
        <<",\"local_tile_size\":"<<m.tileSize<<",\"local\":[";bool comma2=false;
        for(const auto& r:m.residual){if(comma2)o<<',';comma2=true;o<<'['<<r.dx<<','<<r.dy<<','<<r.confidence<<']';}o<<"]}";}
    o<<"]}";return o.str();
}
}  // namespace latent::runtime
