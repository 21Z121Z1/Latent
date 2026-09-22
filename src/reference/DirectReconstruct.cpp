#include "latent/reference/DirectReconstruct.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace latent::reference {
namespace {
struct Stats {
    std::array<float,3> mean{}, noise{}, spatial{}, weight{}, varianceSum{}, stddevSum{};
};
// Polynomial compact Gaussian approximation is deliberately not used: matching
// exp on CPU/GPU is checked with an explicit tolerance, not bitwise assertions.
Stats gather(std::span<const MosaicEvidence> input, ReconstructionWarp w,
    const DirectKernelGeometry& g, const ReconstructionAnchor* anchor) {
    Stats s{};
    const int cx=static_cast<int>(std::floor(w.x)), cy=static_cast<int>(std::floor(w.y));
    const int x0=std::max(static_cast<int>(g.sourceTile.x),cx-static_cast<int>(g.radiusX));
    const int y0=std::max(static_cast<int>(g.sourceTile.y),cy-static_cast<int>(g.radiusY));
    const int x1=std::min(static_cast<int>(g.sourceTile.x+g.sourceTile.width)-1,cx+static_cast<int>(g.radiusX)+1);
    const int y1=std::min(static_cast<int>(g.sourceTile.y+g.sourceTile.height)-1,cy+static_cast<int>(g.radiusY)+1);
    const float inv=1/(g.fallbackSigma*g.fallbackSigma);
    const bool separable=!anchor || anchor->precision[1]==0;
    std::array<float,66> wx{},wy{};
    if(separable) {
        const float px=anchor?anchor->precision[0]:inv,py=anchor?anchor->precision[2]:inv;
        for(int x=x0;x<=x1;++x){const float d=static_cast<float>(x)-w.x;wx[static_cast<std::size_t>(x-x0)]=std::exp(-0.5F*d*d*px);}
        for(int y=y0;y<=y1;++y){const float d=static_cast<float>(y)-w.y;wy[static_cast<std::size_t>(y-y0)]=std::exp(-0.5F*d*d*py);}
    }
    for (int y=y0;y<=y1;++y) for(int x=x0;x<=x1;++x) {
        const auto i=static_cast<std::size_t>(y-static_cast<int>(g.sourceTile.y))*g.sourceTile.width+
            static_cast<std::size_t>(x-static_cast<int>(g.sourceTile.x));
        const auto& a=input[i];
        if (!a.valid) continue;
        const auto c=a.channel==3U?2U:(a.channel==0U?0U:1U);
        const float dx=static_cast<float>(x)-w.x,dy=static_cast<float>(y)-w.y;
        const float distance=anchor ? dx*dx*anchor->precision[0]+2*dx*dy*anchor->precision[1]+dy*dy*anchor->precision[2] :
            (dx*dx+dy*dy)*inv;
        const float k=separable ? wx[static_cast<std::size_t>(x-x0)]*wy[static_cast<std::size_t>(y-y0)] : std::exp(-0.5F*distance);
        s.mean[c]+=k*a.value;
        s.weight[c]+=k;
        s.varianceSum[c]+=k*k*a.variance;
        if(g.correlatedSpatialNoise) s.stddevSum[c]+=k*std::sqrt(a.variance);
        if(!anchor) s.spatial[c]+=k*a.value*a.value;
    }
    for(std::size_t c=0;c<3;++c) if(s.weight[c]>1.0e-20F) {
        s.mean[c]/=s.weight[c];
        s.noise[c]=(g.correlatedSpatialNoise?s.stddevSum[c]*s.stddevSum[c]:s.varianceSum[c])/(s.weight[c]*s.weight[c]);
        s.spatial[c]=std::max(0.0F,s.spatial[c]/s.weight[c]-s.mean[c]*s.mean[c]-s.noise[c]);
    }
    return s;
}
void validateGeometry(std::span<const MosaicEvidence> input,std::span<const ReconstructionWarp> warp,
    const DirectKernelGeometry& g) {
    if (!g.sourceTile.width || !g.sourceTile.height ||
        static_cast<std::uint64_t>(g.sourceTile.width)*g.sourceTile.height!=input.size() ||
        g.sourceTile.x+static_cast<std::uint64_t>(g.sourceTile.width)>1000000 ||
        g.sourceTile.y+static_cast<std::uint64_t>(g.sourceTile.height)>1000000 ||
        g.radiusX==0 || g.radiusY==0 || g.radiusX>32 || g.radiusY>32 ||
        !std::isfinite(g.fallbackSigma) || g.fallbackSigma<0.5F || g.fallbackSigma>32)
        throw std::invalid_argument("invalid direct reconstruction footprint");
    for (const auto& v:warp) if (!std::isfinite(v.x) || !std::isfinite(v.y) || std::abs(v.x)>1000000 || std::abs(v.y)>1000000 ||
        !std::isfinite(v.confidence) || v.confidence<0 || v.confidence>1) throw std::invalid_argument("invalid reconstruction warp");
    for (const auto& v:input) if(v.channel>3 || v.valid>1 || !std::isfinite(v.value) ||
        !std::isfinite(v.variance) || v.variance<0) throw std::invalid_argument("invalid mosaic evidence");
}
class ReferenceExecutor final : public DirectTileExecutor {
public:
    ReferenceExecutor(std::size_t n,DirectKernelPolicy p):policy_(p),anchors_(n),accum_(n) {validateDirectKernelPolicy(p);}
    void begin(std::span<const MosaicEvidence> s,std::span<const ReconstructionWarp> w,const DirectKernelGeometry& g) override {
        if (count_ || w.empty() || w.size()>anchors_.size() || !g.referenceFrame) throw std::invalid_argument("invalid tile begin");
        count_=w.size(); std::fill_n(accum_.begin(),count_,ReconstructionAccumulator{});
        makeReconstructionAnchors(s,w,g,policy_,std::span(anchors_).first(count_));
        accumulateDirectReconstruction(s,w,std::span(anchors_).first(count_),g,policy_,std::span(accum_).first(count_));
    }
    void add(std::span<const MosaicEvidence> s,std::span<const ReconstructionWarp> w,const DirectKernelGeometry& g) override {
        if (!count_ || g.referenceFrame || w.size()!=count_) throw std::invalid_argument("tile size changed within accumulation");
        accumulateDirectReconstruction(s,w,std::span(anchors_).first(count_),g,policy_,std::span(accum_).first(count_));
    }
    void finish(std::span<ReconstructedPixel> o) override {
        if(!count_ || o.size()!=count_) throw std::invalid_argument("tile output size mismatch");
        finishDirectReconstruction(std::span(anchors_).first(count_),std::span(accum_).first(count_),o);count_=0;
    }
    std::uint64_t deviceBytes() const override {return 0;}
    std::uint64_t transferBytes() const override {return 0;}
    double gpuMilliseconds() const override {return -1;}
private:
    DirectKernelPolicy policy_;std::vector<ReconstructionAnchor> anchors_;
    std::vector<ReconstructionAccumulator> accum_;std::size_t count_=0;
};
}
void validateDirectKernelInput(std::span<const MosaicEvidence> s,std::span<const ReconstructionWarp> w,
    const DirectKernelGeometry& g) {validateGeometry(s,w,g);}
void validateDirectKernelPolicy(const DirectKernelPolicy& p) {
    if(!std::isfinite(p.detailSigma)||p.detailSigma<0.4F||p.detailSigma>4 ||
        !std::isfinite(p.residualCutoff)||p.residualCutoff<1||p.residualCutoff>10 ||
        !std::isfinite(p.aliasAllowance)||p.aliasAllowance<0||p.aliasAllowance>1 ||
        !std::isfinite(p.varianceFloor)||p.varianceFloor<1e-12F||p.varianceFloor>0.01F ||
        !std::isfinite(p.minimumConfidence)||p.minimumConfidence<0||p.minimumConfidence>1)
        throw std::invalid_argument("invalid direct kernel policy");
}
void makeReconstructionAnchors(std::span<const MosaicEvidence> input,std::span<const ReconstructionWarp> warps,
    const DirectKernelGeometry& g,const DirectKernelPolicy& p,std::span<ReconstructionAnchor> out) {
    validateDirectKernelPolicy(p);validateGeometry(input,warps,g);
    if(out.size()!=warps.size()) throw std::invalid_argument("anchor size mismatch");
    for(std::size_t i=0;i<out.size();++i) {
        const auto s=gather(input,warps[i],g,nullptr);
        out[i]={};
        for(std::size_t c=0;c<3;++c) {
            out[i].mean[c]=s.mean[c];out[i].noise[c]=s.noise[c];out[i].spatial[c]=s.spatial[c];
            if(s.weight[c]>1e-20F) out[i].mean[3]+=static_cast<float>(1U<<c);
        }
        // Default isotropic kernel is a conservative baseline. Anisotropy may
        // be installed by a validated guide-based lowering, not a color prior.
        const float inv=1/(p.detailSigma*p.detailSigma);
        out[i].precision={inv,0,inv,0};
    }
}
void accumulateDirectReconstruction(std::span<const MosaicEvidence> input,std::span<const ReconstructionWarp> warps,
    std::span<const ReconstructionAnchor> anchors,const DirectKernelGeometry& g,const DirectKernelPolicy& p,
    std::span<ReconstructionAccumulator> accum) {
    validateDirectKernelPolicy(p);validateGeometry(input,warps,g);
    if(accum.size()!=warps.size()||anchors.size()!=warps.size()) throw std::invalid_argument("accumulator size mismatch");
    for(std::size_t i=0;i<warps.size();++i) {
        const auto w=warps[i];
        if(!g.referenceFrame && w.confidence<p.minimumConfidence) continue;
        const auto& a=anchors[i];
        const auto broad=g.referenceFrame ? Stats{} : gather(input,w,g,nullptr);
        float robust=1;
        if(!g.referenceFrame) for(std::size_t c=0;c<3;++c) {
            if (broad.weight[c]<=1e-20F || (static_cast<unsigned>(a.mean[3])&(1U<<c))==0) continue;
            const float delta=broad.mean[c]-a.mean[c];
            const float scale=p.residualCutoff*p.residualCutoff*(a.noise[c]+broad.noise[c]+p.varianceFloor)+
                p.aliasAllowance*(a.spatial[c]+broad.spatial[c]);
            const float r=std::max(0.0F,1-delta*delta/scale);
            robust=std::min(robust,r*r);
        }
        robust*=g.referenceFrame?1:w.confidence;
        if(robust<=0) continue;
        const auto fine=gather(input,w,g,&a);
        for(std::size_t c=0;c<3;++c) {
            // Precision uses spatially pooled noise estimates, not individual
            // tap precision. Reported uncertainty is conditional on these weights.
            const float precision=1/std::max(p.varianceFloor,a.noise[c]+(g.referenceFrame?a.noise[c]:broad.noise[c]));
            const float k=robust*precision;
            const float fw=fine.weight[c]*k;
            auto& d=accum[i];
            d.value[c]+=fw*fine.mean[c];d.weight[c]+=fw;
            const float varNumerator=g.correlatedSpatialNoise ? fine.stddevSum[c]*fine.stddevSum[c] : fine.varianceSum[c];
            d.variance[c]+=k*k*varNumerator;
            d.frameWeightSquared[c]+=fw*fw;
            d.nearWeight[c]+=robust*fine.weight[c];
            if(g.referenceFrame) d.referenceWeight[c]=fw;
        }
    }
}
void finishDirectReconstruction(std::span<const ReconstructionAnchor> anchors,std::span<const ReconstructionAccumulator> accum,
    std::span<ReconstructedPixel> out) {
    if(anchors.size()!=accum.size()||out.size()!=accum.size()) throw std::invalid_argument("output size mismatch");
    for(std::size_t i=0;i<out.size();++i) {
        out[i]={};
        for(std::size_t c=0;c<3;++c) {
            const auto& a=accum[i];const auto& r=anchors[i];auto& o=out[i];
            const bool referenceValid=(static_cast<unsigned>(r.mean[3])&(1U<<c))!=0;
            if(a.weight[c]>1e-20F) {
                // Continuous support fallback avoids an ill-conditioned branch
                // at a fixed weight threshold (exp/FMA differ across devices).
                float mix=referenceValid?std::clamp((a.nearWeight[c]-0.02F)/0.08F,0.0F,1.0F):1.0F;
                mix=mix*mix*(3-2*mix);
                const float value=a.value[c]/a.weight[c];
                const float variance=a.variance[c]/(a.weight[c]*a.weight[c]);
                const float frameSquare=a.frameWeightSquared[c]/(a.weight[c]*a.weight[c]);
                const float refFraction=a.referenceWeight[c]/a.weight[c];
                o.rgb[c]=mix*value+(1-mix)*r.mean[c];
                // Fused and reference estimates share RAW samples: use a
                // correlated upper bound, NOT an independent mixture variance.
                const float sigma=mix*std::sqrt(variance)+(1-mix)*std::sqrt(r.noise[c]);
                o.variance[c]=sigma*sigma;
                o.effectiveFrames[c]=1/std::max(1e-30F,mix*mix*frameSquare+
                    2*mix*(1-mix)*refFraction+(1-mix)*(1-mix));
                o.confidence[c]=mix*std::min(1.0F,a.nearWeight[c])+(1-mix)*0.01F;
            } else if(referenceValid) {
                o.rgb[c]=r.mean[c];o.variance[c]=r.noise[c];o.effectiveFrames[c]=1;
                o.confidence[c]=0.01F; // interpolated reference fallback, not SR evidence
            }
        }
    }
}
std::unique_ptr<DirectTileExecutor> makeReferenceDirectExecutor(std::size_t n,const DirectKernelPolicy& p) {
    return std::make_unique<ReferenceExecutor>(n,p);
}
}  // namespace latent::reference
