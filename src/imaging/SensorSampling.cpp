#include "latent/imaging/SensorSampling.h"
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace latent::imaging {
namespace {
bool topology(const CfaTopology& t) {
    return static_cast<unsigned>(t.base) <= 3 && t.groupX > 0 && t.groupY > 0 &&
        t.groupX <= 1024 && t.groupY <= 1024;
}
bool rectInside(SensorRect r, Extent e) {
    return r.width > 0 && r.height > 0 && r.x < e.width && r.y < e.height &&
        r.width <= e.width-r.x && r.height <= e.height-r.y;
}
}
SamplingValidation validateSampling(const SensorSampling& s, Extent e) {
    if (!topology(s.buffer) || (s.physical && !topology(*s.physical))) return {false,"invalid CFA topology"};
    if (s.representation == RawRepresentation::Unknown || static_cast<unsigned>(s.representation) > 3)
        return {false,"unknown RAW representation"};
    const bool grouped = s.buffer.groupX != 1 || s.buffer.groupY != 1;
    if ((s.representation == RawRepresentation::GroupedBayer) != grouped)
        return {false,"RAW representation and buffer topology disagree"};
    if ((s.representation == RawRepresentation::RemosaicedBayer && s.remosaic != ProcessingState::Applied) ||
        (s.representation == RawRepresentation::GroupedBayer && s.remosaic == ProcessingState::Applied))
        return {false,"inconsistent remosaic evidence"};
    if (static_cast<unsigned>(s.pixelMode) > 1 || static_cast<unsigned>(s.binning) > 2 ||
        static_cast<unsigned>(s.remosaic) > 2) return {false,"invalid sensor mode state"};
    if (!e.width || !e.height || !rectInside(s.active,s.pixelArray) ||
        !rectInside(s.preCorrectionActive,s.pixelArray) || !rectInside(s.calibration,s.pixelArray))
        return {false,"invalid sensor/calibration arrays"};
    if (!std::isfinite(s.zoomRatio) || s.zoomRatio < 1 || s.coordinateSpace.empty())
        return {false,"unknown or invalid sensor coordinate space"};
    for (const auto& t:{s.bufferToSensor,s.bufferToCalibration}) {
        if (!std::isfinite(t.scaleX) || !std::isfinite(t.scaleY) || !std::isfinite(t.translateX) ||
            !std::isfinite(t.translateY) || t.scaleX <= 0 || t.scaleY <= 0)
            return {false,"unknown or invalid coordinate mapping"};
        if (t.translateX < 0 || t.translateY < 0 ||
            t.translateX + static_cast<double>(e.width-1)*t.scaleX > static_cast<double>(s.pixelArray.width-1) ||
            t.translateY + static_cast<double>(e.height-1)*t.scaleY > static_cast<double>(s.pixelArray.height-1))
            return {false,"RAW footprint outside sensor pixel array"};
    }
    if (s.originX > std::numeric_limits<std::uint32_t>::max()-(e.width-1) ||
        s.originY > std::numeric_limits<std::uint32_t>::max()-(e.height-1)) return {false,"CFA coordinate overflow"};
    return {};
}
CfaChannel samplingChannelAt(const SensorSampling& s, std::uint32_t x, std::uint32_t y) {
    if (!topology(s.buffer)) throw std::invalid_argument("invalid CFA topology");
    const auto cx=(static_cast<std::uint64_t>(s.originX)+x)/s.buffer.groupX;
    const auto cy=(static_cast<std::uint64_t>(s.originY)+y)/s.buffer.groupY;
    return cfaChannelAt(s.buffer.base,static_cast<std::uint32_t>(cx&1U),static_cast<std::uint32_t>(cy&1U));
}
SensorSampling cropSampling(const SensorSampling& s, Extent e, SensorRect r) {
    const auto v=validateSampling(s,e);
    if (!v.valid || !rectInside(r,e)) throw std::invalid_argument(v.valid?"invalid RAW crop":v.message);
    auto out=s;
    out.originX+=r.x; out.originY+=r.y;
    out.bufferToSensor.translateX+=static_cast<double>(r.x)*s.bufferToSensor.scaleX;
    out.bufferToSensor.translateY+=static_cast<double>(r.y)*s.bufferToSensor.scaleY;
    out.bufferToCalibration.translateX+=static_cast<double>(r.x)*s.bufferToCalibration.scaleX;
    out.bufferToCalibration.translateY+=static_cast<double>(r.y)*s.bufferToCalibration.scaleY;
    return out;
}
bool sameSamplingMode(const SensorSampling& a,const SensorSampling& b) { return a==b; }
SensorSampling regularSampling(Extent e,CfaPattern p) {
    SensorSampling s{};
    s.buffer.base=p; s.representation=RawRepresentation::Bayer;
    s.pixelArray=e; s.active=s.preCorrectionActive=s.calibration={0,0,e.width,e.height};
    s.coordinateSpace="explicit-regular-buffer";
    return s;
}
bool legacyBayerCompatible(const SensorSampling& s,CfaPattern p) {
    if (s.representation!=RawRepresentation::Bayer && s.representation!=RawRepresentation::RemosaicedBayer) return false;
    if (s.buffer.groupX!=1 || s.buffer.groupY!=1) return false;
    for (std::uint32_t y=0;y<2;++y) for (std::uint32_t x=0;x<2;++x)
        if (samplingChannelAt(s,x,y)!=cfaChannelAt(p,x,y)) return false;
    return true;
}
std::string samplingJson(const SensorSampling& s) {
    // quoted escapes are JSON-compatible for these restricted identifiers; reject
    // control chars rather than produce a non-JSON diagnostic bundle.
    if (s.coordinateSpace.find_first_of("\"\\\n\r\t")!=std::string::npos)
        throw std::invalid_argument("invalid coordinate-space identifier");
    for (char c:s.coordinateSpace) if (static_cast<unsigned char>(c)<32) throw std::invalid_argument("invalid identifier");
    std::ostringstream o; o << std::setprecision(17);
    o << "{\"version\":2,\"representation\":" << static_cast<unsigned>(s.representation)
      << ",\"base\":" << static_cast<unsigned>(s.buffer.base) << ",\"group\":[" << s.buffer.groupX << ',' << s.buffer.groupY
      << "],\"origin\":[" << s.originX << ',' << s.originY << "],\"pixel_mode\":" << static_cast<unsigned>(s.pixelMode)
      << ",\"binning\":" << static_cast<unsigned>(s.binning) << ",\"remosaic\":" << static_cast<unsigned>(s.remosaic)
      << ",\"coordinate_space\":\"" << s.coordinateSpace << "\",\"pixel_array\":[" << s.pixelArray.width << ',' << s.pixelArray.height << ']';
    const auto rect=[&o](const char* name,SensorRect r) { o << ",\"" << name << "\":[" << r.x << ',' << r.y << ',' << r.width << ',' << r.height << ']'; };
    rect("active",s.active);rect("pre_correction",s.preCorrectionActive);rect("calibration",s.calibration);
    const auto& f=s.bufferToSensor;
    o << ",\"buffer_to_sensor\":[" << f.scaleX << ',' << f.scaleY << ',' << f.translateX << ',' << f.translateY << ']';
    const auto& t=s.bufferToCalibration;
    o << ",\"buffer_to_calibration\":[" << t.scaleX << ',' << t.scaleY << ',' << t.translateX << ',' << t.translateY
      << "],\"zoom_ratio\":" << s.zoomRatio << ",\"physical\":";
    if (s.physical) o << '[' << static_cast<unsigned>(s.physical->base) << ',' << s.physical->groupX << ',' << s.physical->groupY << ']';
    else o << "null";
    o << '}';return o.str();
}
}  // namespace latent::imaging
