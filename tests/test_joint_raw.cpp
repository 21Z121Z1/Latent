#include "latent/reference/DirectReconstruct.h"
#include "latent/reference/TemporalReconstruct.h"
#include "latent/testing/SyntheticRaw.h"
#ifdef LATENT_JOINT_TEST_VULKAN
#include "latent/vulkan/DirectReconstruction.h"
#include <cstdlib>
#endif
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>

using namespace latent;
namespace {
using reference::MosaicEvidence;
using reference::ReconstructionWarp;
using reference::ReconstructedPixel;
std::size_t checks=0;
#ifdef LATENT_JOINT_TEST_VULKAN
float maxBackendError=0;
#endif
void check(bool x,const char* what) {++checks;if(!x)throw std::runtime_error(what);}
template<class F>void rejects(F f,const char* what) {bool yes=false;try{f();}catch(const std::exception&){yes=true;}check(yes,what);}
constexpr float pi=3.14159265358979323846F;
using Signal=std::function<float(float,float,std::size_t)>;
struct Frame {std::vector<MosaicEvidence> raw;std::vector<ReconstructionWarp> warps;};
struct Fixture {
    imaging::Extent source{40,36},output{25,21};
    float originX=10,originY=9,step=0.5F;
    reference::DirectKernelGeometry geometry{{0,0,40,36},2,2,1,false,false};
    std::vector<Frame> frames;
};
std::vector<std::array<float,2>> cartesian() {
    std::vector<std::array<float,2>> shifts;
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)shifts.push_back({0.5F*static_cast<float>(x),0.5F*static_cast<float>(y)});
    return shifts;
}
// Independent 4x4 midpoint integration over the pixel aperture. No call to the
// reconstruction sampler or the runtime's two-point synthetic image generator.
float aperture(const Signal& signal,float x,float y,std::size_t c) {
    float sum=0;for(unsigned v=0;v<4;++v)for(unsigned u=0;u<4;++u)
        sum+=signal(x+(static_cast<float>(u)+0.5F)*0.25F-0.5F,y+(static_cast<float>(v)+0.5F)*0.25F-0.5F,c);
    return sum/16;
}
Fixture fixture(const Signal& signal,const std::vector<std::array<float,2>>& shifts,float variance=0,std::uint32_t group=1) {
    Fixture f{};f.geometry.radiusX=group+1;f.geometry.radiusY=group+1;
    f.geometry.fallbackSigma=std::max(1.0F,0.75F*static_cast<float>(group));
    const auto burst=testing::syntheticBurst(f.source,1,group,group,imaging::CfaPattern::GRBG,3,1);
    const auto sampling=*burst.members.front().observations.sampling;
    for(const auto& shift:shifts) {
        Frame frame{};frame.raw.resize(static_cast<std::size_t>(f.source.pixelCount()));
        for(std::uint32_t y=0;y<f.source.height;++y)for(std::uint32_t x=0;x<f.source.width;++x) {
            const auto channel=static_cast<std::uint32_t>(imaging::samplingChannelAt(sampling,x,y));
            const auto c=channel==3?2U:(channel==0?0U:1U);
            frame.raw[static_cast<std::size_t>(y)*f.source.width+x]={aperture(signal,static_cast<float>(x)-shift[0],static_cast<float>(y)-shift[1],c),variance,channel,1};
        }
        for(std::uint32_t y=0;y<f.output.height;++y)for(std::uint32_t x=0;x<f.output.width;++x)
            frame.warps.push_back({f.originX+static_cast<float>(x)*f.step+shift[0],f.originY+static_cast<float>(y)*f.step+shift[1],1,0});
        f.frames.push_back(std::move(frame));
    }
    return f;
}
std::vector<ReconstructedPixel> executeFixture(const Fixture& f,reference::DirectTileExecutor& executor) {
    auto g=f.geometry;g.referenceFrame=true;executor.begin(f.frames[0].raw,f.frames[0].warps,g);
    g.referenceFrame=false;for(std::size_t i=1;i<f.frames.size();++i)executor.add(f.frames[i].raw,f.frames[i].warps,g);
    std::vector<ReconstructedPixel> out(static_cast<std::size_t>(f.output.pixelCount()));executor.finish(out);return out;
}
std::vector<ReconstructedPixel> reconstruct(const Fixture& f,reference::DirectKernelPolicy p={}) {
    auto cpu=reference::makeReferenceDirectExecutor(static_cast<std::size_t>(f.output.pixelCount()),p);
    const auto oracle=executeFixture(f,*cpu);
#ifdef LATENT_JOINT_TEST_VULKAN
    std::string detail;
    auto gpu=vulkan::makeVulkanDirectExecutor(static_cast<std::size_t>(f.source.pixelCount()),static_cast<std::size_t>(f.output.pixelCount()),p,&detail);
    check(gpu!=nullptr,"joint GPU property test cannot fall back");
    const auto out=executeFixture(f,*gpu);
    for(std::size_t i=0;i<out.size();++i)for(std::size_t c=0;c<3;++c) {
        const auto& a=oracle[i];const auto& b=out[i];
        const float error=std::abs(a.rgb[c]-b.rgb[c])/std::max(1.0F,std::abs(a.rgb[c]));maxBackendError=std::max(maxBackendError,error);
        check(error<3e-5F,"joint SR/gain CPU/GPU signal mismatch");
        check(std::abs(a.variance[c]-b.variance[c])<1e-7F+1e-3F*a.variance[c],"joint SR/gain CPU/GPU variance mismatch");
        check(std::abs(a.effectiveFrames[c]-b.effectiveFrames[c])<2e-3F,"joint SR/gain CPU/GPU frame support mismatch");
        check(std::abs(a.confidence[c]-b.confidence[c])<5e-4F,"joint SR/gain CPU/GPU confidence mismatch");
        check(std::abs(a.samplingDiversity[c]-b.samplingDiversity[c])<3e-5F,"joint SR/gain CPU/GPU phase mismatch");
        check(std::abs(a.modelBlend[c]-b.modelBlend[c])<1e-4F,"joint SR/gain CPU/GPU model mismatch");
    }
    return out;
#else
    return oracle;
#endif
}
double mse(const Fixture& f,const std::vector<ReconstructedPixel>& out,const Signal& signal) {
    double error=0;for(std::uint32_t y=0;y<f.output.height;++y)for(std::uint32_t x=0;x<f.output.width;++x)for(std::size_t c=0;c<3;++c) {
        const double delta=out[static_cast<std::size_t>(y)*f.output.width+x].rgb[c]-aperture(signal,f.originX+static_cast<float>(x)*f.step,f.originY+static_cast<float>(y)*f.step,c);
        error+=delta*delta;
    }
    return error/static_cast<double>(out.size()*3);
}
void srQuality() {
    double worstRatio=0;
    for(float frequency:{0.20F,0.35F,0.60F})for(unsigned axis=0;axis<3;++axis) {
        const Signal signal=[=](float x,float y,std::size_t c) {
            const float coordinate=axis==0?x:(axis==1?y:(x+y)*0.7071067811865475F);
            return 0.3F+0.1F*static_cast<float>(c)+0.10F*std::sin(2*pi*frequency*coordinate+0.3F*static_cast<float>(c));
        };
        const auto f=fixture(signal,cartesian());
        auto p=reference::DirectKernelPolicy::superResolution();
        const auto joint=reconstruct(f,p);p.quadraticStrength=0;const auto gaussian=reconstruct(f,p);
        auto single=f;single.frames.resize(1);const auto one=reconstruct(single,p);
        const double ratio=mse(f,joint,signal)/mse(f,gaussian,signal);worstRatio=std::max(worstRatio,ratio);
        double blend=0;for(const auto& v:joint)blend+=v.modelBlend[0];blend/=static_cast<double>(joint.size());
        std::cout<<"joint-sr frequency="<<frequency<<" axis="<<axis<<" mse="<<mse(f,joint,signal)
            <<" gaussian_mse="<<mse(f,gaussian,signal)<<" single_mse="<<mse(f,one,signal)<<" ratio="<<ratio<<" blend="<<blend<<'\n';
        check(blend>0.1,"SR test must actually exercise joint fitting");
        check(ratio<0.85,"joint fit must improve the Gaussian ablation, including above native Nyquist");
        check(mse(f,joint,signal)<mse(f,one,signal),"joint SR must beat a single RAW reconstructed on the SAME grid");
    }
    std::cout<<"joint-sr worst_mse_ratio="<<worstRatio<<'\n';
}
// Exercise the actual retained native-grid fusion and demosaic implementation,
// not a reimplementation of its sampler. Exact geometry and a permissive robust
// cutoff keep valid phase changes from becoming a motion-rejection straw man.
std::vector<ReconstructedPixel> nativeGridBaseline(const Fixture& f) {
    reference::NormalizedRaw ref{}; ref.extent=f.source;
    bool matched=false;
    for(unsigned pattern=0;pattern<4;++pattern) {
        const auto cfa=static_cast<imaging::CfaPattern>(pattern);bool same=true;
        for(std::uint32_t y=0;y<2;++y)for(std::uint32_t x=0;x<2;++x) {
            const auto actual=static_cast<imaging::CfaChannel>(f.frames[0].raw[static_cast<std::size_t>(y)*f.source.width+x].channel);
            same= same && reference::rgbChannelIndex(imaging::cfaChannelAt(cfa,x,y))==reference::rgbChannelIndex(actual);
        }
        if(same){ref.cfa=cfa;matched=true;break;}
    }
    check(matched,"legacy comparator requires a regular Bayer spectral layout");
    const auto copySamples=[](reference::NormalizedRaw& n,const Frame& frame) {
        n.samples.clear();n.samples.reserve(frame.raw.size());
        for(const auto& sample:frame.raw)n.samples.push_back({sample.value,sample.variance,static_cast<float>(sample.valid),1});
    };
    copySamples(ref,f.frames.front());
    reference::TemporalPolicy policy{};policy.residualCutoffSigma=100;
    policy.varianceFloor=1e-4F;policy.missingNoiseVariance=1e-4F;
    auto session=reference::makeReferenceFusionSession(ref,policy);
    for(std::size_t i=0;i<f.frames.size();++i) {
        auto source=ref;copySamples(source,f.frames[i]);
        reference::AlignmentField field{};field.source=imaging::FrameId{i+1};field.reference=imaging::FrameId{1};
        field.extent=f.source;field.tileSize=policy.tileSize;
        field.columns=(f.source.width+field.tileSize-1)/field.tileSize;
        field.rows=(f.source.height+field.tileSize-1)/field.tileSize;
        field.tiles.assign(static_cast<std::size_t>(field.columns)*field.rows,
            {f.frames[i].warps[0].x-f.originX,f.frames[i].warps[0].y-f.originY,1,0});
        (void)session->add(source,field,i==0);
    }
    const auto fused=reference::finishTemporalFusion(ref,session->finish(),{});
    const auto rgb=reference::demosaicSensorLinear(fused.sensor,{1,1,1,1},reference::DemosaicMethod::MalvarHeCutler2004);
    std::vector<ReconstructedPixel> out(static_cast<std::size_t>(f.output.pixelCount()));
    for(std::uint32_t y=0;y<f.output.height;++y)for(std::uint32_t x=0;x<f.output.width;++x) {
        const float rx=f.originX+static_cast<float>(x)*f.step,ry=f.originY+static_cast<float>(y)*f.step;
        const auto ix=static_cast<std::uint32_t>(rx),iy=static_cast<std::uint32_t>(ry);
        const float tx=rx-static_cast<float>(ix),ty=ry-static_cast<float>(iy);
        // Baseline only: explicit bilinear display-grid resampling after the
        // native CFA evidence has already collapsed. Joint never calls this.
        for(std::uint32_t j=0;j<2;++j)for(std::uint32_t k=0;k<2;++k) {
            const float w=(k?tx:1-tx)*(j?ty:1-ty);
            const auto index=static_cast<std::size_t>(iy+j)*f.source.width+ix+k;
            check(fused.uncertainty.effectiveSampleCount[index]>15,
                "legacy comparison must retain almost all 16 phase frames, not fall back to reference");
            for(std::size_t c=0;c<3;++c)out[static_cast<std::size_t>(y)*f.output.width+x].rgb[c]+=w*rgb.rgb[index*3+c];
        }
    }
    return out;
}
void nativeGridPhaseEvidence() {
    double worstRatio=0;
    for(float frequency:{0.35F,0.60F})for(unsigned axis=0;axis<2;++axis)for(std::size_t active=0;active<3;++active) {
        const Signal signal=[=](float x,float y,std::size_t c) {
            return 0.3F+0.1F*static_cast<float>(c)+(c==active?0.1F*std::sin(2*pi*frequency*(axis?y:x)):0.0F);
        };
        const auto f=fixture(signal,cartesian());
        const auto joint=reconstruct(f,reference::DirectKernelPolicy::superResolution());
        const auto collapsed=nativeGridBaseline(f);
        const double jointError=mse(f,joint,signal),collapsedError=mse(f,collapsed,signal);
        const double ratio=jointError/collapsedError;worstRatio=std::max(worstRatio,ratio);
        std::cout<<"joint-vs-native frequency="<<frequency<<" axis="<<axis<<" color="<<active
            <<" joint_mse="<<jointError<<" fused_demosaic_upscale_mse="<<collapsedError<<" ratio="<<ratio<<'\n';
        check(jointError<collapsedError,"fractional RAW evidence must beat actual native FusedRaw collapse on independent color stripes");
        for(const auto& pixel:joint)for(std::size_t c=0;c<3;++c)if(c!=active)
            check(std::abs(pixel.rgb[c]-(0.3F+0.1F*static_cast<float>(c)))<2e-5F,"independent color stripe must not contaminate flat spectral axes");
    }
    std::cout<<"joint-vs-native worst_mse_ratio="<<worstRatio<<'\n';
}
void homogeneityAndSignedValues() {
    const Signal signal=[](float x,float y,std::size_t c){return -0.12F+0.25F*static_cast<float>(c)+0.08F*std::sin(x*0.7F+y*0.3F);};
    auto f=fixture(signal,cartesian(),1e-5F);
    // A real photometric outlier exercises adaptive rejection under gain changes.
    for(auto& v:f.frames.back().raw)v.value+=0.09F;
    const auto baseline=reconstruct(f);
    float maxError=0,maxVarianceError=0,maxEvidenceError=0;
    for(float gain:{0.000244140625F,0.001953125F,0.125F,8.0F,512.0F,0.00073F,3.7F,700.0F}) {
        auto scaled=f;
        for(auto& frame:scaled.frames)for(auto& v:frame.raw){v.value*=gain;v.variance*=gain*gain;}
        const auto out=reconstruct(scaled);
        for(std::size_t i=0;i<out.size();++i)for(std::size_t c=0;c<3;++c) {
            maxError=std::max(maxError,std::abs(out[i].rgb[c]/gain-baseline[i].rgb[c]));
            maxVarianceError=std::max(maxVarianceError,std::abs(out[i].variance[c]/(gain*gain)-baseline[i].variance[c]));
            maxEvidenceError=std::max(maxEvidenceError,std::abs(out[i].modelBlend[c]-baseline[i].modelBlend[c]));
            check(std::abs(out[i].samplingDiversity[c]-baseline[i].samplingDiversity[c])<2e-5F,"phase evidence is degree zero");
            check(std::abs(out[i].effectiveFrames[c]-baseline[i].effectiveFrames[c])<2e-4F,"effective support is degree zero");
        }
    }
    check(maxError<2e-6F && maxVarianceError<2e-9F && maxEvidenceError<2e-5F,"intensity/variance equivariance at fixed correspondences");
    check(baseline.front().rgb[0]<0,"sub-black camera values must survive joint reconstruction");
    for(float level:{0.0F,-0.001F,-2.0F,4.0F}) {
        const Signal flat=[=](float,float,std::size_t){return level;};
        const auto out=reconstruct(fixture(flat,cartesian()));
        for(const auto& v:out)for(std::size_t c=0;c<3;++c) {
            check(std::isfinite(v.rgb[c])&&std::abs(v.rgb[c]-level)<2e-5F,"zero/signed/super-white constants remain finite and unclipped");
            check(v.modelBlend[c]==0,"flat evidence must not be promoted to texture");
        }
    }
    std::cout<<"joint-gain max_rgb_error="<<maxError<<" max_variance_error="<<maxVarianceError<<" max_model_error="<<maxEvidenceError<<'\n';
}
void phaseAndAxisControls() {
    const Signal signal=[](float x,float,std::size_t c){return 0.3F+0.07F*std::sin(1.2F*x+static_cast<float>(c));};
    for(unsigned path=0;path<3;++path) {
        std::vector<std::array<float,2>> shifts;
        for(unsigned i=0;i<8;++i) {const float d=static_cast<float>(i)*0.25F;shifts.push_back(path==0?std::array<float,2>{0,0}:(path==1?std::array<float,2>{d,0}:std::array<float,2>{d,d}));}
        const auto out=reconstruct(fixture(signal,shifts,1e-5F));
        for(const auto& v:out)for(std::size_t c=0;c<3;++c) {
            check(v.samplingDiversity[c]<2e-5F,"degenerate phase path must not claim 2D diversity");
            check(v.modelBlend[c]==0,"degenerate phase path must use Gaussian fallback");
        }
    }
    const auto f=fixture(signal,cartesian());const auto out=reconstruct(f);
    float crossAxis=0;
    // Compare equal Bayer/output phases as well as adjacent output phases. With
    // complete 2x2 CFA phase coverage the axis-constant target has no y texture.
    for(std::uint32_t y=1;y<f.output.height;++y)for(std::uint32_t x=0;x<f.output.width;++x)for(std::size_t c=0;c<3;++c)
        crossAxis=std::max(crossAxis,std::abs(out[static_cast<std::size_t>(y)*f.output.width+x].rgb[c]-out[x].rgb[c]));
    std::cout<<"joint-axis cross_axis_max="<<crossAxis<<'\n';
    check(crossAxis<0.002F,"axis control must not invent significant orthogonal periodic color texture");
}
void spatialNoiseDebias() {
    const Signal signal=[](float x,float y,std::size_t c){return 0.3F+0.03F*std::sin(x*1.3F+y*0.9F+static_cast<float>(c));};
    auto f=fixture(signal,{{0,0}},0.0002F);f.frames[0].warps.resize(1);
    auto g=f.geometry;g.referenceFrame=true;
    for(bool correlated:{false,true}) {
        g.correlatedSpatialNoise=correlated;std::array<reference::ReconstructionAnchor,1> a;
        reference::makeReconstructionAnchors(f.frames[0].raw,f.frames[0].warps,g,{},a);
        double weight=0,sum=0,square=0,weightedNoise=0,meanNoise=0;
        const auto w=f.frames[0].warps[0];
        const int cx=static_cast<int>(std::floor(w.x)),cy=static_cast<int>(std::floor(w.y));
        for(int y=cy-2;y<=cy+3;++y)for(int x=cx-2;x<=cx+3;++x) {
            const auto& v=f.frames[0].raw[static_cast<std::size_t>(y)*f.source.width+static_cast<std::size_t>(x)];if(v.channel!=0)continue;
            const double dx=x-static_cast<double>(w.x),dy=y-static_cast<double>(w.y),k=std::exp(-0.5*(dx*dx+dy*dy));
            weight+=k;sum+=k*v.value;square+=k*v.value*v.value;weightedNoise+=k*v.variance;meanNoise+=k*k*v.variance;
        }
        const double scatter=square/weight-(sum/weight)*(sum/weight);
        const double expected=std::max(0.0,scatter-weightedNoise/weight+(correlated?0.0:meanNoise/(weight*weight)));
        check(std::abs(a[0].spatial[0]-expected)<1e-8,"spatial scatter must subtract sample noise, not just mean-estimate noise");
    }
}
void conditionalNoise(unsigned mode) {
    const Signal flat=[](float,float,std::size_t c){return 0.25F+0.1F*static_cast<float>(c);};
    auto f=fixture(flat,cartesian(),1e-5F);f.output={1,1};
    for(auto& frame:f.frames)frame.warps.resize(1);
    reference::DirectKernelPolicy p{};p.detailSigma=0.5F;
    auto g=f.geometry;g.referenceFrame=true;
    std::array<reference::ReconstructionAnchor,1> anchor;
    reference::makeReconstructionAnchors(f.frames[0].raw,f.frames[0].warps,g,p,anchor);
    // Fixed accepted statistics for a textured neighborhood: this test isolates
    // the conditional linear operator, not a claim of adaptive calibration.
    anchor[0].spatial={0.04F,0.04F,0.04F,0};
    std::array<reference::ReconstructionAccumulator,1> accumulated{};
    for(std::size_t i=0;i<f.frames.size();++i) {
        g.correlatedSpatialNoise=mode==2 || (mode==1 && i%2!=0);
        g.referenceFrame=i==0;reference::accumulateDirectReconstruction(f.frames[i].raw,f.frames[i].warps,anchor,g,p,accumulated);
    }
    std::array<ReconstructedPixel,1> result{};
    reference::finishDirectReconstruction(anchor,accumulated,result,p);
    check(result[0].modelBlend[0]>0.5F,"variance test must exercise signed joint-fit coefficients");
    // Recover the fixed linear map from moment impulses. Independently form
    // individual RAW sample coefficients and compare sum a_i^2 sigma_i^2.
    std::array<float,6> h{};
    auto impulse=accumulated;for(auto& v:impulse[0].signal)v.fill(0);
    for(std::size_t m=0;m<6;++m) {
        impulse[0].signal[m][0]=1;std::array<ReconstructedPixel,1> response;
        reference::finishDirectReconstruction(anchor,impulse,response,p);h[m]=response[0].rgb[0];impulse[0].signal[m][0]=0;
    }
    std::vector<float> coefficients,randomCoefficients;std::vector<std::size_t> frameEnds;
    float amplitudeSquared=0;for(std::size_t c=0;c<3;++c)
        amplitudeSquared=std::max(amplitudeSquared,anchor[0].mean[c]*anchor[0].mean[c]+anchor[0].spatial[c]+anchor[0].noise[c]);
    const float floor=p.relativeVarianceFloor*amplitudeSquared;
    for(std::size_t frame=0;frame<f.frames.size();++frame) {
        const auto w=f.frames[frame].warps[0];
        const int cx=static_cast<int>(std::floor(w.x)),cy=static_cast<int>(std::floor(w.y));
        double broadWeight=0,broadVariance=0;
        for(int y=cy-2;y<=cy+3;++y)for(int x=cx-2;x<=cx+3;++x) {
            const auto& v=f.frames[frame].raw[static_cast<std::size_t>(y)*f.source.width+static_cast<std::size_t>(x)];
            if(v.channel!=0)continue;
            const double dx=x-static_cast<double>(w.x),dy=y-static_cast<double>(w.y);
            const double k=std::exp(-0.5*(dx*dx+dy*dy));broadWeight+=k;broadVariance+=k*k*v.variance;
        }
        const bool correlated=mode==2 || (mode==1 && frame%2!=0);
        const float noise=anchor[0].noise[0]+(frame==0?anchor[0].noise[0]:(correlated?1e-5F:static_cast<float>(broadVariance/(broadWeight*broadWeight))));
        const float precision=floor/std::max(floor,noise);
        for(int y=cy-2;y<=cy+3;++y)for(int x=cx-2;x<=cx+3;++x) {
            const auto& v=f.frames[frame].raw[static_cast<std::size_t>(y)*f.source.width+static_cast<std::size_t>(x)];if(v.channel!=0)continue;
            const float u=(static_cast<float>(x)-w.x)/p.detailSigma,t=(static_cast<float>(y)-w.y)/p.detailSigma;
            const std::array<float,6> phi{1,u,t,u*u,u*t,t*t};
            float coefficient=0;for(std::size_t j=0;j<6;++j)coefficient+=h[j]*phi[j];
            coefficients.push_back(coefficient*precision*std::exp(-0.5F*(u*u+t*t)));
        }
        const auto begin=frameEnds.empty()?0:frameEnds.back();
        if(correlated)randomCoefficients.push_back(std::accumulate(coefficients.begin()+static_cast<std::ptrdiff_t>(begin),coefficients.end(),0.0F));
        else randomCoefficients.insert(randomCoefficients.end(),coefficients.begin()+static_cast<std::ptrdiff_t>(begin),coefficients.end());
        frameEnds.push_back(coefficients.size());
    }
    double sum=0,variance=0,frameNorm=0;bool negative=false;std::size_t begin=0;
    for(float a:coefficients){sum+=a;negative|=a<0;}
    for(float a:randomCoefficients)variance+=a*a*1e-5;
    for(auto end:frameEnds) {
        const double contribution=std::accumulate(coefficients.begin()+static_cast<std::ptrdiff_t>(begin),coefficients.begin()+static_cast<std::ptrdiff_t>(end),0.0);
        frameNorm+=contribution*contribution;begin=end;
    }
    check(std::abs(result[0].effectiveFrames[0]*frameNorm-1)<1e-4,"QR frame root must match direct signed frame coefficients");
    check(negative,"joint variance oracle must include negative RAW coefficients");
    check(std::abs(sum-1)<2e-5,"unpenalized intercept must preserve DC");
    if(mode==0)check(std::abs(variance-result[0].variance[0])<variance*0.002,"full moment covariance must equal independent RAW coefficient oracle");
    else check(result[0].variance[0]>=variance*0.998,"correlated/mixed noise bound must not understate the RAW coefficient oracle");
    std::mt19937 random(8743);std::normal_distribution<float> gaussian(0,1);
    constexpr unsigned trials=4096;double mean=0,second=0;
    for(unsigned trial=0;trial<trials;++trial){double noise=0;for(float a:randomCoefficients)noise+=a*gaussian(random)*std::sqrt(1e-5F);mean+=noise;second+=noise*noise;}
    mean/=trials;const double measured=second/trials-mean*mean;
    check(measured/variance>0.90&&measured/variance<1.10,"fixed-weight Monte Carlo variance calibration");
    std::cout<<"joint-variance mode="<<mode<<" predicted="<<result[0].variance[0]<<" raw_oracle="<<variance<<" monte_carlo_ratio="<<measured/variance<<" dc_gain="<<sum<<'\n';
}
void sparseColorBorders() {
    // Regression from the full 2x, 4x4 grouped-CFA benchmark. Tiny Gaussian
    // support at a cropped red/blue border must use the finite reference path,
    // never evaluate 0*sqrt(infinity) in an inactive uncertainty branch.
    const auto b=testing::syntheticBurst({96,80},8,4,4,imaging::CfaPattern::RGGB,0,0,0,1e-5F);
    const auto motion=testing::translations(8,4.0F);testing::SyntheticRawSource source(b,motion);
    const runtime::ReconstructionGrid grid{{191,159},0,0,0.5F,0.5F};
    runtime::TiledReconstructionPolicy p{};p.tileSize=64;p.kernel=reference::DirectKernelPolicy::superResolution();
    runtime::ReconstructionCapabilities caps{};
#ifdef LATENT_JOINT_TEST_VULKAN
    caps.preferVulkan=true;
#endif
    testing::ImageSink sink(grid.extent);
    const auto trace=runtime::reconstructRawTiles(b,source,grid,p,caps,sink,motion,imaging::FrameId{1});
#ifdef LATENT_JOINT_TEST_VULKAN
    check(trace.plan.vulkan,"grouped border regression must execute Vulkan");
#endif
    check(trace.invalidChannels==0,"grouped border reference evidence remains available");
    check(trace.referenceFallbackChannels>0,"grouped border must exercise reference fallback");
    for(const auto& pixel:sink.pixels)for(std::size_t c=0;c<3;++c) {
        check(std::isfinite(pixel.rgb[c])&&std::isfinite(pixel.variance[c])&&pixel.variance[c]>=0,"grouped border signal/variance finite");
        check(std::isfinite(pixel.effectiveFrames[c])&&pixel.confidence[c]>0,"grouped border support finite");
    }
    // Conditional operator regression: coefficient product alone exceeds FP32,
    // but multiplying by covariance first leaves a finite, representable form.
    std::array<reference::ReconstructionAnchor,1> anchors{};
    std::array<reference::ReconstructionAccumulator,1> accum{};
    std::array<ReconstructedPixel,1> out{};
    accum[0].gram[0][0]=3e-20F;accum[0].signal[0][0]=6e-21F;
    accum[0].noiseGram[0][0]=1e-30F;accum[0].frameRoot[0][0]=3e-20F;accum[0].nearWeight[0]=1;
    reference::finishDirectReconstruction(anchors,accum,out);
    check(std::isfinite(out[0].variance[0])&&std::abs(out[0].variance[0]/(1e-30F/3e-20F/3e-20F)-1)<1e-5F,"covariance product ordering must avoid coefficient-square overflow");
}
void runtimeTilingAndValidation() {
    const auto b=testing::syntheticBurst({57,49},8,2,3);const auto motion=testing::translations(8,2.0F);
    testing::SyntheticRawSource source(b,motion);
    const runtime::ReconstructionGrid grid{{89,73},3,4,0.5F,0.5F};
    testing::ImageSink a(grid.extent),z(grid.extent);
    runtime::TiledReconstructionPolicy p{};p.tileSize=16;
    const auto trace=runtime::reconstructRawTiles(b,source,grid,p,{},a,motion,imaging::FrameId{1});
    p.tileSize=64;(void)runtime::reconstructRawTiles(b,source,grid,p,{},z,motion,imaging::FrameId{1});
    for(std::size_t i=0;i<a.pixels.size();++i) {
        check(a.pixels[i].rgb==z.pixels[i].rgb&&a.pixels[i].variance==z.pixels[i].variance,"joint tile/halo equivalence");
        check(a.pixels[i].samplingDiversity==z.pixels[i].samplingDiversity&&a.pixels[i].modelBlend==z.pixels[i].modelBlend,"global phase coordinates independent of tile partition");
    }
    check(trace.jointFitChannels>0,"runtime must use the joint estimator");
    const auto json=runtime::directReconstructionJson(trace);
    check(json.find("latent.direct-cfa.2")!=std::string::npos&&json.find("relative_variance_floor")!=std::string::npos,"trace version and actual kernel policy");
    auto bad=motion;bad[1].rowDy=-static_cast<float>(b.extent.height-1);
    rejects([&]{(void)runtime::reconstructRawTiles(b,source,grid,p,{},z,bad,imaging::FrameId{1});},"row-augmented singular Jacobian must fail before reconstruction");
    auto kernel=p.kernel;kernel.fitRegularization=0;rejects([&]{reference::validateDirectKernelPolicy(kernel);},"unregularized ill-conditioned fit policy rejected");
    auto f=fixture([](float,float,std::size_t){return 0.2F;},{{0,0}});
    f.frames[0].warps[0].sourceToReference={1,0,0,0};rejects([&]{(void)reconstruct(f);},"singular kernel Jacobian rejected");
    f.frames[0].warps[0].sourceToReference={1,0,0,1};for(auto& v:f.frames[0].raw)v.valid=0;
    for(const auto& pixel:reconstruct(f))for(std::size_t c=0;c<3;++c)check(pixel.confidence[c]==0&&pixel.modelBlend[c]==0,"empty evidence remains explicitly unavailable");
}
}
int main() {
    try {
#ifdef LATENT_JOINT_TEST_VULKAN
        std::string detail;auto probe=vulkan::makeVulkanDirectExecutor(1,1,{},&detail);
        if(!probe) {
            if(std::getenv("LATENT_REQUIRE_VULKAN"))throw std::runtime_error("mandatory joint Vulkan unavailable: "+detail);
            std::cout<<"joint Vulkan tests skipped: "<<detail<<'\n';return 0;
        }
        probe.reset();
#endif
        srQuality();nativeGridPhaseEvidence();homogeneityAndSignedValues();phaseAndAxisControls();spatialNoiseDebias();for(unsigned mode=0;mode<3;++mode)conditionalNoise(mode);sparseColorBorders();runtimeTilingAndValidation();
#ifdef LATENT_JOINT_TEST_VULKAN
        std::cout<<"joint CPU/GPU max_scaled_signal_error="<<maxBackendError<<'\n';
#endif
        std::cout<<"joint RAW checks="<<checks<<" PASS\n";return 0;}
    catch(const std::exception& e){std::cerr<<"joint RAW: "<<e.what()<<'\n';return 1;}
}
