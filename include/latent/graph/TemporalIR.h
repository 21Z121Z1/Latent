#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace latent::graph {
// Minimal canonical temporal slice. The legacy image-only ImagingGraph cannot
// represent a burst/selection/field value; do not pretend those are RGB images.
// These entries are the source for plan stages and future derived introspection.
enum class TemporalOperation : std::uint8_t {
    SelectReference, NormalizeFrame, AlignFrame, FuseRaw, ReconstructScene
};
enum class TemporalValue : std::uint8_t { RawBurst, Selection, SensorLinear, Alignment, Scene };
struct TemporalSchema {
    TemporalOperation operation;
    std::string_view name;
    TemporalValue input;
    TemporalValue output;
    bool progressive;
    bool changesDomain;
};
inline constexpr std::uint32_t temporalSchemaVersion = 1U;
inline constexpr std::array temporalSchemas{
    TemporalSchema{TemporalOperation::SelectReference, "select-reference", TemporalValue::RawBurst, TemporalValue::Selection, false, false},
    TemporalSchema{TemporalOperation::NormalizeFrame, "normalize-temporal-raw", TemporalValue::RawBurst, TemporalValue::SensorLinear, true, false},
    TemporalSchema{TemporalOperation::AlignFrame, "align-temporal-raw", TemporalValue::SensorLinear, TemporalValue::Alignment, true, false},
    TemporalSchema{TemporalOperation::FuseRaw, "robust-raw-fusion", TemporalValue::SensorLinear, TemporalValue::SensorLinear, true, false},
    TemporalSchema{TemporalOperation::ReconstructScene, "reconstruct-scene", TemporalValue::SensorLinear, TemporalValue::Scene, false, true}
};
}  // namespace latent::graph
