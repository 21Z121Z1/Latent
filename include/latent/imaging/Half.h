#pragma once

#include <cstdint>
#include <cstring>

namespace latent::imaging {

// IEEE-754 binary16 helpers implemented with explicit integer arithmetic and
// round-to-nearest-even, so CPU-side quantization is bit-identical to Vulkan
// packHalf2x16/unpackHalf2x16 on any conformant driver.
//
// half layout: sign(1) | exponent(5, bias 15) | mantissa(10)

[[nodiscard]] inline float halfBitsToFloat(std::uint16_t bits) noexcept {
    const std::uint32_t sign = static_cast<std::uint32_t>(bits & 0x8000U) << 16U;
    const std::uint32_t exponent = static_cast<std::uint32_t>(bits >> 10) & 0x1FU;
    const std::uint32_t mantissa = bits & 0x03FFU;

    std::uint32_t outBits;
    if (exponent == 0U) {
        if (mantissa == 0U) {
            outBits = sign;
        } else {
            // Renormalize: find the leading bit of the subnormal significand.
            std::uint32_t shifted = mantissa;
            std::uint32_t leadingZeros = 0U;
            while ((shifted & 0x400U) == 0U) {
                shifted <<= 1U;
                ++leadingZeros;
            }
            const std::uint32_t normalizedExponent =
                127U - 15U + 1U - leadingZeros;
            outBits = sign | (normalizedExponent << 23U) |
                      ((shifted & 0x3FFU) << 13U);
        }
    } else if (exponent == 31U) {
        outBits = sign | 0x7F800000U | (mantissa << 13U);
    } else {
        outBits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
    }

    float result = 0.0F;
    std::memcpy(&result, &outBits, sizeof(result));
    return result;
}

[[nodiscard]] inline std::uint16_t floatToHalfBits(float value) noexcept {
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));

    const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
    const std::uint32_t rest = bits & 0x7FFFFFFFU;

    if (rest >= 0x7F800000U) {
        if (rest > 0x7F800000U) {
            // NaN: preserve the top payload bits.
            return static_cast<std::uint16_t>(sign | 0x7E00U |
                                              ((rest >> 13U) & 0x03FFU));
        }
        return static_cast<std::uint16_t>(sign | 0x7C00U);
    }

    const std::int32_t biased32 = static_cast<std::int32_t>((rest >> 23U) & 0xFFU);
    const std::uint32_t mantissa = rest & 0x007FFFFFU;

    if (biased32 == 0 && mantissa == 0) {
        return sign;
    }

    // Full significand with the implicit bit; value = sig * 2^(trueExp - 23).
    const std::uint32_t significand =
        (biased32 == 0 ? mantissa : (mantissa | 0x800000U));
    std::int32_t trueExp = biased32 - 127;
    if (biased32 == 0) {
        trueExp = -126;
    }

    std::int32_t exp16 = trueExp + 15;

    if (exp16 >= 31) {
        return static_cast<std::uint16_t>(sign | 0x7C00U);
    }

    if (exp16 > 0) {
        // Normal half: round the 24-bit significand to 11 bits.
        constexpr std::uint32_t kShift = 13U;
        const std::uint32_t rounded = static_cast<std::uint32_t>(significand >> kShift);
        const std::uint32_t remainder =
            significand & static_cast<std::uint32_t>((1ULL << kShift) - 1ULL);
        constexpr std::uint32_t kHalfway = 1U << (kShift - 1U);

        std::uint32_t quantized = rounded;
        if (remainder > kHalfway ||
            (remainder == kHalfway && (quantized & 1U) != 0U)) {
            ++quantized;
        }
        if (quantized == (1U << 11U)) {
            quantized >>= 1U;
            ++exp16;
            if (exp16 >= 31) {
                return static_cast<std::uint16_t>(sign | 0x7C00U);
            }
        }
        return static_cast<std::uint16_t>(
            sign | (static_cast<std::uint32_t>(exp16) << 10U) |
            (quantized & 0x3FFU));
    }

    // Subnormal half: value = round(sig * 2^(trueExp + 1)) * 2^-24.
    // exp16 <= 0 implies trueExp <= -15, so the right shift is at least 14.
    const auto shift = static_cast<std::uint32_t>(-trueExp - 1);
    std::uint32_t quantized;
    std::uint32_t remainder;

    if (shift <= 24U) {
        quantized = significand >> shift;
        remainder =
            significand & static_cast<std::uint32_t>((1ULL << shift) - 1ULL);
    } else {
        // Every significand bit shifted out; a nonzero sticky remainder still
        // participates in rounding.
        quantized = 0U;
        remainder = significand != 0U ? 1U : 0U;
    }

    const std::uint32_t halfway = 1U << (shift - 1U);
    if (remainder > halfway || (remainder == halfway && (quantized & 1U) != 0U)) {
        ++quantized;
    }

    if (quantized == (1U << 10U)) {
        // Rounded up into the smallest normal.
        return static_cast<std::uint16_t>(sign | (1U << 10U));
    }
    return static_cast<std::uint16_t>(sign | quantized);
}

}  // namespace latent::imaging
