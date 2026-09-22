#pragma once
#include <jni.h>
#include "latent/imaging/ColorScience.h"
#include "latent/runtime/CfaTransport.h"
#include "latent/runtime/Camera2Sampling.h"
#include "latent/imaging/RawBurst.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace latent::android_transport {
struct PendingJavaException {};
inline void checkJava(JNIEnv* e) { if (e->ExceptionCheck()) throw PendingJavaException{}; }

class LocalRef {
public:
    LocalRef(JNIEnv* e, jobject value) : env_(e), value_(value) {}
    ~LocalRef() { if (value_) env_->DeleteLocalRef(value_); }
    LocalRef(const LocalRef&) = delete;
    LocalRef& operator=(const LocalRef&) = delete;
    [[nodiscard]] jobject get() const { return value_; }
private:
    JNIEnv* env_;
    jobject value_;
};

class Fields {
public:
    Fields(JNIEnv* e, jobject object) : e_(e), object_(object), type_(e, e->GetObjectClass(object)) {
        checkJava(e_);
        if (!object_) throw std::invalid_argument("null native input");
    }
    [[nodiscard]] jfieldID id(const char* name, const char* signature) const {
        const auto field = e_->GetFieldID(static_cast<jclass>(type_.get()), name, signature);
        checkJava(e_); return field;
    }
    [[nodiscard]] jlong integer64(const char* n) const { return e_->GetLongField(object_, id(n, "J")); }
    [[nodiscard]] jint integer(const char* n) const { return e_->GetIntField(object_, id(n, "I")); }
    [[nodiscard]] float real(const char* n) const { return e_->GetFloatField(object_, id(n, "F")); }
    [[nodiscard]] bool boolean(const char* n) const { return e_->GetBooleanField(object_, id(n, "Z")) == JNI_TRUE; }
    [[nodiscard]] jobject object(const char* n, const char* signature) const {
        const auto value = e_->GetObjectField(object_, id(n, signature)); checkJava(e_); return value;
    }
    [[nodiscard]] std::string text(const char* n) const {
        LocalRef value(e_, object(n, "Ljava/lang/String;"));
        if (!value.get()) throw std::invalid_argument("missing text metadata");
        const auto s = static_cast<jstring>(value.get());
        const char* chars = e_->GetStringUTFChars(s, nullptr); checkJava(e_);
        if (!chars) throw std::bad_alloc{};
        try {
            std::string result(chars); e_->ReleaseStringUTFChars(s, chars); return result;
        } catch (...) { e_->ReleaseStringUTFChars(s, chars); throw; }
    }
    [[nodiscard]] std::vector<float> floats(const char* n, jsize maximum = 65536) const {
        LocalRef value(e_, object(n, "[F"));
        if (!value.get()) throw std::invalid_argument("null metadata array");
        const auto a = static_cast<jfloatArray>(value.get());
        const auto count = e_->GetArrayLength(a);
        if (count < 0 || count > maximum) throw std::invalid_argument("metadata array exceeds safety bound");
        std::vector<float> result(static_cast<std::size_t>(count));
        if (count != 0) e_->GetFloatArrayRegion(a, 0, count, result.data());
        checkJava(e_);
        for (float v : result) if (!std::isfinite(v)) throw std::invalid_argument("non-finite metadata");
        return result;
    }
    [[nodiscard]] std::vector<jint> integers(const char* n, jsize maximum = 65536) const {
        LocalRef value(e_, object(n, "[I"));
        if (!value.get()) throw std::invalid_argument("null integer metadata");
        const auto array=static_cast<jintArray>(value.get());
        const auto count=e_->GetArrayLength(array);
        if (count<0 || count>maximum) throw std::invalid_argument("integer metadata exceeds safety bound");
        std::vector<jint> result(static_cast<std::size_t>(count));
        if(count)e_->GetIntArrayRegion(array,0,count,result.data());
        checkJava(e_);return result;
    }
    template<std::size_t N> [[nodiscard]] std::array<float, N> fixed(const char* n) const {
        const auto input = floats(n, static_cast<jsize>(N));
        if (input.size() != N) throw std::invalid_argument("metadata array length mismatch");
        std::array<float, N> output{}; std::copy(input.begin(), input.end(), output.begin()); return output;
    }
private:
    JNIEnv* e_;
    jobject object_;
    LocalRef type_;
};

inline std::uint32_t positiveInt(jint value) {
    if (value <= 0) throw std::invalid_argument("non-positive dimension or capture limit");
    return static_cast<std::uint32_t>(value);
}
inline std::uint64_t positiveLong(jlong value) {
    if (value <= 0) throw std::invalid_argument("non-positive identity or byte budget");
    return static_cast<std::uint64_t>(value);
}
template<class T> imaging::MetadataValue<T> observed(T value, imaging::MetadataSource source) {
    return {std::move(value), source, imaging::MetadataValidity::Valid, 1.0F};
}


struct ParsedRawInput {
    imaging::RawBurstMember member;
    std::array<float,4> whiteBalance{};
    imaging::Matrix3f color;
};
inline ParsedRawInput parseRawMetadata(const Fields& f,bool canonical) {
    ParsedRawInput parsed{};auto& member=parsed.member;
    const bool synthetic=f.boolean("synthetic");
        member.id = imaging::FrameId{positiveLong(f.integer64("id"))};
        auto& m = member.observations;
        m.sensorTimestampNs = f.integer64("timestampNs"); m.exposureTimeNs = f.integer64("exposureNs");
        m.sensitivityIso = f.real("iso"); m.exposureCalibration.nominalIso = m.sensitivityIso;
        m.cameraId = f.text("cameraId"); m.sensorMode = f.text("sensorMode");
        const auto cfa = f.integer("cfa");
        if (cfa < 0 || cfa > 3) throw std::invalid_argument("unsupported CFA layout");
        const auto phaseX = canonical ? 0 : f.integer("phaseX"), phaseY = canonical ? 0 : f.integer("phaseY");
        if (phaseX < 0 || phaseX > 1 || phaseY < 0 || phaseY > 1) throw std::invalid_argument("invalid CFA crop phase");
        const auto mapping = runtime::cfaTransportMap(static_cast<imaging::CfaPattern>(cfa),
            static_cast<std::uint32_t>(phaseX), static_cast<std::uint32_t>(phaseY));
        m.cfa = mapping.pattern;
        using Source = imaging::MetadataSource;
        const auto inputBlack = f.fixed<4>("black");
        imaging::BlackLevel black{};
        for (std::size_t c = 0; c < 4; ++c) black.cfa[c] = inputBlack[mapping.layoutIndex[c]];
        m.staticBlack = observed(black, Source::StaticCharacteristic);
        m.staticWhite = observed(f.real("white"), Source::StaticCharacteristic);
        const auto dynamic = f.floats("dynamicBlack", 4);
        if (!dynamic.empty()) {
            if (dynamic.size() != 4) throw std::invalid_argument("dynamic black length mismatch");
            imaging::BlackLevel level{};
            for (std::size_t c = 0; c < 4; ++c) level.cfa[c] = dynamic[mapping.layoutIndex[c]];
            m.dynamicBlack = observed(level, Source::DynamicCaptureResult);
        }
        const auto dynamicWhite = f.real("dynamicWhite");
        if (!std::isfinite(dynamicWhite) || dynamicWhite < 0) throw std::invalid_argument("invalid dynamic white");
        if (dynamicWhite > 0) m.dynamicWhite = observed(dynamicWhite, Source::DynamicCaptureResult);
        const auto noise = f.floats("noise", 8);
        if (!noise.empty()) {
            if (noise.size() != 8) throw std::invalid_argument("noise profile length mismatch");
            imaging::NoiseModel model{}; model.coordinate = imaging::NoiseCoordinate::NormalizedBlackSubtracted;
            for (std::size_t c = 0; c < 4; ++c) {
                const auto inputChannel = mapping.layoutIndex[c];
                model.shot[c] = noise[2U*inputChannel]; model.read[c] = noise[2U*inputChannel+1U];
            }
            m.noiseProfile = observed(model, synthetic ? Source::DeviceProfile : Source::DynamicCaptureResult);
        }
        auto shading = f.floats("shading");
        if (f.boolean("lensShadingAlreadyApplied")) {
            // Calibration was already applied upstream; applying these gains twice is wrong.
        } else if (!shading.empty()) {
            if (shading.size() % 4U != 0U) throw std::invalid_argument("lens shading channel count mismatch");
            for (std::size_t n = 0; n < shading.size(); n += 4U) {
                const std::array<float,4> inputGains{shading[n], shading[n+1U], shading[n+2U], shading[n+3U]};
                for (std::size_t c = 0; c < 4; ++c) shading[n+c] = inputGains[mapping.sensorChannel[c]];
            }
            imaging::LensShadingMap map{positiveInt(f.integer("shadingColumns")), positiveInt(f.integer("shadingRows")), std::move(shading)};
            m.lensShading = observed(std::move(map), Source::DynamicCaptureResult);
        } else if (f.integer("shadingColumns") != 0 || f.integer("shadingRows") != 0) {
            throw std::invalid_argument("missing lens-shading payload");
        }
        const auto inputWb = f.fixed<4>("whiteBalance");
        std::array<float,4> gains{};
        for (std::size_t c = 0; c < 4; ++c) gains[c] = inputWb[mapping.sensorChannel[c]];
        parsed.whiteBalance=gains;
        m.colorCorrectionGains=observed(gains,Source::DynamicCaptureResult);
        for (float gain : parsed.whiteBalance) if (gain <= 0) throw std::invalid_argument("invalid white balance gain");
        parsed.color=imaging::Matrix3f{f.fixed<9>("sensorToLinearSrgb")};
        if (!imaging::inverted(parsed.color)) throw std::invalid_argument("singular sensor color transform");
        if (synthetic) { m.exposureCalibration.source = Source::DeviceProfile; m.exposureCalibration.gainUncertainty = 0; }
    return parsed;
}
inline imaging::SensorRect sensorRect(const Fields& f,const char* name) {
    const auto v=f.integers(name,4);
    if(v.size()!=4 || v[0]<0 || v[1]<0)throw std::invalid_argument("invalid sensor rectangle");
    return {static_cast<std::uint32_t>(v[0]),static_cast<std::uint32_t>(v[1]),positiveInt(v[2]),positiveInt(v[3])};
}
inline imaging::SensorSampling sensorMode(JNIEnv* env,jobject object) {
    const Fields f(env,object);runtime::Camera2SamplingEvidence e{};
    const auto base=f.integer("cfa"),requested=f.integer("requestedMode"),actual=f.integer("actualMode");
    if(base<0||base>3||requested<0||requested>1||actual < -1||actual>1)
        throw std::invalid_argument("unsupported Camera2 CFA or pixel mode");
    e.base=static_cast<imaging::CfaPattern>(base);
    const auto gx=f.integer("groupWidth"),gy=f.integer("groupHeight"),used=f.integer("groupingUsed");
    if(gx || gy)e.physicalGroup=imaging::Extent{positiveInt(gx),positiveInt(gy)};
    if(used < -1 || used>1)throw std::invalid_argument("invalid grouping observation");
    if(used>=0)e.rawGroupingUsed=used!=0;
    e.ultraHighResolution=f.boolean("ultraHighResolution");e.remosaicReprocessing=f.boolean("remosaicReprocessing");
    e.pixelModeRequestAvailable=f.boolean("pixelModeAvailable");
    e.requestedMode=static_cast<imaging::SensorPixelMode>(requested);
    if(actual>=0)e.actualMode=static_cast<imaging::SensorPixelMode>(actual);
    e.pixelArray={positiveInt(f.integer("pixelWidth")),positiveInt(f.integer("pixelHeight"))};
    e.rawExtent={positiveInt(f.integer("rawWidth")),positiveInt(f.integer("rawHeight"))};
    e.active=sensorRect(f,"active");e.preCorrection=sensorRect(f,"preCorrection");e.deliveredCrop=sensorRect(f,"deliveredCrop");
    e.croppedRawStream=f.boolean("croppedRaw");
    if(!f.integers("rawCrop",4).empty())e.rawCropRegion=sensorRect(f,"rawCrop");
    e.zoomRatio=f.real("zoom");e.coordinateSpace=f.text("coordinateSpace");
    return runtime::interpretCamera2Sampling(e);
}
} // namespace latent::android_transport
