#include "latent/reference/TemporalReconstruct.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace latent::reference {
namespace {
struct Proxy {
    std::uint32_t width = 0, height = 0;
    std::vector<TemporalSample> pixels;
    const TemporalSample& at(std::uint32_t x, std::uint32_t y) const {
        return pixels[static_cast<std::size_t>(y) * width + x];
    }
};
Proxy proxy(const NormalizedRaw& raw) {
    Proxy p{std::max(1U, raw.extent.width / 2U), std::max(1U, raw.extent.height / 2U), {}};
    p.pixels.resize(static_cast<std::size_t>(p.width) * p.height);
    for (std::uint32_t y = 0; y < p.height; ++y) for (std::uint32_t x = 0; x < p.width; ++x) {
        auto& d = p.pixels[static_cast<std::size_t>(y) * p.width + x];
        float count = 0;
        for (std::uint32_t j = 0; j < 2U; ++j) for (std::uint32_t i = 0; i < 2U; ++i) {
            const auto sx = x * 2U + i, sy = y * 2U + j;
            if (sx >= raw.extent.width || sy >= raw.extent.height) continue;
            const auto& s = raw.samples[static_cast<std::size_t>(sy) * raw.extent.width + sx];
            if (s.usable == 0) continue;
            // Equal R/G0/G1/B cell aggregation: execution-only intensity proxy.
            d.value += s.value; d.variance += s.variance; count += 1.0F;
        }
        d.usable = count > 0 ? 1.0F : 0.0F;
        if (count > 0) { d.value /= count; d.variance /= count * count; }
    }
    return p;
}
Proxy downsample(const Proxy& input) {
    Proxy p{(input.width + 1U) / 2U, (input.height + 1U) / 2U, {}};
    p.pixels.resize(static_cast<std::size_t>(p.width) * p.height);
    // Separable binomial [1 2 1] Gaussian approximation. Truncated real taps at
    // edges avoid treating replicated samples as independent observations.
    for (std::uint32_t y = 0; y < p.height; ++y) for (std::uint32_t x = 0; x < p.width; ++x) {
        auto& d = p.pixels[static_cast<std::size_t>(y) * p.width + x];
        float sum = 0;
        for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
            const int sx = static_cast<int>(x * 2U) + i, sy = static_cast<int>(y * 2U) + j;
            if (sx < 0 || sy < 0 || sx >= static_cast<int>(input.width) || sy >= static_cast<int>(input.height)) continue;
            const auto& s = input.at(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy));
            if (s.usable == 0) continue;
            const float w = (i == 0 ? 2.0F : 1.0F) * (j == 0 ? 2.0F : 1.0F);
            d.value += w * s.value; d.variance += w * w * s.variance; sum += w;
        }
        d.usable = sum > 0 ? 1.0F : 0.0F;
        if (sum > 0) { d.value /= sum; d.variance /= sum * sum; }
    }
    return p;
}
std::optional<TemporalSample> interpolate(const Proxy& p, float x, float y) {
    if (x < 0 || y < 0 || x > static_cast<float>(p.width - 1U) || y > static_cast<float>(p.height - 1U)) return {};
    const auto ix = static_cast<std::uint32_t>(x), iy = static_cast<std::uint32_t>(y);
    const float fx = x - static_cast<float>(ix), fy = y - static_cast<float>(iy);
    TemporalSample out{};
    for (std::uint32_t j = 0; j < 2; ++j) for (std::uint32_t i = 0; i < 2; ++i) {
        const float w = (i == 0 ? 1.0F - fx : fx) * (j == 0 ? 1.0F - fy : fy);
        if (w == 0) continue;
        const auto& s = p.at(ix + i, iy + j);
        if (s.usable == 0) return {};
        out.value += w * s.value; out.variance += w * w * s.variance;
    }
    return out;
}
struct DifferentialSample {
    TemporalSample sample{};
    float gx = 0, gy = 0;
    float noiseXX = 0, noiseXY = 0, noiseYY = 0;
};
// Cardinal cubic interpolation is only a registration guide. It does not
// resample RAW for fusion, clip negative values, or create sensor observations.
std::optional<DifferentialSample> cubic(const Proxy& p, float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y) || p.width < 4U || p.height < 4U || x < 1.0F || y < 1.0F ||
        x >= static_cast<float>(p.width - 2U) || y >= static_cast<float>(p.height - 2U)) return {};
    const auto ix = static_cast<std::uint32_t>(x), iy = static_cast<std::uint32_t>(y);
    const auto coefficients = [](float t) {
        const float t2 = t*t, t3 = t2*t;
        return std::array<float, 4>{-0.5F*t+t2-0.5F*t3, 1.0F-2.5F*t2+1.5F*t3,
                                   0.5F*t+2.0F*t2-1.5F*t3, -0.5F*t2+0.5F*t3};
    };
    const auto derivatives = [](float t) {
        const float t2 = t*t;
        return std::array<float, 4>{-0.5F+2.0F*t-1.5F*t2, -5.0F*t+4.5F*t2,
                                   0.5F+4.0F*t-4.5F*t2, -t+1.5F*t2};
    };
    const float fx = x-static_cast<float>(ix), fy = y-static_cast<float>(iy);
    const auto wx = coefficients(fx), wy = coefficients(fy);
    const auto gx = derivatives(fx), gy = derivatives(fy);
    DifferentialSample out{};
    for (std::uint32_t j = 0; j < 4U; ++j) for (std::uint32_t i = 0; i < 4U; ++i) {
        const auto& v = p.at(ix+i-1U, iy+j-1U);
        if (v.usable == 0) return {};
        const float w = wx[i]*wy[j];
        out.sample.value += w*v.value; out.sample.variance += w*w*v.variance;
        const float derivativeX = gx[i]*wy[j], derivativeY = wx[i]*gy[j];
        out.gx += derivativeX*v.value; out.gy += derivativeY*v.value;
        out.noiseXX += derivativeX*derivativeX*v.variance;
        out.noiseXY += derivativeX*derivativeY*v.variance;
        out.noiseYY += derivativeY*derivativeY*v.variance;
    }
    return out;
}
struct Window { std::uint32_t x0, y0, x1, y1; };
struct Cost { float value = 1.0e20F, coverage = 0; };
Cost cost(const Proxy& ref, const Proxy& src, Window window, float dx, float dy, float floor, std::uint32_t density = 48U) {
    float sum = 0, count = 0, possible = 0;
    const auto step = std::max(1U, std::max(window.x1 - window.x0, window.y1 - window.y0) / density);
    for (auto y = window.y0; y < window.y1; y += step) for (auto x = window.x0; x < window.x1; x += step) {
        const auto& r = ref.at(x, y);
        if (r.usable == 0) continue;
        possible += 1.0F;
        const auto s = interpolate(src, static_cast<float>(x) + dx, static_cast<float>(y) + dy);
        if (!s) continue;
        const float delta = r.value - s->value;
        const float z2 = delta * delta / (r.variance + s->variance + floor);
        // Huber growth preserves discrimination at coarse levels. A capped
        // cost can make every candidate identical when sampling error exceeds noise.
        sum += z2 <= 9.0F ? z2 : 6.0F * std::sqrt(z2) - 9.0F;
        count += 1.0F;
    }
    if (count < 4.0F || count < possible * 0.5F) return {};
    return {sum / count + (1.0F - count / possible), count / possible};
}
MotionTile search(const Proxy& ref, const Proxy& src, Window window,
    MotionTile center, int radius, float step, float limit, float floor) {
    MotionTile best = center;
    auto bestCost = cost(ref, src, window, center.dx, center.dy, floor);
    for (int j = -radius; j <= radius; ++j) for (int i = -radius; i <= radius; ++i) {
        const float dx = center.dx + static_cast<float>(i) * step;
        const float dy = center.dy + static_cast<float>(j) * step;
        if (std::abs(dx) > limit || std::abs(dy) > limit) continue;
        const auto c = cost(ref, src, window, dx, dy, floor);
        // Strict comparison preserves the center on a flat/tied cost surface.
        if (c.value < bestCost.value) { best.dx = dx; best.dy = dy; bestCost = c; }
    }
    best.residual = bestCost.value;
    best.confidence = bestCost.coverage / (1.0F + 0.25F * bestCost.value);
    return best;
}
struct Linearization {
    float xx = 0, xy = 0, yy = 0, bx = 0, by = 0;
    float noiseXX = 0, noiseXY = 0, noiseYY = 0, noisePowerSquared = 0;
    float cost = 1.0e20F, coverage = 0;
    std::uint32_t count = 0;
};
Linearization linearize(const Proxy& ref, const Proxy& src, Window window,
                        float dx, float dy, float floor) {
    Linearization out{};
    float loss = 0, possible = 0;
    const auto step = std::max(1U, std::max(window.x1-window.x0, window.y1-window.y0) / 48U);
    // Compare the same guide footprint in both images. The cubic halo is not
    // missing scene visibility and must not reduce identical-frame confidence.
    for (auto y = std::max(1U, window.y0); y+2U < ref.height && y < window.y1; y += step)
        for (auto x = std::max(1U, window.x0); x+2U < ref.width && x < window.x1; x += step) {
            const auto& r = ref.at(x,y);
            if (r.usable == 0) continue;
            possible += 1.0F;
            const auto s = cubic(src, static_cast<float>(x)+dx, static_cast<float>(y)+dy);
            if (!s) continue;
            const float variance = r.variance+s->sample.variance+floor;
            const float delta = r.value-s->sample.value;
            const float z2 = delta*delta/variance;
            const float robust = z2 <= 9.0F ? 1.0F : 3.0F/std::sqrt(z2);
            const float weight = robust/variance;
            out.xx += weight*s->gx*s->gx; out.xy += weight*s->gx*s->gy;
            out.yy += weight*s->gy*s->gy;
            out.bx += weight*s->gx*delta; out.by += weight*s->gy*delta;
            out.noiseXX += weight*s->noiseXX; out.noiseXY += weight*s->noiseXY; out.noiseYY += weight*s->noiseYY;
            const float noisePower = weight*(s->noiseXX+s->noiseYY);
            out.noisePowerSquared += noisePower*noisePower;
            loss += z2 <= 9.0F ? z2 : 6.0F*std::sqrt(z2)-9.0F;
            ++out.count;
        }
    if (out.count >= 4U && static_cast<float>(out.count) >= possible*0.5F) {
        out.coverage = static_cast<float>(out.count)/possible;
        out.cost = loss/static_cast<float>(out.count) + 1.0F-out.coverage;
    }
    return out;
}
MotionTile refineContinuous(const Proxy& ref, const Proxy& src, Window window,
                            MotionTile motion, float limit, float floor) {
    auto state = linearize(ref, src, window, motion.dx, motion.dy, floor);
    if (state.coverage == 0) return motion;
    // Conditioned two-parameter robust Gauss-Newton with a bounded descent
    // step. The coarse hypotheses resolve large motion; this stage removes
    // quarter-pixel quantization, not aperture/periodic-texture ambiguity.
    for (int iteration = 0; iteration < 12; ++iteration) {
        const float determinant = state.xx*state.yy-state.xy*state.xy;
        const float trace = state.xx+state.yy;
        if (!(determinant > 1.0e-6F*trace*trace) || !std::isfinite(determinant)) break;
        float ux = (state.yy*state.bx-state.xy*state.by)/determinant;
        float uy = (state.xx*state.by-state.xy*state.bx)/determinant;
        const float magnitude = std::max(std::abs(ux),std::abs(uy));
        if (!std::isfinite(magnitude) || magnitude < 1.0e-5F) break;
        if (magnitude > 0.5F) { ux *= 0.5F/magnitude; uy *= 0.5F/magnitude; }
        bool accepted = false;
        for (int backtrack = 0; backtrack < 6; ++backtrack) {
            const float dx = std::clamp(motion.dx+ux, -limit, limit);
            const float dy = std::clamp(motion.dy+uy, -limit, limit);
            const auto candidate = linearize(ref, src, window, dx, dy, floor);
            if (candidate.cost < state.cost && static_cast<float>(candidate.count) >= 0.95F*static_cast<float>(state.count)) {
                motion.dx = dx; motion.dy = dy; state = candidate; accepted = true; break;
            }
            ux *= 0.5F; uy *= 0.5F;
        }
        if (!accepted) break;
    }
    motion.residual = state.cost;
    motion.confidence = state.coverage/(1.0F+0.25F*state.cost);
    return motion;
}
RegistrationEvidence geometricEvidence(const Proxy& ref, const Proxy& src, Window window,
    MotionTile motion, float limit, float floor, bool ambiguous) {
    const auto state = linearize(ref, src, window, motion.dx, motion.dy, floor);
    RegistrationEvidence out{}; out.supportedGuideSamples = state.count;
    // Subtract expected gradient-noise energy before declaring geometry
    // observable. A flat noisy field is not a textured scene. The conservative
    // margin accounts for Gaussian quadratic-energy fluctuations and reuse;
    // this is a policy gate, not a measured posterior probability.
    const float margin = 3.0F*std::sqrt(32.0F*state.noisePowerSquared);
    const float xx = state.xx-state.noiseXX-margin, yy = state.yy-state.noiseYY-margin;
    const float xy = state.xy-state.noiseXY;
    const float trace = xx+yy;
    const float determinant = xx*yy-xy*xy;
    if (state.coverage == 0 || xx <= 0 || yy <= 0 || !(determinant > 1.0e-6F*trace*trace)) return out;
    const float largest = 0.5F*(trace+std::hypot(xx-yy,2.0F*xy));
    const float smallest = determinant/largest;
    // Each source proxy contributes to at most 16 residuals; the factor 16
    // upper-bounds reuse for fixed weights/gradients. Native pixel scale is 2.
    out.localizationStdDevPixels = 8.0F/std::sqrt(smallest);
    const auto shiftBound = [](std::uint32_t v, float shift, std::uint32_t bound) {
        return static_cast<std::uint32_t>(std::clamp(static_cast<float>(v)+shift, 0.0F, static_cast<float>(bound)));
    };
    const Window reverseWindow{shiftBound(window.x0, motion.dx, src.width),
        shiftBound(window.y0, motion.dy, src.height), shiftBound(window.x1, motion.dx, src.width),
        shiftBound(window.y1, motion.dy, src.height)};
    const auto reverse = refineContinuous(src, ref, reverseWindow,
        {-motion.dx, -motion.dy, motion.confidence, motion.residual}, limit, floor);
    const auto reverseState = linearize(src, ref, reverseWindow, reverse.dx, reverse.dy, floor);
    if (reverseState.coverage == 0) return out;
    out.cycleErrorPixels = 2.0F*std::hypot(motion.dx+reverse.dx, motion.dy+reverse.dy);
    out.status = ambiguous ? GeometryStatus::Ambiguous :
        (out.cycleErrorPixels > 0.5F ? GeometryStatus::Inconsistent : GeometryStatus::Estimated);
    return out;
}
// Keep spatially distinct hypotheses through ambiguous coarse levels. A
// single winning translation can lock onto a different texture period.
std::vector<MotionTile> beamSearch(const Proxy& ref, const Proxy& src,
    std::span<const MotionTile> centers, int radius, float limit, float floor, std::uint32_t density = 48U) {
    std::vector<MotionTile> candidates;
    for (const auto& center : centers) {
        for (int y = -radius; y <= radius; ++y) for (int x = -radius; x <= radius; ++x) {
            const float dx = center.dx + static_cast<float>(x), dy = center.dy + static_cast<float>(y);
            if (std::abs(dx) > limit || std::abs(dy) > limit) continue;
            const auto c = cost(ref, src, {0, 0, ref.width, ref.height}, dx, dy, floor, density);
            candidates.push_back({dx, dy, c.coverage / (1.0F + 0.25F * c.value), c.value});
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.residual != b.residual) return a.residual < b.residual;
        return a.dx*a.dx + a.dy*a.dy < b.dx*b.dx + b.dy*b.dy;
    });
    std::vector<MotionTile> selected;
    for (const auto& c : candidates) {
        const bool distinct = std::none_of(selected.begin(), selected.end(), [&](const auto& a) {
            const float dx = a.dx-c.dx, dy=a.dy-c.dy;
            return dx*dx + dy*dy < 4.0F;
        });
        if (distinct) selected.push_back(c);
        if (selected.size() == 8U) break;
    }
    // Zero motion remains a candidate, not an assumed observation.
    selected.push_back({});
    return selected;
}
}  // namespace

AlignmentField alignTemporalRaw(const NormalizedRaw& reference, const NormalizedRaw& source,
    imaging::FrameId referenceId, imaging::FrameId sourceId, const TemporalPolicy& policy) {
    validateTemporalPolicy(policy);
    if ((source.extent.width != reference.extent.width || source.extent.height != reference.extent.height) || source.cfa != reference.cfa ||
        reference.extent.width < 2U || reference.extent.height < 2U ||
        source.samples.size() != source.extent.pixelCount() || reference.samples.size() != source.samples.size()) {
        throw std::invalid_argument("alignment requires compatible sensor coordinates");
    }
    if (!referenceId.valid() || !sourceId.valid()) throw std::invalid_argument("alignment requires valid frame identities");
    const auto validSample = [](const TemporalSample& sample) {
        return std::isfinite(sample.value) && std::isfinite(sample.variance) && sample.variance >= 0 &&
            (sample.usable == 0 || sample.usable == 1);
    };
    if (!std::all_of(reference.samples.begin(), reference.samples.end(), validSample) ||
        !std::all_of(source.samples.begin(), source.samples.end(), validSample))
        throw std::invalid_argument("alignment requires finite value/variance/validity evidence");
    AlignmentField field{};
    field.source = sourceId; field.reference = referenceId; field.extent = reference.extent;
    field.tileSize = policy.tileSize;
    field.columns = (field.extent.width + field.tileSize - 1U) / field.tileSize;
    field.rows = (field.extent.height + field.tileSize - 1U) / field.tileSize;
    field.tiles.resize(static_cast<std::size_t>(field.columns) * field.rows);
    field.evidence.resize(field.tiles.size());
    if (sourceId == referenceId) {
        field.global.confidence = 1;
        std::fill(field.tiles.begin(), field.tiles.end(), field.global);
        field.globalEvidence = {0, 0, 0, GeometryStatus::Reference};
        std::fill(field.evidence.begin(), field.evidence.end(), field.globalEvidence);
        return field;
    }
    std::vector<Proxy> rp, sp;
    rp.push_back(proxy(reference)); sp.push_back(proxy(source));
    float scale = 2.0F;
    while (std::min(rp.back().width, rp.back().height) >= 16U &&
           static_cast<float>(policy.maximumDisplacement) / scale > 4.0F) {
        rp.push_back(downsample(rp.back())); sp.push_back(downsample(sp.back())); scale *= 2.0F;
    }
    MotionTile global{};
    std::vector<MotionTile> hypotheses(1);
    for (std::size_t level = rp.size(); level-- > 0U;) {
        const auto& r = rp[level]; const auto& s = sp[level];
        const float limit = static_cast<float>(policy.maximumDisplacement) / scale;
        const int radius = level + 1U == rp.size() ? static_cast<int>(std::ceil(limit)) : 2;
        hypotheses = beamSearch(r, s, hypotheses, radius, limit, policy.varianceFloor);
        if (level != 0U) {
            for (auto& h : hypotheses) { h.dx *= 2.0F; h.dy *= 2.0F; }
            scale *= 0.5F;
        }
    }
    const float limit = static_cast<float>(policy.maximumDisplacement) * 0.5F;
    const auto& r = rp.front(); const auto& s = sp.front();
    global.residual = 1.0e20F;
    for (const auto& h : hypotheses) {
        const auto refined = refineContinuous(r, s, {0, 0, r.width, r.height}, h, limit, policy.varianceFloor);
        if (refined.residual < global.residual) global = refined;
    }
    if (global.confidence < policy.minimumAlignmentConfidence) {
        // Recovery searches a bounded, sparse proxy grid, never the full RAW.
        // This catches a coarse pyramid alias instead of trusting it silently.
        const std::array<MotionTile, 1> origin{};
        const auto recovery = beamSearch(r, s, origin, static_cast<int>(std::ceil(limit)),
                                         limit, policy.varianceFloor, 16U);
        for (const auto& h : recovery) {
            auto refined = search(r, s, {0, 0, r.width, r.height}, h, 1, 1.0F, limit, policy.varianceFloor);
            refined = refineContinuous(r, s, {0, 0, r.width, r.height}, refined, limit, policy.varianceFloor);
            if (refined.residual < global.residual) global = refined;
        }
    }
    global = refineContinuous(r, s, {0, 0, r.width, r.height}, global, limit, policy.varianceFloor);
    // Test distinct retained hypotheses for a photometrically indistinguishable
    // alternative. This detects ambiguity within the searched set; it does not
    // certify uniqueness outside that set. Missing border support is not texture.
    const auto bestState = linearize(r, s, {0, 0, r.width, r.height}, global.dx, global.dy, policy.varianceFloor);
    const float bestLoss = bestState.cost-(1.0F-bestState.coverage);
    bool ambiguous = false;
    for (const auto& h : hypotheses) {
        if (std::hypot(h.dx-global.dx, h.dy-global.dy) < 2.0F) continue;
        const auto other = refineContinuous(r, s, {0, 0, r.width, r.height}, h, limit, policy.varianceFloor);
        if (std::hypot(other.dx-global.dx, other.dy-global.dy) < 2.0F) continue;
        const auto otherState = linearize(r, s, {0, 0, r.width, r.height}, other.dx, other.dy, policy.varianceFloor);
        const float tolerance = 3.0F*std::sqrt(2.0F/static_cast<float>(std::max(1U, std::min(bestState.count, otherState.count))));
        if (otherState.coverage > 0 && otherState.cost-(1.0F-otherState.coverage) <= bestLoss+tolerance) ambiguous = true;
    }
    field.globalEvidence = geometricEvidence(r, s, {0, 0, r.width, r.height}, global, limit, policy.varianceFloor, ambiguous);
    field.global = global; field.global.dx *= 2.0F; field.global.dy *= 2.0F;
    for (std::uint32_t ty = 0; ty < field.rows; ++ty) for (std::uint32_t tx = 0; tx < field.columns; ++tx) {
        const auto halfTile = field.tileSize / 2U;
        const auto x0 = std::min(tx * halfTile, r.width > halfTile ? r.width-halfTile : 0U);
        const auto y0 = std::min(ty * halfTile, r.height > halfTile ? r.height-halfTile : 0U);
        const auto x1 = std::min(r.width, x0 + field.tileSize / 2U);
        const auto y1 = std::min(r.height, y0 + field.tileSize / 2U);
        auto tile = search(r, s, {x0, y0, x1, y1}, global, 2, 1.0F, limit, policy.varianceFloor);
        tile = refineContinuous(r, s, {x0, y0, x1, y1}, tile, limit, policy.varianceFloor);
        const auto ti = static_cast<std::size_t>(ty)*field.columns+tx;
        field.evidence[ti] = geometricEvidence(r, s, {x0, y0, x1, y1}, tile, limit, policy.varianceFloor, ambiguous);
        if (field.evidence[ti].status == GeometryStatus::Inconsistent) {
            const float error = field.evidence[ti].cycleErrorPixels;
            tile.confidence /= 1.0F+error*error;
        }
        tile.dx *= 2.0F; tile.dy *= 2.0F;
        field.tiles[static_cast<std::size_t>(ty) * field.columns + tx] = tile;
    }
    // Conservative spatial-consistency confidence. Never smooth displacements
    // across a motion boundary; decrease trust instead of inventing a warp.
    const auto original = field.tiles;
    for (std::uint32_t y = 0; y < field.rows; ++y) for (std::uint32_t x = 0; x < field.columns; ++x) {
        const auto i = static_cast<std::size_t>(y) * field.columns + x;
        float mismatch = 0, neighbors = 0;
        for (const auto& [dx, dy] : {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1}, std::pair{0, 1}}) {
            const int nx = static_cast<int>(x) + dx, ny = static_cast<int>(y) + dy;
            if (nx < 0 || ny < 0 || nx >= static_cast<int>(field.columns) || ny >= static_cast<int>(field.rows)) continue;
            const auto& n = original[static_cast<std::size_t>(ny) * field.columns + static_cast<std::size_t>(nx)];
            const float ddx = n.dx - original[i].dx, ddy = n.dy - original[i].dy;
            mismatch += std::min(16.0F, ddx * ddx + ddy * ddy); neighbors += 1.0F;
        }
        field.tiles[i].confidence /= 1.0F + 0.25F * mismatch / std::max(neighbors, 1.0F);
    }
    return field;
}
}  // namespace latent::reference
