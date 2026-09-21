#include <jni.h>
#include <android/bitmap.h>

#include "latent/imaging/ColorScience.h"
#include "latent/runtime/CapturePlan.h"
#include "latent/runtime/CfaTransport.h"
#include "latent/runtime/TemporalPipeline.h"
#include "latent/render/ReferenceRenderer.h"
#include "latent/vulkan/TemporalFusion.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <locale>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace latent;

struct PendingJavaException {};
void checkJava(JNIEnv* e) { if (e->ExceptionCheck()) throw PendingJavaException{}; }

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

std::uint32_t positiveInt(jint value) {
    if (value <= 0) throw std::invalid_argument("non-positive dimension or capture limit");
    return static_cast<std::uint32_t>(value);
}
std::uint64_t positiveLong(jlong value) {
    if (value <= 0) throw std::invalid_argument("non-positive identity or byte budget");
    return static_cast<std::uint64_t>(value);
}
template<class T> imaging::MetadataValue<T> observed(T value, imaging::MetadataSource source) {
    return {std::move(value), source, imaging::MetadataValidity::Valid, 1.0F};
}

std::string escape(const std::string& text) {
    std::string output;
    for (char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '"' || c == '\\') { output += '\\'; output += static_cast<char>(c); }
        else if (c >= 32U) output += static_cast<char>(c);
    }
    return output;
}

class BitmapPixels {
public:
    BitmapPixels(JNIEnv* e, jobject bitmap, const imaging::Extent extent) : env_(e), bitmap_(bitmap) {
        if (AndroidBitmap_getInfo(e, bitmap, &info_) != ANDROID_BITMAP_RESULT_SUCCESS ||
            info_.format != ANDROID_BITMAP_FORMAT_RGBA_8888 || info_.width != extent.width || info_.height != extent.height)
            throw std::invalid_argument("output must be an extent-matched mutable RGBA8888 bitmap");
        if (AndroidBitmap_lockPixels(e, bitmap, &pixels_) != ANDROID_BITMAP_RESULT_SUCCESS)
            throw std::runtime_error("cannot lock output bitmap");
    }
    ~BitmapPixels() { if (pixels_) AndroidBitmap_unlockPixels(env_, bitmap_); }
    BitmapPixels(const BitmapPixels&) = delete;
    BitmapPixels& operator=(const BitmapPixels&) = delete;
    void write(const imaging::RenderedFrame& rendered) {
        for (std::uint32_t y = 0; y < info_.height; ++y) {
            auto* row = static_cast<std::uint8_t*>(pixels_) + static_cast<std::size_t>(y) * info_.stride;
            for (std::uint32_t x = 0; x < info_.width; ++x) {
                const auto offset = (static_cast<std::size_t>(y) * info_.width + x) * 3U;
                for (std::size_t c = 0; c < 3U; ++c) {
                    const float value = rendered.image.rgb[offset + c];
                    if (!std::isfinite(value)) throw std::runtime_error("non-finite display output");
                    row[static_cast<std::size_t>(x) * 4U + c] = static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
                }
                row[static_cast<std::size_t>(x) * 4U + 3U] = 255U;
            }
        }
    }
private:
    JNIEnv* env_;
    jobject bitmap_;
    AndroidBitmapInfo info_{};
    void* pixels_ = nullptr;
};

imaging::Matrix3f linearSrgbToScene() {
    const auto toXyz = imaging::rgbPrimariesToXyzMatrix({0.64F,0.33F}, {0.30F,0.60F}, {0.15F,0.06F}, imaging::kIlluminantD65);
    const auto ap1 = imaging::rgbPrimariesToXyzMatrix(imaging::kAp1Red, imaging::kAp1Green, imaging::kAp1Blue, imaging::kAcesWhite);
    return imaging::inverted(ap1).value().multiplied(imaging::bradfordAdaptation(imaging::kIlluminantD65, imaging::kAcesWhite)).multiplied(toXyz);
}

jstring process(JNIEnv* e, jobjectArray inputs, jobject bitmap, jboolean vk, jlong memory,
                jfloat renderEv, jobject progress) {
    if (!inputs || !bitmap || !progress) throw std::invalid_argument("missing processing input");
    const auto count = e->GetArrayLength(inputs);
    if (count <= 0 || count > 64) throw std::invalid_argument("RAW membership outside admission limit");
    if (!std::isfinite(renderEv) || std::abs(renderEv) > 16.0F) throw std::invalid_argument("invalid render exposure");
    static std::atomic<std::uint64_t> nextBurst{1};
    const auto identity = nextBurst.fetch_add(1);
    imaging::RawBurst burst{};
    burst.id = imaging::BurstId{identity}; burst.sequence = imaging::CaptureSequenceId{identity};
    burst.calibration = imaging::CalibrationId{identity};
    std::vector<runtime::HostRawBinding> host;
    std::vector<std::array<float, 4>> wb;
    std::vector<imaging::Matrix3f> color;
    bool synthetic = false;
    for (jsize index = 0; index < count; ++index) {
        LocalRef input(e, e->GetObjectArrayElement(inputs, index)); checkJava(e);
        if (!input.get()) throw std::invalid_argument("null RAW member");
        const Fields f(e, input.get());
        const auto width = positiveInt(f.integer("width")), height = positiveInt(f.integer("height"));
        const auto stride = positiveInt(f.integer("rowStrideBytes"));
        if (width > 65535U || height > 65535U || stride % 2U != 0U || stride / 2U < width)
            throw std::invalid_argument("unsupported RAW plane dimensions or alignment");
        if (index == 0) { burst.extent = {width, height}; synthetic = f.boolean("synthetic"); }
        if (width != burst.extent.width || height != burst.extent.height || synthetic != f.boolean("synthetic"))
            throw std::invalid_argument("inconsistent burst extent or source provenance");
        imaging::RawBurstMember member{};
        member.id = imaging::FrameId{positiveLong(f.integer64("id"))};
        auto& m = member.observations;
        m.sensorTimestampNs = f.integer64("timestampNs"); m.exposureTimeNs = f.integer64("exposureNs");
        m.sensitivityIso = f.real("iso"); m.exposureCalibration.nominalIso = m.sensitivityIso;
        m.cameraId = f.text("cameraId"); m.sensorMode = f.text("sensorMode");
        const auto cfa = f.integer("cfa");
        if (cfa < 0 || cfa > 3) throw std::invalid_argument("unsupported CFA layout");
        const auto phaseX = f.integer("phaseX"), phaseY = f.integer("phaseY");
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
        if (!shading.empty()) {
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
        wb.push_back(gains);
        for (float gain : wb.back()) if (gain <= 0) throw std::invalid_argument("invalid white balance gain");
        color.push_back(imaging::Matrix3f{f.fixed<9>("sensorToLinearSrgb")});
        if (!imaging::inverted(color.back())) throw std::invalid_argument("singular sensor color transform");
        if (synthetic) { m.exposureCalibration.source = Source::DeviceProfile; m.exposureCalibration.gainUncertainty = 0; }
        LocalRef buffer(e, f.object("pixels", "Ljava/nio/ByteBuffer;"));
        if (!buffer.get()) throw std::invalid_argument("missing direct RAW buffer");
        const auto bytes = e->GetDirectBufferCapacity(buffer.get());
        const auto* pixels = static_cast<const std::uint16_t*>(e->GetDirectBufferAddress(buffer.get()));
        checkJava(e);
        if (!pixels || bytes <= 0 || reinterpret_cast<std::uintptr_t>(pixels) % alignof(std::uint16_t) != 0U)
            throw std::invalid_argument("RAW requires an aligned direct byte buffer");
        const auto required = static_cast<std::uint64_t>(height - 1U) * stride + static_cast<std::uint64_t>(width) * 2U;
        if (static_cast<std::uint64_t>(bytes) < required) throw std::invalid_argument("RAW buffer is truncated");
        host.push_back({member.id, {burst.extent, stride / 2U,
            {pixels, static_cast<std::size_t>(bytes) / sizeof(std::uint16_t)}}});
        burst.members.push_back(std::move(member));
    }
    runtime::HostRawBindings bindings(std::move(host));
    runtime::TemporalExecutionPolicy execution{};
    execution.preferVulkan = vk == JNI_TRUE; execution.memoryBudgetBytes = positiveLong(memory);
    reference::TemporalPolicy policy{};
    runtime::TemporalRequest request{};
    // Admission precedes selector scratch allocations. Use the same capability
    // predicate as production execution; do not reject a legal CPU fallback.
    const bool canFuseVk = execution.preferVulkan && vulkan::temporalFusionAvailable(burst.extent);
    (void)runtime::compileTemporalPlan(burst, request, policy, execution, {canFuseVk});
    LocalRef callbackType(e, e->GetObjectClass(progress)); checkJava(e);
    const auto callback = e->GetMethodID(static_cast<jclass>(callbackType.get()), "onProgress", "(III)Z"); checkJava(e);
    const runtime::TemporalExecutionControl control{[&](const runtime::TemporalProgress& p) {
        const bool keep = e->CallBooleanMethod(progress, callback, static_cast<jint>(p.stage),
            static_cast<jint>(p.completedFrames), static_cast<jint>(p.totalFrames)) == JNI_TRUE;
        checkJava(e); return keep;
    }};
    if (!control.continueExecution({graph::TemporalOperation::SelectReference, 0U, burst.members.size()}))
        throw runtime::TemporalCancelled{};
    // Reference metadata defines scene color. Selection uses the canonical oracle;
    // its metrics are retained instead of being relabelled as a user override.
    const auto selection = reference::selectBurstReference(burst, bindings, policy);
    const auto ref = std::find_if(burst.members.begin(), burst.members.end(), [&](const auto& m) { return m.id == selection.frame; });
    const auto refIndex = static_cast<std::size_t>(ref - burst.members.begin());
    request.reference = selection.frame;
    request.reconstruction.applyLensShading = true;
    request.reconstruction.whiteBalanceGains = wb[refIndex];
    request.reconstruction.cameraToAcescg = linearSrgbToScene().multiplied(color[refIndex]);
    request.reconstruction.whiteBalanceConfidence = synthetic ? 1.0F : 0.75F;
    auto result = runtime::reconstructRawBurst(burst, bindings, request, policy, execution, control);
    result.trace.selection = selection;
    if (!control.continueExecution({graph::TemporalOperation::ReconstructScene, burst.members.size(), burst.members.size()}))
        throw runtime::TemporalCancelled{};
    const auto rendered = render::renderReference(result.scene, render::makeSdrRenderConfig(renderEv));
    BitmapPixels output(e, bitmap, burst.extent); output.write(rendered);
    const auto& uncertainty = result.fused.uncertainty;
    const auto average = [](const auto& values) { return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size()); };
    std::ostringstream json; json.imbue(std::locale::classic());
    json << "{\"schemaVersion\":1,\"source\":\"" << (synthetic ? "synthetic-fixture" : "Camera2-RAW")
         << "\",\"burstId\":" << identity << ",\"referenceId\":" << selection.frame.value
         << ",\"frameCount\":" << count << ",\"backend\":\"" << (result.trace.fusionBackend == runtime::TemporalBackend::Vulkan ? "Vulkan-fusion/CPU-alignment" : "FP32-reference")
         << "\",\"fallback\":" << static_cast<unsigned>(result.trace.fallback)
         << ",\"colorPath\":\"" << (synthetic ? "fixture-linear-sRGB" : "Camera2-dynamic-linear-sRGB-estimate")
         << "\",\"effectiveN\":" << average(uncertainty.effectiveSampleCount)
         << ",\"alignmentConfidence\":" << average(uncertainty.alignmentConfidence)
         << ",\"robustnessConfidence\":" << average(uncertainty.robustnessConfidence)
         << ",\"conditionalVariance\":" << average(uncertainty.marginalVariance)
         << ",\"processingMs\":" << result.trace.processingMilliseconds
         << ",\"workingSetBoundBytes\":" << result.trace.workingSetBoundBytes
         << ",\"exposureNs\":" << ref->observations.exposureTimeNs << ",\"iso\":" << ref->observations.sensitivityIso
         << ",\"frames\":[";
    for (std::size_t n = 0; n < result.trace.frames.size(); ++n) {
        if (n) json << ',';
        const auto& frame = result.trace.frames[n]; const auto& contribution = result.scene.lineage->contributions[n];
        json << "{\"id\":" << frame.frame.value << ",\"gainEstimated\":" << (frame.gainEstimated ? "true" : "false")
             << ",\"noiseEstimated\":" << (frame.noiseEstimated ? "true" : "false") << ",\"radiometricConfidence\":" << frame.radiometricConfidence
             << ",\"accepted\":" << contribution.acceptedSamples << ",\"regions\":[";
        for (std::size_t r = 0; r < contribution.regions.size(); ++r) {
            if (r) json << ',';
            const auto& region = contribution.regions[r];
            json << '[' << region.x << ',' << region.y << ',' << region.width << ',' << region.height << ',' << region.accepted;
            for (auto rejected : region.rejected) json << ',' << rejected;
            json << ']';
        }
        json << "]}";
    }
    json << "]}";
    return e->NewStringUTF(json.str().c_str());
}

jstring capturePlan(JNIEnv* e, jobject observation, jobject intent, jobject capability) {
    if (!observation || !intent || !capability) throw std::invalid_argument("missing capture inputs");
    const Fields o(e, observation), i(e, intent), c(e, capability);
    runtime::CaptureObservations obs{o.integer64("exposureNs"), o.integer64("frameDurationNs"), positiveInt(o.integer("iso")), o.boolean("aeConverged"), {}, {}, o.boolean("gyroTimestampComparable")};
    if (o.real("noiseVariance") >= 0) obs.normalizedMidtoneVariance = o.real("noiseVariance");
    if (o.real("angularSpeed") >= 0) obs.angularSpeedRadiansPerSecond = o.real("angularSpeed");
    if (!std::isfinite(o.real("noiseVariance")) || !std::isfinite(o.real("angularSpeed"))) throw std::invalid_argument("non-finite optional observation");
    runtime::CaptureIntent aim{i.real("targetStandardDeviation"), i.integer64("latencyBudgetNs"), i.integer64("integrationBudgetNs"),
        i.real("maximumAngularTravel"), i.real("highlightExposureEv"), positiveInt(i.integer("maximumFrames"))};
    runtime::CaptureCapabilities caps{c.boolean("raw"), c.boolean("manualSensor"), c.boolean("aeLock"),
        c.integer64("minimumExposureNs"), c.integer64("maximumExposureNs"), c.integer64("minimumRawFrameDurationNs"), c.integer64("maximumFrameDurationNs"),
        positiveInt(c.integer("minimumIso")), positiveInt(c.integer("maximumIso")), positiveLong(c.integer64("retainedRawBudgetBytes")),
        positiveLong(c.integer64("bytesPerRawFrame")), positiveInt(c.integer("maximumRetainedFrames"))};
    const auto plan = runtime::compileCapturePlan(obs, aim, caps);
    std::ostringstream json; json.imbue(std::locale::classic());
    json << "{\"schemaVersion\":1,\"manual\":" << (plan.control == runtime::ExposureControl::Manual ? "true" : "false")
         << ",\"reason\":\"" << escape(plan.reason) << "\",\"frames\":[";
    for (std::size_t n = 0; n < plan.frames.size(); ++n) {
        if (n) json << ',';
        const auto& f = plan.frames[n];
        json << "{\"exposureNs\":" << f.exposureTimeNs << ",\"durationNs\":" << f.frameDurationNs << ",\"iso\":" << f.sensitivityIso << '}';
    }
    json << "]}";
    return e->NewStringUTF(json.str().c_str());
}

void translateException(JNIEnv* e) {
    if (e->ExceptionCheck()) return;
    const char* type = "java/lang/IllegalStateException";
    std::string message;
    try { throw; }
    catch (const PendingJavaException&) { return; }
    catch (const runtime::TemporalCancelled& x) { type = "java/util/concurrent/CancellationException"; message = x.what(); }
    catch (const std::invalid_argument& x) { type = "java/lang/IllegalArgumentException"; message = x.what(); }
    catch (const std::bad_alloc&) { type = "java/lang/OutOfMemoryError"; message = "native allocation failed"; }
    catch (const std::exception& x) { message = x.what(); }
    catch (...) { message = "unknown native execution failure"; }
    LocalRef klass(e, e->FindClass(type));
    if (klass.get()) e->ThrowNew(static_cast<jclass>(klass.get()), message.c_str());
}
} // namespace

extern "C" JNIEXPORT jstring JNICALL Java_dev_latent_camera_NativeBridge_processRaw(
    JNIEnv* e, jobject, jobjectArray frames, jobject output, jboolean vk, jlong memory, jfloat ev, jobject callback) {
    try { return process(e, frames, output, vk, memory, ev, callback); }
    catch (...) { translateException(e); return nullptr; }
}
extern "C" JNIEXPORT jstring JNICALL Java_dev_latent_camera_NativeBridge_capturePlan(
    JNIEnv* e, jobject, jobject observation, jobject intent, jobject capability) {
    try { return capturePlan(e, observation, intent, capability); }
    catch (...) { translateException(e); return nullptr; }
}

extern "C" JNIEXPORT jlong JNICALL Java_dev_latent_camera_NativeBridge_processingBound(
    JNIEnv* e, jobject, jint width, jint height, jint maximumFrames, jboolean preferVk) {
    try {
        const latent::imaging::Extent extent{positiveInt(width), positiveInt(height)};
        const bool vk = preferVk == JNI_TRUE && latent::vulkan::temporalFusionAvailable(extent);
        const auto bytes = latent::runtime::temporalWorkingSetBound(extent, positiveInt(maximumFrames), {},
            vk ? latent::runtime::TemporalBackend::Vulkan : latent::runtime::TemporalBackend::Reference);
        if (bytes > static_cast<std::uint64_t>(std::numeric_limits<jlong>::max()))
            throw std::invalid_argument("working set exceeds JNI byte range");
        return static_cast<jlong>(bytes);
    } catch (...) { translateException(e); return 0; }
}
