#include "NativeTransport.h"
#include "latent/runtime/TiledReconstruction.h"
#include <bit>
#include <filesystem>
#include <fstream>
#include <locale>
#include <sstream>

namespace {
using namespace latent;
using namespace latent::android_transport;
std::string javaString(JNIEnv* e,jstring value) {
    if(!value)throw std::invalid_argument("missing path");
    const char* chars=e->GetStringUTFChars(value,nullptr);checkJava(e);
    if(!chars)throw std::bad_alloc{};
    try{std::string s(chars);e->ReleaseStringUTFChars(value,chars);return s;}
    catch(...){e->ReleaseStringUTFChars(value,chars);throw;}
}
// Row-major 16-float pixels: RGB+reserved, conditional variance+reserved,
// effective temporal count+reserved, confidence+reserved. Header is ASCII
// LATENT-CAMERA-RGB-V1\nWIDTH HEIGHT\n followed by little-endian FP32 data.
// Publish by same-directory rename only after all tiles and flush succeed.
class PixelFile final:public runtime::ReconstructionTileSink {
public:
    PixelFile(std::filesystem::path path,imaging::Extent extent):path_(std::move(path)),partial_(path_.string()+".partial"),extent_(extent) {
        if(std::endian::native!=std::endian::little)throw std::invalid_argument("little-endian output required");
        if(std::filesystem::exists(path_)||std::filesystem::exists(partial_))throw std::invalid_argument("refusing to overwrite reconstruction output");
        file_.open(partial_,std::ios::out|std::ios::binary|std::ios::trunc);
        file_<<"LATENT-CAMERA-RGB-V1\n"<<extent.width<<' '<<extent.height<<'\n';
        header_=file_.tellp();if(header_<0){file_.close();std::error_code error;std::filesystem::remove(partial_,error);throw std::runtime_error("cannot create output");}
    }
    ~PixelFile() override {file_.close();if(!published_){std::error_code error;std::filesystem::remove(partial_,error);}}
    std::uint64_t residentBytes() const override{return 8192;}
    void write(imaging::SensorRect tile,std::span<const reference::ReconstructedPixel> pixels) override {
        for(std::uint32_t y=0;y<tile.height;++y) {
            const auto offset=(static_cast<std::uint64_t>(tile.y+y)*extent_.width+tile.x)*sizeof(reference::ReconstructedPixel);
            file_.seekp(header_+static_cast<std::streamoff>(offset));
            file_.write(reinterpret_cast<const char*>(pixels.data()+static_cast<std::size_t>(y)*tile.width),
                static_cast<std::streamsize>(tile.width*sizeof(reference::ReconstructedPixel)));
            if(!file_)throw std::runtime_error("reconstruction output write failed (disk full?)");
        }
    }
    void publish(){file_.flush();if(!file_)throw std::runtime_error("output flush failed");file_.close();std::filesystem::rename(partial_,path_);published_=true;}
private:
    std::filesystem::path path_,partial_;imaging::Extent extent_;std::ofstream file_;std::streamoff header_=0;bool published_=false;
};
void translate(JNIEnv* e) {
    if(e->ExceptionCheck())return;
    const char* klass="java/lang/IllegalStateException";std::string text;
    try{throw;}catch(const PendingJavaException&){return;}
    catch(const std::invalid_argument& x){klass="java/lang/IllegalArgumentException";text=x.what();}
    catch(const std::bad_alloc&){klass="java/lang/OutOfMemoryError";text="bounded RAW allocation failed";}
    catch(const std::exception& x){text=x.what();}catch(...){text="unknown reconstruction failure";}
    LocalRef type(e,e->FindClass(klass));if(type.get())e->ThrowNew(static_cast<jclass>(type.get()),text.c_str());
}
}
extern "C" JNIEXPORT jstring JNICALL Java_dev_latent_camera_NativeBridge_interpretSensorMode(JNIEnv* e,jobject,jobject mode) {
    try{return e->NewStringUTF(imaging::samplingJson(sensorMode(e,mode)).c_str());}catch(...){translate(e);return nullptr;}
}
extern "C" JNIEXPORT jstring JNICALL Java_dev_latent_camera_NativeBridge_reconstructRawFiles(JNIEnv* e,jobject,
    jobjectArray inputs,jobjectArray modes,jobjectArray paths,jstring output,jboolean vk,jlong budget,jint thermal) {
    try {
        if(!inputs||!modes||!paths)throw std::invalid_argument("missing file burst transport");
        const auto count=e->GetArrayLength(inputs);
        if(count<1||count>32||e->GetArrayLength(modes)!=count||e->GetArrayLength(paths)!=count||thermal<0||thermal>2)
            throw std::invalid_argument("invalid file burst membership/thermal observation");
        imaging::RawBurst burst{};burst.id={1};burst.sequence={1};burst.calibration={1};
        std::vector<runtime::RawFileBinding> files;std::vector<ParsedRawInput> parsed;
        for(jsize n=0;n<count;++n) {
            LocalRef input(e,e->GetObjectArrayElement(inputs,n)),mode(e,e->GetObjectArrayElement(modes,n)),path(e,e->GetObjectArrayElement(paths,n));checkJava(e);
            const Fields fields(e,input.get());auto raw=parseRawMetadata(fields,true);
            const imaging::Extent extent{positiveInt(fields.integer("width")),positiveInt(fields.integer("height"))};
            if(n==0)burst.extent=extent;
            if(extent!=burst.extent)throw std::invalid_argument("file burst extent mismatch");
            raw.member.observations.sampling=sensorMode(e,mode.get());
            const auto valid=imaging::validateSampling(*raw.member.observations.sampling,extent);
            if(!valid.valid)throw std::invalid_argument(valid.message);
            files.push_back({raw.member.id,javaString(e,static_cast<jstring>(path.get())),0,extent.width});
            burst.members.push_back(raw.member);parsed.push_back(std::move(raw));
        }
        const auto bytes=positiveLong(budget);
        runtime::ReconstructionCapabilities caps{};caps.hostBudgetBytes=bytes;caps.preferVulkan=vk==JNI_TRUE;
        caps.deviceBudgetBytes=std::min<std::uint64_t>(bytes/2,64U*1024U*1024U);caps.collectGpuTimings=true;
        caps.thermalSeverity=static_cast<std::uint32_t>(thermal);
        runtime::TiledReconstructionPolicy policy{};policy.maximumFrames=static_cast<std::uint32_t>(count);
        auto source=runtime::makeFileTileSource(burst,files);PixelFile sink(javaString(e,output),burst.extent);
        const auto trace=runtime::reconstructRawTiles(burst,*source,{burst.extent},policy,caps,sink);
        const auto index=static_cast<std::size_t>(std::find_if(parsed.begin(),parsed.end(),[&](const auto& p){return p.member.id==trace.reference;})-parsed.begin());
        std::ostringstream json;json.imbue(std::locale::classic());
        json<<"{\"schema\":1,\"output_domain\":\"reference-camera-linear-green-balanced\",\"trace\":"<<runtime::directReconstructionJson(trace)<<",\"sampling\":[";
        for(std::size_t i=0;i<parsed.size();++i){if(i)json<<',';json<<imaging::samplingJson(*parsed[i].member.observations.sampling);}
        json<<"],\"white_balance\":[";
        for(std::size_t i=0;i<4;++i){if(i)json<<',';json<<parsed[index].whiteBalance[i];}
        const auto& wb=parsed[index].whiteBalance;
        json<<"],\"rgb_white_balance\":["<<wb[0]<<','<<0.5F*(wb[1]+wb[2])<<','<<wb[3]<<"],\"sensor_to_linear_srgb\":[";
        const auto& color=parsed[index].color;
        for(std::size_t i=0;i<9;++i){if(i)json<<',';json<<color.values[i];}
        json<<"]}";
        // Allocate Java report before publishing, so JNI OOM does not masquerade as success.
        jstring result=e->NewStringUTF(json.str().c_str());checkJava(e);if(!result)throw std::bad_alloc{};
        sink.publish();return result;
    }catch(...){translate(e);return nullptr;}
}
