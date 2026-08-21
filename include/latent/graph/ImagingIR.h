#pragma once

#include "latent/imaging/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace latent::graph {

using ValueId = std::uint32_t;

enum class AccessPattern : std::uint8_t {
    Point,
    Neighborhood,
    Reduction,
    Temporal,
    External,
};

enum class OperationKind : std::uint8_t {
    RawIngress,
    BlackSubtract,
    SensorNormalize,
    DefectCorrect,
    LensShading,
    WhiteBalance,
    Demosaic,
    ColorTransform,
    SceneDenoise,
    Analyze,
    RenderSDR,
    RenderHDR,
    GainMapEncode,
};

struct OperationDescriptor {
    OperationKind kind = OperationKind::RawIngress;
    std::string name;
    std::vector<ValueId> inputs;
    imaging::ImageType outputType{};

    AccessPattern access = AccessPattern::Point;
    std::uint32_t radiusX = 0;
    std::uint32_t radiusY = 0;
    bool pure = true;
    bool canFuse = true;
    bool temporal = false;
    bool changesReferenceDomain = false;
};

struct Value {
    ValueId id = 0;
    std::string name;
    imaging::ImageType type{};
    bool graphInput = false;
};

struct Operation {
    OperationDescriptor descriptor{};
    ValueId output = 0;
};

struct GraphValidation {
    bool valid = true;
    std::vector<std::string> errors;
};

struct FusionGroup {
    std::vector<std::size_t> operationIndices;
};

class ImagingGraph {
public:
    [[nodiscard]] ValueId addInput(std::string name, imaging::ImageType type);
    [[nodiscard]] ValueId addOperation(OperationDescriptor descriptor);

    [[nodiscard]] const std::vector<Value>& values() const noexcept { return values_; }
    [[nodiscard]] const std::vector<Operation>& operations() const noexcept { return operations_; }

    [[nodiscard]] GraphValidation validate() const;
    [[nodiscard]] std::vector<FusionGroup> planConservativeFusion() const;

private:
    std::vector<Value> values_;
    std::vector<Operation> operations_;
};

}  // namespace latent::graph
