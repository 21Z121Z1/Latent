#pragma once

#include "latent/imaging/ColorScience.h"
#include "latent/imaging/Types.h"

#include <array>
#include <cstdint>
#include <string>

namespace latent::reference {

struct DngCameraProfile {
    imaging::Matrix3f colorMatrix1{};
    imaging::Matrix3f colorMatrix2{};
    imaging::Matrix3f calibrationTransform1{};
    imaging::Matrix3f calibrationTransform2{};
    imaging::Matrix3f forwardMatrix1{};
    imaging::Matrix3f forwardMatrix2{};

    bool forwardMatricesPresent = false;
    float calibrationIlluminant1Cct = 2850.0F;
    float calibrationIlluminant2Cct = 6500.0F;
    std::array<float, 3> analogBalance{1.0F, 1.0F, 1.0F};
};

struct ProfileValidation {
    bool valid = true;
    std::string message;
};

[[nodiscard]] ProfileValidation validateDngProfile(const DngCameraProfile& profile);

[[nodiscard]] imaging::Matrix3f interpolateByCct(
    float cct,
    float cct1,
    float cct2,
    const imaging::Matrix3f& matrix1,
    const imaging::Matrix3f& matrix2);

[[nodiscard]] imaging::Matrix3f xyzToCameraMatrix(
    imaging::ChromaticityXY wbXy,
    const DngCameraProfile& profile);

[[nodiscard]] std::array<float, 3> cameraNeutralFromXy(
    imaging::ChromaticityXY wbXy,
    const DngCameraProfile& profile);

[[nodiscard]] imaging::ChromaticityXY xyFromCameraNeutral(
    const std::array<float, 3>& cameraNeutral,
    const DngCameraProfile& profile);

enum class CameraToXyzMethod : std::uint8_t {
    ForwardMatrix,
    InverseColorMatrix,
};

struct CameraToXyzD50 {
    imaging::Matrix3f matrix{};
    CameraToXyzMethod method = CameraToXyzMethod::ForwardMatrix;
};

[[nodiscard]] CameraToXyzD50 cameraToXyzD50Matrix(
    imaging::ChromaticityXY wbXy,
    const DngCameraProfile& profile);

[[nodiscard]] imaging::Matrix3f xyzD50ToAcescgMatrix();

}  // namespace latent::reference
