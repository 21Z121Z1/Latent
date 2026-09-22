#include "latent/reference/DirectReconstruct.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace latent::reference {
namespace {
using Basis = std::array<float,kReconstructionBasis>;
using Moments = std::array<std::array<float,4>,kReconstructionMoments>;
constexpr std::size_t at(std::size_t i,std::size_t j) {return i*(i+1)/2+j;}
struct Stats {std::array<float,3> mean{}, noise{}, spatial{}, weight{};};
struct Bounds {int x0,y0,x1,y1;};
Bounds bounds(ReconstructionWarp w,const DirectKernelGeometry& g) {
    const int cx=static_cast<int>(std::floor(w.x)),cy=static_cast<int>(std::floor(w.y));
    return {std::max(static_cast<int>(g.sourceTile.x),cx-static_cast<int>(g.radiusX)),
        std::max(static_cast<int>(g.sourceTile.y),cy-static_cast<int>(g.radiusY)),
        std::min(static_cast<int>(g.sourceTile.x+g.sourceTile.width)-1,cx+static_cast<int>(g.radiusX)+1),
        std::min(static_cast<int>(g.sourceTile.y+g.sourceTile.height)-1,cy+static_cast<int>(g.radiusY)+1)};
}
const MosaicEvidence& sample(std::span<const MosaicEvidence> input,int x,int y,const DirectKernelGeometry& g) {
    return input[static_cast<std::size_t>(y-static_cast<int>(g.sourceTile.y))*g.sourceTile.width+
        static_cast<std::size_t>(x-static_cast<int>(g.sourceTile.x))];
}
Stats gather(std::span<const MosaicEvidence> input,ReconstructionWarp w,const DirectKernelGeometry& g) {
    Stats s{};std::array<float,3> variance{},sigma{},weightedNoise{};
    const auto b=bounds(w,g);const float inv=1/(g.fallbackSigma*g.fallbackSigma);
    for(int y=b.y0;y<=b.y1;++y)for(int x=b.x0;x<=b.x1;++x) {
        const auto& a=sample(input,x,y,g);if(!a.valid)continue;
        const auto c=a.channel==3U?2U:(a.channel==0U?0U:1U);
        const float dx=static_cast<float>(x)-w.x,dy=static_cast<float>(y)-w.y;
        const float k=std::exp(-0.5F*(dx*dx+dy*dy)*inv);
        if(k<=0)continue;
        // Weighted Welford avoids subtracting nearly equal squared means in
        // shadows/flat fields. Constants do not acquire fictitious texture.
        const float weight=s.weight[c]+k,delta=a.value-s.mean[c];
        s.mean[c]+=(k/weight)*delta;s.spatial[c]+=k*delta*(a.value-s.mean[c]);s.weight[c]=weight;
        variance[c]+=k*k*a.variance;weightedNoise[c]+=k*a.variance;
        if(g.correlatedSpatialNoise)sigma[c]+=k*std::sqrt(a.variance);
    }
    for(std::size_t c=0;c<3;++c)if(s.weight[c]>1e-20F) {
        s.noise[c]=(g.correlatedSpatialNoise?sigma[c]*sigma[c]:variance[c])/(s.weight[c]*s.weight[c]);
        // Noise in spatial scatter is E[sigma_i^2] - Var(weighted mean),
        // not Var(weighted mean). With unknown correlations subtract the
        // conservative upper bound E[sigma_i^2] instead of inventing texture.
        const float scatterNoise=std::max(0.0F,weightedNoise[c]/s.weight[c]-(g.correlatedSpatialNoise?0.0F:s.noise[c]));
        s.spatial[c]=std::max(0.0F,s.spatial[c]/s.weight[c]-scatterNoise);
    }
    return s;
}
float smoothGate(float x) {x=std::clamp(x,0.0F,1.0F);return x*x*(3-2*x);}
float phaseDiversity(const ReconstructionAccumulator& a,std::size_t c,float weight) {
    std::array<float,4> re{},im{};
    for(std::size_t j=0;j<4;++j){re[j]=a.phaseCos[j][c]/weight;im[j]=a.phaseSin[j][c]/weight;}
    const float vx=std::max(0.0F,1-re[0]*re[0]-im[0]*im[0]);
    const float vy=std::max(0.0F,1-re[1]*re[1]-im[1]*im[1]);
    const float cr=re[3]-(re[0]*re[1]+im[0]*im[1]);
    const float ci=im[3]-(im[0]*re[1]-re[0]*im[1]);
    const float pr=re[2]-(re[0]*re[1]-im[0]*im[1]);
    const float pi=im[2]-(im[0]*re[1]+re[0]*im[1]);
    // Conservatively penalize both correlation and pseudo-correlation. This
    // rejects repeated, one-axis, and diagonal phase trajectories. It need not
    // recognize every useful trajectory (in particular integer-CFA shifts).
    return std::clamp(0.5F*(vx+vy-std::sqrt((vx-vy)*(vx-vy)+4*(cr*cr+ci*ci+pr*pr+pi*pi))),0.0F,1.0F);
}
bool solveIntercept(const Moments& gram,std::size_t c,float weight,float regularization,Basis& h) {
    std::array<float,36> lower{};
    for(std::size_t i=0;i<6;++i)for(std::size_t j=0;j<=i;++j) {
        float x=gram[at(i,j)][c]/weight;
        if(i==j && i!=0)x+=regularization; // Never penalize the constant term.
        for(std::size_t k=0;k<j;++k)x-=lower[i*6+k]*lower[j*6+k];
        if(i==j) {if(!std::isfinite(x)||x<=1e-6F)return false;lower[i*6+j]=std::sqrt(x);}
        else lower[i*6+j]=x/lower[j*6+j];
    }
    Basis y{};
    for(std::size_t i=0;i<6;++i) {
        float x=i==0?1.0F:0.0F;for(std::size_t k=0;k<i;++k)x-=lower[i*6+k]*y[k];
        y[i]=x/lower[i*6+i];
    }
    for(int i=5;i>=0;--i) {
        const auto u=static_cast<std::size_t>(i);float x=y[u];
        for(std::size_t k=u+1;k<6;++k)x-=lower[k*6+u]*h[k];
        h[u]=x/lower[u*6+u];if(!std::isfinite(h[u]))return false;
    }
    return true;
}
void addFrameRoot(Moments& root,std::size_t c,Basis v) {
    // Incremental QR of rows of per-frame moment sums. Computing h^T F h
    // from the expanded Gram matrix loses significant digits at cropped
    // borders with signed fitted weights. ||R h||^2 is the same statistic.
    for(std::size_t j=0;j<6;++j) {
        const float diagonal=root[at(j,j)][c];
        const float length=std::sqrt(diagonal*diagonal+v[j]*v[j]);
        if(length==0)continue;
        const float cosine=diagonal/length,sine=v[j]/length;
        root[at(j,j)][c]=length;
        for(std::size_t k=j+1;k<6;++k) {
            const float old=root[at(k,j)][c];
            root[at(k,j)][c]=cosine*old+sine*v[k];v[k]=-sine*old+cosine*v[k];
        }
    }
}
float frameSquare(const Moments& root,std::size_t c,const Basis& h) {
    float sum=0;
    for(std::size_t j=0;j<6;++j){float v=0;for(std::size_t k=j;k<6;++k)v+=root[at(k,j)][c]*h[k];sum+=v*v;}
    return sum;
}
float quadraticForm(const Moments& matrix,std::size_t c,const Basis& h,bool absolute) {
    float result=0;
    for(std::size_t i=0;i<6;++i)for(std::size_t j=0;j<=i;++j) {
        // Multiply by the covariance before the second coefficient: h_i*h_j
        // can overflow for tiny support even when the quadratic form is finite.
        const float left=absolute?std::abs(h[i]):h[i],right=absolute?std::abs(h[j]):h[j];
        const float term=(matrix[at(i,j)][c]*right)*left;
        result+=(i==j?1.0F:2.0F)*term;
    }
    return std::max(0.0F,result); // Roundoff guard on a positive-semidefinite form.
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
    for(const auto& v:warp) {
        for(float j:v.sourceToReference)if(!std::isfinite(j)||std::abs(j)>16)
            throw std::invalid_argument("invalid reconstruction Jacobian");
        const auto& j=v.sourceToReference;const float det=j[0]*j[3]-j[1]*j[2];
        if(det<0.25F||det>4)throw std::invalid_argument("singular reconstruction Jacobian");
    }
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
        finishDirectReconstruction(std::span(anchors_).first(count_),std::span(accum_).first(count_),o,policy_);count_=0;
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
        !std::isfinite(p.relativeVarianceFloor)||p.relativeVarianceFloor<1e-12F||p.relativeVarianceFloor>0.01F ||
        !std::isfinite(p.minimumConfidence)||p.minimumConfidence<0||p.minimumConfidence>1 ||
        !std::isfinite(p.quadraticStrength)||p.quadraticStrength<0||p.quadraticStrength>1 ||
        !std::isfinite(p.fitRegularization)||p.fitRegularization<1e-4F||p.fitRegularization>0.1F ||
        !std::isfinite(p.maximumFitLeverage)||p.maximumFitLeverage<4||p.maximumFitLeverage>128)
        throw std::invalid_argument("invalid direct kernel policy");
}
void makeReconstructionAnchors(std::span<const MosaicEvidence> input,std::span<const ReconstructionWarp> warps,
    const DirectKernelGeometry& g,const DirectKernelPolicy& p,std::span<ReconstructionAnchor> out) {
    validateDirectKernelPolicy(p);validateGeometry(input,warps,g);
    if(out.size()!=warps.size()) throw std::invalid_argument("anchor size mismatch");
    for(std::size_t i=0;i<out.size();++i) {
        const auto s=gather(input,warps[i],g);
        out[i]={};
        for(std::size_t c=0;c<3;++c) {
            out[i].mean[c]=s.mean[c];out[i].noise[c]=s.noise[c];out[i].spatial[c]=s.spatial[c];
            if(s.weight[c]>1e-20F) out[i].mean[3]+=static_cast<float>(1U<<c);
        }
        // Default isotropic kernel is a conservative baseline. Anisotropy may
        // be installed by a validated guide-based lowering, not a color prior.
        const float inv=1/(p.detailSigma*p.detailSigma);
        out[i].precision={inv,0,inv,0};
        out[i].referenceWarp={warps[i].x,warps[i].y,0,0};
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
        const auto broad=g.referenceFrame ? Stats{} : gather(input,w,g);
        float amplitudeSquared=0;
        for(std::size_t c=0;c<3;++c)amplitudeSquared=std::max(amplitudeSquared,a.mean[c]*a.mean[c]+a.spatial[c]+a.noise[c]);
        if(amplitudeSquared==0)for(std::size_t c=0;c<3;++c)
            amplitudeSquared=std::max(amplitudeSquared,broad.mean[c]*broad.mean[c]+broad.spatial[c]+broad.noise[c]);
        const float floor=p.relativeVarianceFloor*amplitudeSquared;
        float robust=1;
        if(!g.referenceFrame)for(std::size_t c=0;c<3;++c) {
            if(broad.weight[c]<=1e-20F || (static_cast<unsigned>(a.mean[3])&(1U<<c))==0)continue;
            const float delta=broad.mean[c]-a.mean[c];
            const float scale=p.residualCutoff*p.residualCutoff*(a.noise[c]+broad.noise[c]+floor+
                p.aliasAllowance*(a.spatial[c]+broad.spatial[c]));
            const float r=scale>0?std::max(0.0F,1-delta*delta/scale):(delta==0?1.0F:0.0F);
            robust=std::min(robust,r*r);
        }
        robust*=g.referenceFrame?1:w.confidence;if(robust<=0)continue;
        std::array<float,3> precision{};
        for(std::size_t c=0;c<3;++c) {
            const float noise=a.noise[c]+(g.referenceFrame?a.noise[c]:broad.noise[c]);
            // A common scale bounds precision by one without changing relative
            // weights. All-zero noiseless evidence is explicitly well-defined.
            precision[c]=noise>0?floor/std::max(floor,noise):1;
        }
        auto& d=accum[i];
        const std::size_t basisCount=p.quadraticStrength>0?6U:1U;
        std::array<std::array<float,3>,6> frame{},sigma{};
        const auto b=bounds(w,g);const auto& j=w.sourceToReference;
        for(int y=b.y0;y<=b.y1;++y)for(int x=b.x0;x<=b.x1;++x) {
            const auto& v=sample(input,x,y,g);if(!v.valid)continue;
            const auto c=v.channel==3U?2U:(v.channel==0U?0U:1U);
            const float dx=static_cast<float>(x)-w.x,dy=static_cast<float>(y)-w.y;
            const float rx=j[0]*dx+j[1]*dy,ry=j[2]*dx+j[3]*dy;
            const float distance=rx*rx*a.precision[0]+2*rx*ry*a.precision[1]+ry*ry*a.precision[2];
            const float spatial=std::exp(-0.5F*distance),k=robust*precision[c]*spatial;
            const float u=rx/p.detailSigma,t=ry/p.detailSigma;
            const Basis phi{1,u,t,u*u,u*t,t*t};
            d.nearWeight[c]+=robust*spatial;
            for(std::size_t m=0;m<basisCount;++m) {
                d.signal[m][c]+=k*phi[m]*v.value;frame[m][c]+=k*phi[m];
                if(g.correlatedSpatialNoise)sigma[m][c]+=k*std::abs(phi[m])*std::sqrt(v.variance);
                for(std::size_t n=0;n<=m;++n) {
                    const float moment=phi[m]*phi[n];d.gram[at(m,n)][c]+=k*moment;
                    if(!g.correlatedSpatialNoise)d.noiseGram[at(m,n)][c]+=k*k*v.variance*moment;
                }
            }
        }
        const float dx=w.x-a.referenceWarp[0],dy=w.y-a.referenceWarp[1];
        const std::array<float,4> phase{dx,dy,dx+dy,dx-dy};
        for(std::size_t c=0;c<3;++c) {
            for(std::size_t m=0;m<basisCount;++m) {
                if(g.referenceFrame)d.referenceMoments[m][c]=frame[m][c];
                for(std::size_t n=0;n<=m;++n) {
                    if(g.correlatedSpatialNoise)d.noiseBoundGram[at(m,n)][c]+=sigma[m][c]*sigma[n][c];
                }
            }
            Basis frameMoments{};for(std::size_t m=0;m<6;++m)frameMoments[m]=frame[m][c];
            addFrameRoot(d.frameRoot,c,frameMoments);
            for(std::size_t m=0;m<4;++m) {
                const float angle=6.283185307179586F*(phase[m]-std::floor(phase[m]));
                d.phaseCos[m][c]+=frame[0][c]*std::cos(angle);d.phaseSin[m][c]+=frame[0][c]*std::sin(angle);
            }
        }
    }
}
void finishDirectReconstruction(std::span<const ReconstructionAnchor> anchors,std::span<const ReconstructionAccumulator> accum,
    std::span<ReconstructedPixel> out,const DirectKernelPolicy& p) {
    validateDirectKernelPolicy(p);
    if(anchors.size()!=accum.size()||out.size()!=accum.size())throw std::invalid_argument("output size mismatch");
    for(std::size_t i=0;i<out.size();++i) {
        out[i]={};
        for(std::size_t c=0;c<3;++c) {
            const auto& a=accum[i];const auto& r=anchors[i];auto& o=out[i];
            const bool referenceValid=(static_cast<unsigned>(r.mean[3])&(1U<<c))!=0;
            const float weight=a.gram[0][c];
            if(weight>1e-20F) {
                const float support=referenceValid?smoothGate((a.nearWeight[c]-0.02F)/0.08F):1;
                const float diversity=phaseDiversity(a,c,weight);
                if(support==0) {
                    // Do not evaluate an inactive, arbitrarily ill-scaled fit.
                    // IEEE 0*infinity is NaN, not the reference-only estimate.
                    o.rgb[c]=r.mean[c];o.variance[c]=r.noise[c];o.effectiveFrames[c]=1;
                    o.confidence[c]=0.01F;o.samplingDiversity[c]=diversity;continue;
                }
                const float snr=r.spatial[c]+4*r.noise[c];
                float blend=p.quadraticStrength*smoothGate((diversity-0.01F)/0.25F)*(snr>0?r.spatial[c]/snr:0);
                Basis h{};
                if(blend>0) {
                    if(!solveIntercept(a.gram,c,weight,p.fitRegularization,h)){blend=0;h.fill(0);}
                    else blend*=smoothGate((p.maximumFitLeverage-h[0])/(0.5F*p.maximumFitLeverage));
                }
                for(float& v:h)v*=blend;
                h[0]+=1-blend;
                for(float& v:h)v/=weight;
                float value=0,refFraction=0;
                for(std::size_t m=0;m<6;++m){value+=h[m]*a.signal[m][c];refFraction+=h[m]*a.referenceMoments[m][c];}
                const float variance=quadraticForm(a.noiseGram,c,h,false)+quadraticForm(a.noiseBoundGram,c,h,true);
                const float frameNorm=frameSquare(a.frameRoot,c,h);
                o.rgb[c]=support*value+(1-support)*r.mean[c];
                // Signed fitted weights require the full covariance quadratic
                // form. Reference fallback shares samples: bound that mixture.
                const float sigma=support*std::sqrt(variance)+(1-support)*std::sqrt(r.noise[c]);
                o.variance[c]=sigma*sigma;
                o.effectiveFrames[c]=1/std::max(1e-30F,support*support*frameNorm+
                    2*support*(1-support)*refFraction+(1-support)*(1-support));
                o.confidence[c]=support*std::min(1.0F,a.nearWeight[c])+(1-support)*0.01F;
                o.samplingDiversity[c]=diversity;o.modelBlend[c]=support*blend;
            } else if(referenceValid) {
                o.rgb[c]=r.mean[c];o.variance[c]=r.noise[c];o.effectiveFrames[c]=1;o.confidence[c]=0.01F;
            }
        }
    }
}
std::unique_ptr<DirectTileExecutor> makeReferenceDirectExecutor(std::size_t n,const DirectKernelPolicy& p) {
    return std::make_unique<ReferenceExecutor>(n,p);
}
}  // namespace latent::reference
