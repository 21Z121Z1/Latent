#pragma once

namespace latent::render {

// IEC 61966-2-1 sRGB signal encoding/decoding. These functions deliberately
// require normalized display-linear/display-encoded inputs in [0, 1]; gamut
// mapping/clipping is a separate rendering operation and must not be hidden
// inside the transfer function.
[[nodiscard]] float encodeSrgb(float linear);
[[nodiscard]] float decodeSrgb(float encoded);

// SMPTE ST 2084 (PQ), full-range normalized code values. Luminance is absolute
// cd/m^2 (nits) in [0, 10000]. Tone mapping and target-peak decisions happen
// before this encoding step.
[[nodiscard]] float encodePqFromNits(float luminanceNits);
[[nodiscard]] float decodePqToNits(float encoded);

}  // namespace latent::render
