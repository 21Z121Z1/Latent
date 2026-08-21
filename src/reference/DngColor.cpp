#include "latent/reference/DngColor.h"

#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace latent::reference {
namespace {

imaging::Matrix3f diagonalMatrix(const std::array<float, 3>& diagonal) {
    return imaging::Matrix3f{{
        diagonal[0], 0.0F, 0.0F,
        0.0F, diagonal[1], 0.0F,
        0.0F, 0.0F, diagonal[2],
    }};
}

bool allFinite(const imaging::Matrix3f& m) {
    for (const auto value : m.values) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

}  // namespace

ProfileValidation validateDngProfile(const DngCameraProfile& profile) {
    const imaging::Matrix3f* matrices[] = {
        &profile.colorMatrix1,
        &profile.colorMatrix2,
        &profile.calibrationTransform1,
        &profile.calibrationTransform2,
        &profile.forwardMatrix1,
        &profile.forwardMatrix2,
    };
    for (const auto* matrix : matrices) {
        if (!allFinite(*matrix)) {
            return {false, "camera profile matrices must contain only finite values"};
        }
    }

    if (!(profile.calibrationIlluminant1Cct > 0.0F) ||
        !(profile.calibrationIlluminant2Cct > profile.calibrationIlluminant1Cct)) {
        return {false, "calibration illuminants must be positive and strictly ordered"};
    }

    for (const auto gain : profile.analogBalance) {
        if (!std::isfinite(gain) || !(gain > 0.0F)) {
            return {false, "analog balance must be finite and positive"};
        }
    }

    if (!inverted(profile.colorMatrix1).has_value() ||
        !inverted(profile.colorMatrix2).has_value()) {
        return {false, "color matrices must be invertible"};
    }

    if (profile.forwardMatricesPresent) {
        const auto d50 = imaging::xyToXyz(imaging::kIlluminantD50);
        for (const auto* forward : {&profile.forwardMatrix1, &profile.forwardMatrix2}) {
            const auto mapped = forward->apply({1.0F, 1.0F, 1.0F});
            for (std::size_t i = 0; i < 3; ++i) {
                if (std::fabs(mapped[i] - d50[i]) > 5.0e-3F) {
                    return {false,
                            "forward matrices must map the camera unit vector to XYZ D50"};
                }
            }
        }
    }

    return {};
}

imaging::Matrix3f interpolateByCct(
    float cct,
    float cct1,
    float cct2,
    const imaging::Matrix3f& matrix1,
    const imaging::Matrix3f& matrix2) {
    if (!(cct1 > 0.0F) || !(cct2 > cct1)) {
        throw std::invalid_argument("calibration illuminants must be positive and strictly ordered");
    }
    if (!std::isfinite(cct) || !(cct > 0.0F)) {
        throw std::invalid_argument("interpolation CCT must be finite and positive");
    }

    if (cct <= cct1) {
        return matrix1;
    }
    if (cct >= cct2) {
        return matrix2;
    }

    const float reciprocal = 1.0e6F / cct;
    const float t = (reciprocal - 1.0e6F / cct2) /
                    (1.0e6F / cct1 - 1.0e6F / cct2);

    imaging::Matrix3f result{};
    for (std::size_t i = 0; i < result.values.size(); ++i) {
        result.values[i] = matrix1.values[i] * t + matrix2.values[i] * (1.0F - t);
    }
    return result;
}

imaging::Matrix3f xyzToCameraMatrix(
    imaging::ChromaticityXY wbXy,
    const DngCameraProfile& profile) {
    const auto temperature = imaging::xyToCorrelatedTemperature(wbXy);
    const auto colorMatrix = interpolateByCct(
        temperature.cctKelvin,
        profile.calibrationIlluminant1Cct,
        profile.calibrationIlluminant2Cct,
        profile.colorMatrix1,
        profile.colorMatrix2);
    const auto calibrationTransform = interpolateByCct(
        temperature.cctKelvin,
        profile.calibrationIlluminant1Cct,
        profile.calibrationIlluminant2Cct,
        profile.calibrationTransform1,
        profile.calibrationTransform2);

    return diagonalMatrix(profile.analogBalance)
        .multiplied(calibrationTransform)
        .multiplied(colorMatrix);
}

std::array<float, 3> cameraNeutralFromXy(
    imaging::ChromaticityXY wbXy,
    const DngCameraProfile& profile) {
    const auto matrix = xyzToCameraMatrix(wbXy, profile);
    auto neutral = matrix.apply(imaging::xyToXyz(wbXy));
    if (!(std::fabs(neutral[1]) > 0.0F) || !std::isfinite(neutral[1])) {
        throw std::invalid_argument("white balance xy does not produce a valid camera neutral");
    }
    const float greenComponent = neutral[1];
    for (auto& component : neutral) {
        component /= greenComponent;
        if (!std::isfinite(component)) {
            throw std::invalid_argument("camera neutral is non-finite");
        }
    }
    return neutral;
}

imaging::ChromaticityXY xyFromCameraNeutral(
    const std::array<float, 3>& cameraNeutral,
    const DngCameraProfile& profile) {
    for (const auto component : cameraNeutral) {
        if (!std::isfinite(component)) {
            throw std::invalid_argument("camera neutral must be finite");
        }
    }

    imaging::ChromaticityXY xy{1.0F / 3.0F, 1.0F / 3.0F};
    constexpr float kConvergenceEpsilon = 1.0e-6F;
    constexpr int kMaxIterations = 100;

    for (int iteration = 0; iteration < kMaxIterations; ++iteration) {
        const auto previous = xy;
        const auto matrix = xyzToCameraMatrix(xy, profile);
        const auto inverse = inverted(matrix);
        if (!inverse.has_value()) {
            throw std::runtime_error("XYZ-to-camera matrix became singular during iteration");
        }

        const auto xyz = inverse->apply(cameraNeutral);
        if (!(xyz[1] > 0.0F)) {
            throw std::runtime_error("camera neutral did not map to a physical XYZ value");
        }

        xy = imaging::xyzToChromaticity(xyz);
        if (!std::isfinite(xy.x) || !std::isfinite(xy.y)) {
            throw std::runtime_error("white balance iteration diverged");
        }
        if (std::fabs(previous.x - xy.x) + std::fabs(previous.y - xy.y) <=
            kConvergenceEpsilon) {
            return xy;
        }
    }

    throw std::runtime_error("camera neutral to xy iteration did not converge");
}

CameraToXyzD50 cameraToXyzD50Matrix(
    imaging::ChromaticityXY wbXy,
    const DngCameraProfile& profile) {
    if (profile.forwardMatricesPresent) {
        const auto temperature = imaging::xyToCorrelatedTemperature(wbXy);
        const auto calibrationTransform = interpolateByCct(
            temperature.cctKelvin,
            profile.calibrationIlluminant1Cct,
            profile.calibrationIlluminant2Cct,
            profile.calibrationTransform1,
            profile.calibrationTransform2);

        const auto abCc = diagonalMatrix(profile.analogBalance)
                              .multiplied(calibrationTransform);
        const auto inverseAbCc = inverted(abCc);
        if (!inverseAbCc.has_value()) {
            throw std::invalid_argument("analog balance and calibration transform are singular");
        }

        const auto cameraNeutral = cameraNeutralFromXy(wbXy, profile);
        std::array<float, 3> referenceNeutral = inverseAbCc->apply(cameraNeutral);
        for (auto& component : referenceNeutral) {
            if (!(std::fabs(component) > 0.0F) || !std::isfinite(component)) {
                throw std::invalid_argument("reference camera neutral is degenerate");
            }
            component = 1.0F / component;
        }

        const auto diagonalD = diagonalMatrix(referenceNeutral);
        const auto forwardMatrix = interpolateByCct(
            temperature.cctKelvin,
            profile.calibrationIlluminant1Cct,
            profile.calibrationIlluminant2Cct,
            profile.forwardMatrix1,
            profile.forwardMatrix2);

        return {forwardMatrix.multiplied(diagonalD).multiplied(*inverseAbCc),
                CameraToXyzMethod::ForwardMatrix};
    }

    const auto xyzToCamera = xyzToCameraMatrix(wbXy, profile);
    const auto cameraToXyz = inverted(xyzToCamera);
    if (!cameraToXyz.has_value()) {
        throw std::invalid_argument("XYZ-to-camera matrix is singular");
    }

    const auto adaptation =
        imaging::bradfordAdaptation(wbXy, imaging::kIlluminantD50);
    return {adaptation.multiplied(*cameraToXyz), CameraToXyzMethod::InverseColorMatrix};
}

imaging::Matrix3f xyzD50ToAcescgMatrix() {
    static const imaging::Matrix3f matrix = [] {
        const auto adaptation =
            imaging::bradfordAdaptation(imaging::kIlluminantD50, imaging::kAcesWhite);
        const auto npmAp1 = imaging::rgbPrimariesToXyzMatrix(
            imaging::kAp1Red, imaging::kAp1Green, imaging::kAp1Blue, imaging::kAcesWhite);
        const auto inverseNpm = inverted(npmAp1);
        if (!inverseNpm.has_value()) {
            throw std::logic_error("AP1 normalised primary matrix must be invertible");
        }
        return inverseNpm->multiplied(adaptation);
    }();
    return matrix;
}

}  // namespace latent::reference
