#include "latent/runtime/CapturePlan.h"
#include "TemporalFixture.h"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace latent;
namespace {
void check(bool v, const char* why) { if (!v) throw std::runtime_error(why); }
template<class F> void rejects(F f) { try { f(); } catch(const std::invalid_argument&) { return; } throw std::runtime_error("invalid plan accepted"); }
}
int main() {
    try {
        runtime::CaptureObservations o{20000000,33333333,400,true,0.0001F,{},false};
        runtime::CaptureCapabilities c{true,true,true,100000,200000000,33333333,1000000000,50,3200,128000000,8000000,12};
        runtime::CaptureIntent i{};
        const auto p = runtime::compileCapturePlan(o,i,c);
        check(p.frames.size() > 1 && p.frames.size() <= i.maximumFrames,"noise-driven burst count");
        for (const auto& f : p.frames) check(f.exposureTimeNs == p.frames[0].exposureTimeNs && f.sensitivityIso == 400,"constant exposure without fake gain");
        check(p.frames[0].exposureTimeNs < o.exposureTimeNs,"highlight protection");
        check(!p.noiseEstimated,"noise observation provenance");
        o.angularSpeedRadiansPerSecond=0.5F; o.gyroTimestampComparable=false;
        check(runtime::compileCapturePlan(o,i,c).frames[0].exposureTimeNs==p.frames[0].exposureTimeNs,"unknown sensor timebase must not imply gyro synchronization");
        o.gyroTimestampComparable=true;
        const auto moving=runtime::compileCapturePlan(o,i,c);
        check(moving.motionConstraintUsed && moving.frames[0].exposureTimeNs < p.frames[0].exposureTimeNs,"motion caps integration");
        c.manualSensor=false;
        const auto locked=runtime::compileCapturePlan(o,i,c);
        check(locked.control==runtime::ExposureControl::LockedAutoExposure && locked.frames[0].exposureTimeNs==o.exposureTimeNs,"AE-lock cannot request a fictitious manual exposure");
        c.retainedRawBudgetBytes=c.bytesPerRawFrame;
        check(runtime::compileCapturePlan(o,i,c).frames.size()==1,"retained-frame budget");
        o.normalizedMidtoneVariance.reset();
        check(runtime::compileCapturePlan(o,i,c).noiseEstimated,"prior is not measurement");
        auto bad=c; bad.raw=false; rejects([&]{(void)runtime::compileCapturePlan(o,i,bad);});
        bad=c;bad.retainedRawBudgetBytes=0; rejects([&]{(void)runtime::compileCapturePlan(o,i,bad);});
        o.aeConverged=false; rejects([&]{(void)runtime::compileCapturePlan(o,i,c);});
        o.aeConverged=true;i.targetStandardDeviation=std::numeric_limits<float>::quiet_NaN();
        rejects([&]{(void)runtime::compileCapturePlan(o,i,c);});
        std::vector<imaging::RawFrame> raw{test::frame(1),test::frame(2)};
        const auto burst=imaging::describeRawBurst(imaging::BurstId{1},imaging::CaptureSequenceId{2},imaging::CalibrationId{3},raw);
        const auto binding=runtime::bindReferenceFrames(raw);
        runtime::TemporalExecutionPolicy execution{};execution.preferVulkan=false;
        std::size_t events=0;
        runtime::TemporalExecutionControl control{[&](const runtime::TemporalProgress& progress) {
            ++events; check(progress.totalFrames==2,"progress membership"); return progress.completedFrames==0;
        }};
        bool cancelled=false;
        try { (void)runtime::reconstructRawBurst(burst,binding,{}, {},execution,control); }
        catch(const runtime::TemporalCancelled&) { cancelled=true; }
        check(cancelled && events>2,"cancel between source lifetimes");
        const auto resumed=runtime::reconstructRawBurst(burst,binding,{}, {},execution);
        check(resumed.scene.lineage->inputs.size()==2,"cancel does not poison a subsequent run");
        const auto compiled=runtime::compileTemporalPlan(burst,{}, {},execution,{});
        check(compiled.workingSetBound()==runtime::temporalWorkingSetBound(burst.extent,burst.members.size(),{},runtime::TemporalBackend::Reference),"one admission formula");
        rejects([&]{(void)runtime::temporalWorkingSetBound({2,2},0,{},runtime::TemporalBackend::Reference);});
        rejects([&]{(void)runtime::temporalWorkingSetBound({0xffffffffU,0xffffffffU},64,{},runtime::TemporalBackend::Vulkan);});
        std::cout << "capture policy, authority and execution cancellation tests passed\n";
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
