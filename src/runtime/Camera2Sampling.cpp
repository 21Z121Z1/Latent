#include "latent/runtime/Camera2Sampling.h"
#include <stdexcept>

namespace latent::runtime {
namespace {
using namespace imaging;
bool within(SensorRect a,SensorRect b) {
    return a.width && a.height && a.x>=b.x && a.y>=b.y &&
        static_cast<std::uint64_t>(a.x)+a.width<=static_cast<std::uint64_t>(b.x)+b.width &&
        static_cast<std::uint64_t>(a.y)+a.height<=static_cast<std::uint64_t>(b.y)+b.height;
}
SampleTransform toRegion(Extent e,SensorRect r) {
    const double sx=static_cast<double>(r.width)/e.width,sy=static_cast<double>(r.height)/e.height;
    return {sx,sy,r.x+(sx-1)*0.5,r.y+(sy-1)*0.5};
}
}
imaging::SensorSampling interpretCamera2Sampling(const Camera2SamplingEvidence& e) {
    using namespace imaging;
    if(static_cast<unsigned>(e.base)>3 || static_cast<unsigned>(e.requestedMode)>1 ||
        !e.rawExtent.width || !e.rawExtent.height) throw std::invalid_argument("unsupported Camera2 CFA or extent");
    if(e.actualMode && (*e.actualMode!=e.requestedMode || static_cast<unsigned>(*e.actualMode)>1))
        throw std::invalid_argument("requested/actual sensor pixel mode mismatch");
    if(!e.actualMode && (e.pixelModeRequestAvailable || e.requestedMode!=SensorPixelMode::Default))
        throw std::invalid_argument("missing actual sensor pixel mode");
    SensorSampling s{};s.buffer.base=e.base;s.pixelMode=e.requestedMode;
    s.pixelArray=e.pixelArray;s.active=e.active;s.preCorrectionActive=s.calibration=e.preCorrection;
    s.coordinateSpace=e.coordinateSpace;s.zoomRatio=e.zoomRatio;
    if(e.physicalGroup)s.physical=CfaTopology{e.base,e.physicalGroup->width,e.physicalGroup->height};
    const bool nonUhrMaximum=!e.ultraHighResolution && e.pixelModeRequestAvailable &&
        e.requestedMode==SensorPixelMode::MaximumResolution;
    if(nonUhrMaximum && e.rawGroupingUsed && !*e.rawGroupingUsed)
        throw std::invalid_argument("grouping result contradicts non-UHR maximum-resolution RAW contract");
    if(e.rawGroupingUsed.value_or(false) || nonUhrMaximum) {
        if(!e.physicalGroup)throw std::invalid_argument("grouped RAW without same-filter dimensions");
        s.buffer=*s.physical;s.remosaic=ProcessingState::NotApplied;
    } else if(!e.rawGroupingUsed && e.ultraHighResolution &&
        (e.physicalGroup || e.remosaicReprocessing)) {
        throw std::invalid_argument("RAW topology discriminator unavailable");
    } else if(!e.rawGroupingUsed && e.physicalGroup && !e.pixelModeRequestAvailable) {
        throw std::invalid_argument("physical CFA observation does not identify RAW topology");
    }
    s.representation=(s.buffer.groupX==1&&s.buffer.groupY==1)?RawRepresentation::Bayer:RawRepresentation::GroupedBayer;
    // false means regular buffer, NOT proof of a particular binning/remosaic history.
    const auto full=SensorRect{0,0,e.pixelArray.width,e.pixelArray.height};
    if(!within(e.preCorrection,full)||!within(e.active,e.preCorrection))
        throw std::invalid_argument("inconsistent mode-specific Camera2 arrays");
    if(e.croppedRawStream) {
        if(!e.rawCropRegion || !within(*e.rawCropRegion,e.preCorrection))
            throw std::invalid_argument("CROPPED_RAW requires actual RAW crop region");
        if(s.representation==RawRepresentation::GroupedBayer)
            throw std::invalid_argument("native grouped CROPPED_RAW CFA origin needs a calibrated device profile");
        if(e.rawExtent.width!=e.preCorrection.width || e.rawExtent.height!=e.preCorrection.height)
            throw std::invalid_argument("cropped RAW extent lacks an unambiguous CFA origin");
        // RAW crop reports FoV, not a buffer crop. Result LSC/intrinsics/hot pixel
        // coordinates cover the post-crop preCorrection array, per API 34.
        s.bufferToSensor=toRegion(e.rawExtent,*e.rawCropRegion);
        s.bufferToCalibration={1,1,static_cast<double>(e.preCorrection.x),static_cast<double>(e.preCorrection.y)};
        s.originX=e.preCorrection.x;s.originY=e.preCorrection.y;
    } else {
        if(e.rawCropRegion)throw std::invalid_argument("unexpected RAW crop result without CROPPED_RAW stream");
        if(e.rawExtent==e.pixelArray) { s.originX=0;s.originY=0; }
        else if(e.rawExtent.width==e.preCorrection.width&&e.rawExtent.height==e.preCorrection.height) {
            s.originX=e.preCorrection.x;s.originY=e.preCorrection.y;
            s.bufferToSensor=s.bufferToCalibration={1,1,static_cast<double>(s.originX),static_cast<double>(s.originY)};
        } else throw std::invalid_argument("RAW extent does not identify sensor sampling coordinates");
    }
    const auto valid=validateSampling(s,e.rawExtent);
    if(!valid.valid)throw std::invalid_argument(valid.message);
    return cropSampling(s,e.rawExtent,e.deliveredCrop);
}
}  // namespace latent::runtime
