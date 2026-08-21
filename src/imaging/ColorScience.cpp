#include "latent/imaging/ColorScience.h"

#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace latent::imaging {
namespace {

// Robertson (1968) reciprocal-temperature interpolation table from
// Wyszecki & Stiles, "Color Science", 2nd edition, p. 228, with the
// 325-mired correction; identical to the table used by the Adobe DNG SDK.
constexpr std::array<float, 31> kRobertsonMired{
    0.0F, 10.0F, 20.0F, 30.0F, 40.0F, 50.0F, 60.0F, 70.0F, 80.0F, 90.0F,
    100.0F, 125.0F, 150.0F, 175.0F, 200.0F, 225.0F, 250.0F, 275.0F,
    300.0F, 325.0F, 350.0F, 375.0F, 400.0F, 425.0F, 450.0F, 475.0F,
    500.0F, 525.0F, 550.0F, 575.0F, 600.0F};

constexpr std::array<float, 31> kRobertsonU{
    0.18006F, 0.18066F, 0.18133F, 0.18208F, 0.18293F, 0.18388F, 0.18494F,
    0.18611F, 0.18740F, 0.18880F, 0.19032F, 0.19462F, 0.19962F, 0.20525F,
    0.21142F, 0.21807F, 0.22511F, 0.23247F, 0.24010F, 0.24702F, 0.25591F,
    0.26400F, 0.27218F, 0.28039F, 0.28863F, 0.29685F, 0.30505F, 0.31320F,
    0.32129F, 0.32931F, 0.33724F};

constexpr std::array<float, 31> kRobertsonV{
    0.26352F, 0.26589F, 0.26846F, 0.27119F, 0.27407F, 0.27709F, 0.28021F,
    0.28342F, 0.28668F, 0.28997F, 0.29326F, 0.30141F, 0.30921F, 0.31647F,
    0.32312F, 0.32909F, 0.33439F, 0.33904F, 0.34308F, 0.34655F, 0.34951F,
    0.35200F, 0.35407F, 0.35577F, 0.35714F, 0.35823F, 0.35907F, 0.35968F,
    0.36011F, 0.36038F, 0.36051F};

constexpr std::array<float, 31> kRobertsonSlope{
    -0.24341F, -0.25479F, -0.26876F, -0.28539F, -0.30470F, -0.32675F,
    -0.35156F, -0.37915F, -0.40955F, -0.44278F, -0.47888F, -0.58204F,
    -0.70471F, -0.84901F, -1.0182F, -1.2168F, -1.4512F, -1.7298F,
    -2.0637F, -2.4681F, -2.9641F, -3.5814F, -4.3633F, -5.3762F,
    -6.7262F, -8.5955F, -11.324F, -15.628F, -23.325F, -40.770F, -116.45F};

constexpr std::array<float, 9> kBradfordBasis{
    0.8951F, 0.2664F, -0.1614F,
    -0.7502F, 1.7135F, 0.0367F,
    0.0389F, -0.0685F, 1.0296F};

Matrix3f diagonalMatrix(const std::array<float, 3>& diagonal) {
    return Matrix3f{{
        diagonal[0], 0.0F, 0.0F,
        0.0F, diagonal[1], 0.0F,
        0.0F, 0.0F, diagonal[2],
    }};
}

}  // namespace

Matrix3f rgbPrimariesToXyzMatrix(
    ChromaticityXY red,
    ChromaticityXY green,
    ChromaticityXY blue,
    ChromaticityXY white) {
    if (std::fabs(red.y) < 1.0e-9F || std::fabs(green.y) < 1.0e-9F ||
        std::fabs(blue.y) < 1.0e-9F) {
        throw std::invalid_argument("primary chromaticities require non-zero y");
    }
    if (!(white.y > 0.0F)) {
        throw std::invalid_argument("white chromaticity requires positive y");
    }

    const Xyz primaryX = {red.x / red.y, green.x / green.y, blue.x / blue.y};
    const Xyz primaryZ = {
        (1.0F - red.x - red.y) / red.y,
        (1.0F - green.x - green.y) / green.y,
        (1.0F - blue.x - blue.y) / blue.y,
    };

    const Matrix3f primaries{{
        primaryX[0], primaryX[1], primaryX[2],
        1.0F, 1.0F, 1.0F,
        primaryZ[0], primaryZ[1], primaryZ[2],
    }};

    const auto scale = inverted(primaries);
    if (!scale.has_value()) {
        throw std::invalid_argument("degenerate primary chromaticity set");
    }

    const auto whiteVector = scale->apply(xyToXyz(white));
    for (const auto component : whiteVector) {
        if (!std::isfinite(component)) {
            throw std::invalid_argument("non-finite RP177 scale factor");
        }
    }

    return Matrix3f{{
        whiteVector[0] * primaryX[0], whiteVector[1] * primaryX[1], whiteVector[2] * primaryX[2],
        whiteVector[0], whiteVector[1], whiteVector[2],
        whiteVector[0] * primaryZ[0], whiteVector[1] * primaryZ[1], whiteVector[2] * primaryZ[2],
    }};
}

Matrix3f bradfordAdaptation(ChromaticityXY sourceWhite, ChromaticityXY targetWhite) {
    const Matrix3f basis{kBradfordBasis};
    const auto sourceCone = basis.apply(xyToXyz(sourceWhite));
    const auto targetCone = basis.apply(xyToXyz(targetWhite));

    std::array<float, 3> ratio{};
    for (std::size_t i = 0; i < ratio.size(); ++i) {
        if (!(std::fabs(sourceCone[i]) > 0.0F) || !std::isfinite(targetCone[i])) {
            throw std::invalid_argument("degenerate Bradford cone response");
        }
        ratio[i] = targetCone[i] / sourceCone[i];
    }

    const auto inverseBasis = inverted(basis);
    if (!inverseBasis.has_value()) {
        throw std::invalid_argument("Bradford basis must be invertible");
    }

    return inverseBasis->multiplied(diagonalMatrix(ratio)).multiplied(basis);
}

CorrelatedTemperature xyToCorrelatedTemperature(ChromaticityXY xy) {
    if (!std::isfinite(xy.x) || !std::isfinite(xy.y) || !(xy.y > 0.0F)) {
        throw std::invalid_argument("chromaticity must be finite with positive y");
    }

    const float denominator = 1.5F - xy.x + 6.0F * xy.y;
    if (!(std::fabs(denominator) > 0.0F)) {
        throw std::invalid_argument("chromaticity is outside the Robertson domain");
    }

    const float u = 2.0F * xy.x / denominator;
    const float v = 3.0F * xy.y / denominator;
    if (!std::isfinite(u) || !std::isfinite(v)) {
        throw std::invalid_argument("chromaticity maps outside the Robertson domain");
    }

    float lastDt = 0.0F;
    float lastDu = 0.0F;
    float lastDv = 0.0F;

    for (std::size_t index = 1; index < kRobertsonMired.size(); ++index) {
        float dv = kRobertsonSlope[index];
        const float length = std::sqrt(1.0F + dv * dv);
        const float du = 1.0F / length;
        dv /= length;

        const float uu = u - kRobertsonU[index];
        const float vv = v - kRobertsonV[index];
        const float dt = -uu * dv + vv * du;

        if (dt <= 0.0F || index == kRobertsonMired.size() - 1U) {
            float distance = dt > 0.0F ? 0.0F : dt;
            distance = -distance;

            const float f =
                index == 1 ? 0.0F : distance / (lastDt + distance);

            CorrelatedTemperature result{};
            const float mired = kRobertsonMired[index - 1U] * f +
                                kRobertsonMired[index] * (1.0F - f);
            result.cctKelvin = 1.0e6F / mired;

            const float interpolatedU = kRobertsonU[index - 1U] * f +
                                        kRobertsonU[index] * (1.0F - f);
            const float interpolatedV = kRobertsonV[index - 1U] * f +
                                        kRobertsonV[index] * (1.0F - f);

            float slopeU = du * (1.0F - f) + lastDu * f;
            float slopeV = dv * (1.0F - f) + lastDv * f;
            const float slopeLength = std::sqrt(slopeU * slopeU + slopeV * slopeV);
            if (!(slopeLength > 0.0F)) {
                slopeU = 1.0F;
                slopeV = 0.0F;
            } else {
                slopeU /= slopeLength;
                slopeV /= slopeLength;
            }

            const float offsetU = u - interpolatedU;
            const float offsetV = v - interpolatedV;
            result.tint = (offsetU * slopeU + offsetV * slopeV) * -3000.0F;

            if (!std::isfinite(result.cctKelvin) || !std::isfinite(result.tint)) {
                throw std::invalid_argument("correlated temperature is non-finite");
            }
            return result;
        }

        lastDt = dt;
        lastDu = du;
        lastDv = dv;
    }

    throw std::logic_error("Robertson interpolation did not terminate");
}

float xyToCctMcCamy(ChromaticityXY xy) noexcept {
    const float n = (xy.x - 0.3320F) / (0.1858F - xy.y);
    return 449.0F * n * n * n + 3525.0F * n * n + 6823.3F * n + 5520.33F;
}

}  // namespace latent::imaging
