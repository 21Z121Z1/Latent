#include "latent/imaging/RawBurst.h"
#include "latent/imaging/Lineage.h"
#include "latent/reference/RawNormalize.h"
#include "latent/reference/NoisePropagation.h"
#include "latent/reference/ReferenceReconstruct.h"
#include "latent/render/ReferenceRenderer.h"
#include "latent/codec/UltraHdrStaging.h"
#include "latent/runtime/RawBindings.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template <typename F> void rejects(F&& fn, const char* message) {
    bool rejected = false;
    try { fn(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, message);
}
latent::imaging::RawFrame frame(std::uint64_t id) {
    using namespace latent::imaging;
    RawFrame value{};
    value.id = id;
    value.cameraId = "synthetic";
    value.sensorMode = "3x3";
    value.sensorTimestampNs = static_cast<std::int64_t>(id * 1000U);
    value.exposureTimeNs = 1000;
    value.sensitivityIso = 100;
    value.storage.extent = {3, 3};
    value.storage.rowStridePixels = 4;
    value.storage.pixels = {50, 500, 1100, 0, 300, 900, 600, 0, 400, 700, 800};
    value.staticBlack = {BlackLevel{{100, 100, 100, 100}}, MetadataSource::StaticCharacteristic, MetadataValidity::Valid, 1};
    value.staticWhite = {1000, MetadataSource::StaticCharacteristic, MetadataValidity::Valid, 1};
    return value;
}
void contract() {
    using namespace latent;
    static_assert(!std::is_convertible_v<imaging::FrameId, imaging::BurstId>);
    static_assert(!std::is_convertible_v<std::uint64_t, imaging::FrameId>);
    std::vector<imaging::RawFrame> frames{frame(1), frame(2)};
    auto burst = imaging::describeRawBurst(imaging::BurstId{4}, imaging::CaptureSequenceId{5}, imaging::CalibrationId{6}, frames);
    auto bindings = runtime::bindReferenceFrames(frames);
    check(imaging::validateRawBurst(burst).valid(), "valid ordered burst");
    check(bindings.validate(burst).valid, "non-owning host bindings");
    const auto view = bindings.view(burst, imaging::FrameId{1});
    check(view.storage.pixels.data() == frames[0].storage.pixels.data(), "binding does not copy payload");
    check(view.metadata == &burst.members[0].observations, "descriptor owns the authoritative metadata snapshot");
    const auto owned = reference::normalizeRaw(frames[0]);
    const auto borrowed = reference::normalizeRaw(view);
    check(owned.samples == borrowed.samples, "owning and borrowed normalization agree");
    check(borrowed.samples[0] < 0 && borrowed.samples[2] > 1, "sub-black and above-white survive borrowed normalization");
    frames[0].sensitivityIso = 800;
    check(view.metadata->sensitivityIso == 100, "fixture metadata edits cannot mutate a semantic snapshot");
    auto bad = burst; bad.members[1].id = bad.members[0].id;
    check(imaging::validateRawBurst(bad).code == imaging::BurstValidationCode::DuplicateFrame, "duplicate member rejected");
    bad = burst; bad.members[1].observations.sensorTimestampNs = 1;
    check(!imaging::validateRawBurst(bad).valid(), "out-of-order timestamps rejected");
    bad = burst; bad.members[1].observations.cameraId = "other";
    check(!imaging::validateRawBurst(bad).valid(), "cross-sensor calibration rejected");
    bad = burst; bad.members[0].observations.sensitivityIso = std::numeric_limits<float>::quiet_NaN();
    check(!imaging::validateRawBurst(bad).valid(), "non-finite observations rejected");
    runtime::HostRawBindings wrong({{imaging::FrameId{1}, view.storage}, {imaging::FrameId{1}, view.storage}});
    check(!wrong.validate(burst).valid, "duplicate binding rejected");
    auto shortView = view; shortView.storage.pixels = shortView.storage.pixels.first(2);
    check(!runtime::validateRawView(shortView).valid, "short binding rejected");
    rejects([&] { (void)bindings.view(burst, imaging::FrameId{999}); }, "unknown resource rejected");
    bad = burst; bad.extent.width = 2;
    rejects([&] { (void)bindings.view(bad, imaging::FrameId{1}); }, "view rejects mismatched semantic extent");
}
void noiseUnits() {
    using namespace latent;
    imaging::NoiseModel android{};
    android.coordinate = imaging::NoiseCoordinate::NormalizedBlackSubtracted;
    android.shot.fill(0.002F); android.read.fill(0.000001F);
    const auto levels = reference::selectRawLevels(frame(1));
    const auto normalized = reference::normalizeNoiseModel(android, levels);
    check(normalized.shot == android.shot && normalized.read == android.read, "Android coefficients are not normalized twice");
    imaging::NoiseModel raw{}; raw.shot.fill(2); raw.read.fill(9);
    const auto n = reference::normalizeNoiseModel(raw, levels);
    check(std::fabs(n.shot[0] - 2.0F / 900.0F) < 1e-8F, "raw shot units converted");
    check(std::fabs(n.read[0] - 209.0F / 810000.0F) < 1e-9F, "raw read offset includes black coordinate change");
    check(reference::normalizeNoiseModel(n, levels).read == n.read, "noise normalization is idempotent");
    auto invalid = frame(1); invalid.staticBlack.value->cfa[2] = std::numeric_limits<float>::infinity();
    rejects([&] { (void)reference::normalizeRaw(invalid); }, "infinite level rejected");
    imaging::MetadataValue<float> stale{}; stale.value = 1;
    check(!stale.usable(), "missing validity cannot become usable through a stale value");
    imaging::LensShadingMap oversized{0xffffffffU, 0xffffffffU, {}};
    check(!imaging::validateLensShadingMap(oversized).valid, "LSC dimensions cannot wrap gain count");
    invalid = frame(1); invalid.noiseProfile.source = static_cast<imaging::MetadataSource>(255);
    check(!imaging::validateRawMetadata(invalid).valid, "unknown metadata enum rejected");
}
void lineage() {
    using namespace latent;
    reference::ReconstructionConfig config{};
    config.cameraToAcescg = imaging::Matrix3f::identity();
    auto scene = reference::reconstructSingleRaw(frame(1), config);
    check(scene.lineage && scene.lineage->reference == imaging::FrameId{1}, "single RAW gets typed lineage");
    auto l = std::make_shared<imaging::ImageLineage>();
    l->burst = imaging::BurstId{7}; l->sequence = imaging::CaptureSequenceId{8};
    l->calibration = imaging::CalibrationId{9};
    l->reference = imaging::FrameId{1}; l->inputs = {imaging::FrameId{1}, imaging::FrameId{2}};
    scene.sourceRawId = 0; scene.lineage = l;
    auto sdr = render::renderReference(scene, render::makeSdrRenderConfig());
    auto hdr = render::renderReference(scene, render::makeHdrPqRenderConfig());
    check(sdr.lineage == scene.lineage && hdr.lineage == scene.lineage, "renditions retain immutable multi-source lineage without copying it");
    const auto packed = codec::stageUltraHdrRenditions(sdr, hdr);
    check(packed.lineage == scene.lineage && packed.sourceRawId == 0, "codec retains multiframe lineage, not a scalar substitute");
    auto other = std::make_shared<imaging::ImageLineage>(*l); other->burst = imaging::BurstId{99}; hdr.lineage = other;
    rejects([&] { (void)codec::stageUltraHdrRenditions(sdr, hdr); }, "scalar zero must not conflate different bursts");
}
}  // namespace
int main() {
    try { contract(); noiseUnits(); lineage(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    if (failures != 0) return 1;
    std::cout << "temporal contract tests passed\n";
    return 0;
}
