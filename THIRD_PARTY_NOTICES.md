# Third-party reference notices

Latent's core remains Apache-2.0. The following upstream projects are used as standards/reference material for clean, testable implementations.

## Academy Color Encoding System (ACES)

- Project: ACES Project / Academy Software Foundation
- Upstream: `aces-aswf/aces-core` and `aces-aswf/aces-output`
- License: Apache License 2.0
- Copyright: Contributors to the ACES Project
- Use in Latent: the scalar ACES 2 tonescale parameterization is transcribed in `src/render/AcesToneScale.cpp`; SMPTE ST 2084 constants are cross-checked against `Lib.Academy.DisplayEncoding.ctl`. Latent does **not** claim that these scalar primitives constitute the complete ACES 2 Output Transform.

## Colour - Demosaicing

- Project: `colour-science/colour-demosaicing`
- License: BSD-3-Clause
- Use in Latent: Malvar-He-Cutler reference/golden-vector validation. Latent's production Vulkan implementation is independent and differentially tested against its own reference semantics.

## libultrahdr

- Project: `google/libultrahdr`
- Validated version: v2.0.2
- License: Apache License 2.0 or MIT, at the consumer's option
- Use in Latent: optional external codec dependency behind `latent::codec`; Latent stages explicit Rec.709/sRGB SDR and BT.2020/PQ HDR renditions into the public C API formats, then delegates gain-map generation, ISO 21496 metadata, and JPEG/HEIF/AVIF container behavior to libultrahdr. The upstream source is not vendored or built inside Latent's CMake scope.

No GPL or proprietary implementation code from MotionCam, PhotonCamera, Google Camera, or Adobe Project Indigo is copied into the Latent core.
