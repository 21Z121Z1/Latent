#pragma once

#include "latent/imaging/RawFrame.h"
#include "latent/imaging/SceneFrame.h"
#include "latent/reference/ReferenceReconstruct.h"

namespace latent::backend {

class ImagingBackend {
public:
    virtual ~ImagingBackend() = default;

    [[nodiscard]] virtual imaging::SceneFrame reconstructSingleRaw(
        const imaging::RawFrame& raw,
        const reference::ReconstructionConfig& config) const = 0;
};

class ReferenceFP32Backend final : public ImagingBackend {
public:
    [[nodiscard]] imaging::SceneFrame reconstructSingleRaw(
        const imaging::RawFrame& raw,
        const reference::ReconstructionConfig& config) const override {
        return reference::reconstructSingleRaw(raw, config);
    }
};

}  // namespace latent::backend
