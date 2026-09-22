#include "latent/vulkan/DirectReconstruction.h"
#include "latent/vulkan/ComputeRunner.h"
#include "latent/vulkan/VulkanRuntime.h"
#include "direct_reconstruction_spv.h"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace latent::vulkan {
namespace {
using namespace reference;
struct Push {
    std::array<std::uint32_t,4> counts{},tile{},geometry{};
    std::array<float,4> policy{},policy2{},policy3{};
};
static_assert(sizeof(Push)==96);
struct State {ReconstructionAnchor anchor;ReconstructionAccumulator accum;};
static_assert(sizeof(State)==sizeof(ReconstructionAnchor)+sizeof(ReconstructionAccumulator));
class Executor final : public DirectTileExecutor {
public:
    Executor(std::unique_ptr<ComputeRunner> runner,std::size_t sources,std::size_t outputs,
        DirectKernelPolicy policy,bool timing):runner_(std::move(runner)),maxSource_(sources),maxOutput_(outputs),policy_(policy) {
        validateDirectKernelPolicy(policy_);
        VkPhysicalDeviceProperties caps{};
        vkGetPhysicalDeviceProperties(VulkanRuntime::physicalDeviceHandle(),&caps);
        maxGroups_=caps.limits.maxComputeWorkGroupCount[0];
        if (!sources || !outputs || sources>UINT32_MAX || outputs>UINT32_MAX ||
            sources>caps.limits.maxStorageBufferRange/sizeof(MosaicEvidence) ||
            outputs>caps.limits.maxStorageBufferRange/sizeof(State))
            throw std::invalid_argument("direct GPU tile exceeds storage buffer limits");
        try {
            pipeline_=runner_->createComputePipeline({std::begin(direct_reconstruction_spv),std::end(direct_reconstruction_spv)},4,sizeof(Push));
            buffers_[0]=runner_->createStorageBuffer(sources*sizeof(MosaicEvidence));
            buffers_[1]=runner_->createStorageBuffer(outputs*sizeof(ReconstructionWarp));
            buffers_[2]=runner_->createStorageBuffer(outputs*sizeof(State));
            buffers_[3]=runner_->createStorageBuffer(outputs*sizeof(ReconstructedPixel));
            for(std::size_t i=0;i<4;++i) {
                bindings_[i]={buffers_[i].handle,0,VK_WHOLE_SIZE};
                bytes_+=buffers_[i].allocationSize;
            }
            // Establish C++ lifetimes in coherent mapped storage before exposing
            // typed writable spans; GPU-only state is never dereferenced on host.
            std::uninitialized_value_construct_n(static_cast<MosaicEvidence*>(buffers_[0].mapped),sources);
            std::uninitialized_value_construct_n(static_cast<ReconstructionWarp*>(buffers_[1].mapped),outputs);
            std::uninitialized_value_construct_n(static_cast<ReconstructedPixel*>(buffers_[3].mapped),outputs);
            if(timing) (void)runner_->enableTimestamps();
        } catch(...) {release();throw;}
    }
    ~Executor() override {release();}
    void begin(std::span<const MosaicEvidence> s,std::span<const ReconstructionWarp> w,const DirectKernelGeometry& g) override {
        if(count_ || w.empty() || !g.referenceFrame) throw std::invalid_argument("GPU tile begin requires reference");
        run(s,w,g,0);count_=w.size();
    }
    void add(std::span<const MosaicEvidence> s,std::span<const ReconstructionWarp> w,const DirectKernelGeometry& g) override {
        if(g.referenceFrame || w.size()!=count_ || !count_) throw std::invalid_argument("invalid GPU tile contribution");
        run(s,w,g,1);
    }
    void finish(std::span<ReconstructedPixel> out) override {
        if(!count_ || out.size()!=count_) throw std::invalid_argument("GPU tile output size mismatch");
        Push push{};push.counts={static_cast<std::uint32_t>(count_),0,2,0};
        push.policy2={policy_.minimumConfidence,0,policy_.quadraticStrength,policy_.fitRegularization};push.policy3={policy_.maximumFitLeverage,0,0,0};dispatch(push);
        if(out.data()!=buffers_[3].mapped) {
            runner_->download(buffers_[3],out.data(),out.size_bytes());transfers_+=out.size_bytes();
        }
        count_=0;
    }
    std::span<MosaicEvidence> inputArena() override {return {static_cast<MosaicEvidence*>(buffers_[0].mapped),maxSource_};}
    std::span<ReconstructionWarp> warpArena() override {return {static_cast<ReconstructionWarp*>(buffers_[1].mapped),maxOutput_};}
    std::span<ReconstructedPixel> outputArena() override {return {static_cast<ReconstructedPixel*>(buffers_[3].mapped),maxOutput_};}
    std::uint64_t deviceBytes() const override {return bytes_;}
    std::uint64_t transferBytes() const override {return transfers_;}
    double gpuMilliseconds() const override {return runner_->gpuMilliseconds();}
private:
    void run(std::span<const MosaicEvidence> s,std::span<const ReconstructionWarp> w,const DirectKernelGeometry& g,std::uint32_t phase) {
        if(s.size()>maxSource_ || w.empty() || w.size()>maxOutput_) throw std::invalid_argument("GPU tile exceeds admitted arena");
        validateDirectKernelInput(s,w,g);
        if(s.data()!=buffers_[0].mapped) {runner_->upload(buffers_[0],s.data(),s.size_bytes());transfers_+=s.size_bytes();}
        if(w.data()!=buffers_[1].mapped) {runner_->upload(buffers_[1],w.data(),w.size_bytes());transfers_+=w.size_bytes();}
        Push push{{static_cast<std::uint32_t>(w.size()),0,phase,0},
            {g.sourceTile.x,g.sourceTile.y,g.sourceTile.width,g.sourceTile.height},
            {g.radiusX,g.radiusY,g.correlatedSpatialNoise?1U:0U,0},
            {policy_.detailSigma,policy_.residualCutoff,policy_.aliasAllowance,policy_.relativeVarianceFloor},
            {policy_.minimumConfidence,g.fallbackSigma,policy_.quadraticStrength,policy_.fitRegularization},
            {policy_.maximumFitLeverage,0,0,0}};
        dispatch(push);
    }
    void dispatch(Push& p) {
        while(p.counts[1]<p.counts[0]) {
            const auto remaining=p.counts[0]-p.counts[1];
            const auto groups=std::min(maxGroups_,(remaining+63U)/64U);
            runner_->dispatch(pipeline_,bindings_,&p,sizeof(p),groups);
            p.counts[1]+=std::min(remaining,groups*64U);
        }
    }
    void release() noexcept {
        if(!runner_) return;
        if(pipeline_) runner_->destroyPipeline(pipeline_);
        pipeline_=VK_NULL_HANDLE;
        for(auto& b:buffers_) runner_->destroyBuffer(b);
    }
    std::unique_ptr<ComputeRunner> runner_;
    std::size_t maxSource_,maxOutput_,count_=0;
    DirectKernelPolicy policy_;
    std::array<ComputeRunner::Buffer,4> buffers_{};
    std::array<VkDescriptorBufferInfo,4> bindings_{};
    VkPipeline pipeline_=VK_NULL_HANDLE;
    std::uint32_t maxGroups_=0;
    std::uint64_t bytes_=0,transfers_=0;
};
}
std::unique_ptr<reference::DirectTileExecutor> makeVulkanDirectExecutor(std::size_t sources,std::size_t outputs,
    const reference::DirectKernelPolicy& policy,std::string* detail,bool timing) {
    reference::validateDirectKernelPolicy(policy);
    auto runner=ComputeRunner::tryCreate(detail);
    if(!runner) return {};
    try {return std::make_unique<Executor>(std::move(runner),sources,outputs,policy,timing);}
    catch(const std::exception& e) {if(detail) *detail=e.what();return {};}
}
}
