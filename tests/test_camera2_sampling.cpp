#include "latent/runtime/Camera2Sampling.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace latent;
namespace {
unsigned assertions=0;
void check(bool value){++assertions;if(!value)throw std::runtime_error("Camera2 sampling assertion failed");}
template<class F>void rejects(F&& f){bool caught=false;try{f();}catch(const std::invalid_argument&){caught=true;}check(caught);}
runtime::Camera2SamplingEvidence evidence() {
    runtime::Camera2SamplingEvidence e{};
    e.pixelArray={8032,6032};e.rawExtent=e.pixelArray;
    e.active=e.preCorrection={15,17,8000,6000};e.deliveredCrop=e.preCorrection;
    e.coordinateSpace="camera:0/mode:0";return e;
}
}
int main(){try{
    for(unsigned phase=0;phase<4;++phase)for(std::uint32_t gx=1;gx<=4;++gx)for(std::uint32_t gy=1;gy<=4;++gy) {
        auto e=evidence();e.base=static_cast<imaging::CfaPattern>(phase);
        e.ultraHighResolution=true;e.remosaicReprocessing=true;e.physicalGroup=imaging::Extent{gx,gy};
        e.pixelModeRequestAvailable=true;e.requestedMode=imaging::SensorPixelMode::MaximumResolution;e.actualMode=e.requestedMode;
        e.rawGroupingUsed=true;
        const auto s=runtime::interpretCamera2Sampling(e);
        check(s.buffer.groupX==gx&&s.buffer.groupY==gy&&s.originX==15&&s.originY==17);
        check(s.pixelMode==imaging::SensorPixelMode::MaximumResolution);
        check(s.bufferToCalibration.translateX==15&&s.bufferToSensor.translateY==17);
        for(std::uint32_t y=0;y<11;++y)for(std::uint32_t x=0;x<11;++x)
            check(imaging::samplingChannelAt(s,x,y)==imaging::cfaChannelAt(e.base,(x+15)/gx,(y+17)/gy));
        e.rawGroupingUsed=false;const auto regular=runtime::interpretCamera2Sampling(e);
        check(regular.buffer.groupX==1&&regular.representation==imaging::RawRepresentation::Bayer);
        check(regular.physical==s.physical&&regular.remosaic==imaging::ProcessingState::Unknown);
        e.rawGroupingUsed.reset();rejects([&]{(void)runtime::interpretCamera2Sampling(e);});
    }
    auto e=evidence();check(runtime::interpretCamera2Sampling(e).representation==imaging::RawRepresentation::Bayer);
    e.ultraHighResolution=true;check(runtime::interpretCamera2Sampling(e).representation==imaging::RawRepresentation::Bayer);
    e.rawGroupingUsed=true;rejects([&]{(void)runtime::interpretCamera2Sampling(e);});
    e=evidence();e.pixelModeRequestAvailable=true;e.requestedMode=imaging::SensorPixelMode::MaximumResolution;
    rejects([&]{(void)runtime::interpretCamera2Sampling(e);}); // missing actual mode
    e.actualMode=imaging::SensorPixelMode::Default;rejects([&]{(void)runtime::interpretCamera2Sampling(e);});
    e.actualMode=e.requestedMode;rejects([&]{(void)runtime::interpretCamera2Sampling(e);}); // non-UHR maximum, no discriminator
    e.physicalGroup=imaging::Extent{3,3};check(runtime::interpretCamera2Sampling(e).buffer.groupX==3);
    e.rawGroupingUsed=false;rejects([&]{(void)runtime::interpretCamera2Sampling(e);});
    e.requestedMode=imaging::SensorPixelMode::Default;e.actualMode=e.requestedMode;e.rawGroupingUsed.reset();
    check(runtime::interpretCamera2Sampling(e).buffer.groupX==1);
    e=evidence();e.rawExtent={8000,6000};e.deliveredCrop={0,0,8000,6000};
    auto s=runtime::interpretCamera2Sampling(e);check(s.originX==15&&s.originY==17);
    e.croppedRawStream=true;e.rawCropRegion=imaging::SensorRect{2015,1517,4000,3000};e.zoomRatio=2;
    s=runtime::interpretCamera2Sampling(e);
    check(s.bufferToSensor.scaleX==0.5&&s.bufferToSensor.translateX==2014.75);
    check(s.bufferToCalibration.scaleX==1&&s.bufferToCalibration.translateX==15);
    // Crop origin is FoV evidence, NOT a manual CFA phase patch.
    check(s.originX==15&&s.originY==17);
    auto other=e;other.rawCropRegion->x+=1;check(!imaging::sameSamplingMode(s,runtime::interpretCamera2Sampling(other)));
    e.rawCropRegion.reset();rejects([&]{(void)runtime::interpretCamera2Sampling(e);});
    e.rawCropRegion=imaging::SensorRect{2015,1517,4000,3000};e.physicalGroup=imaging::Extent{4,4};e.rawGroupingUsed=true;
    rejects([&]{(void)runtime::interpretCamera2Sampling(e);}); // native crop phase unknown
    e=evidence();e.rawExtent={7999,6000};rejects([&]{(void)runtime::interpretCamera2Sampling(e);});
    e=evidence();e.deliveredCrop.x=9000;rejects([&]{(void)runtime::interpretCamera2Sampling(e);});
    std::cout<<"Camera2 mode, crop/calibration, grouping, ambiguity: "<<assertions<<" assertions PASS\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
