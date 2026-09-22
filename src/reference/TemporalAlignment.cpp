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
    Proxy p{(raw.extent.width + 1U) / 2U, (raw.extent.height + 1U) / 2U, {}};
    p.pixels.resize(static_cast<std::size_t>(p.width) * p.height);
    for (std::uint32_t y = 0; y < p.height; ++y) for (std::uint32_t x = 0; x < p.width; ++x) {
        auto& d = p.pixels[static_cast<std::size_t>(y) * p.width + x];
        float count = 0;
        bool complete = true;
        for (std::uint32_t j = 0; j < 2U; ++j) for (std::uint32_t i = 0; i < 2U; ++i) {
            const auto sx = x * 2U + i, sy = y * 2U + j;
            if (sx >= raw.extent.width || sy >= raw.extent.height) continue;
            const auto& s = raw.samples[static_cast<std::size_t>(sy) * raw.extent.width + sx];
            if (s.usable == 0) { complete = false; continue; }
            // Equal R/G0/G1/B cell aggregation: execution-only intensity proxy.
            d.value += s.value; d.variance += s.variance; count += 1.0F;
        }
        // Partial CFA clipping must not change the proxy's spectral mixture.
        d.usable = count > 0 && complete ? 1.0F : 0.0F;
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
// Public provenance: Wronski et al., Handheld Multi-Frame Super-Resolution
// (2019), section 4, motivates continuous Lucas-Kanade refinement. This
// bounded robust solver is independently derived; no implementation copied.
// Noise-weighted Lucas-Kanade refinement of a selected translation basin.
// The bounded line search accepts only a lower Huber objective. Geometry stays
// in proxy coordinates here; this is not a photometric/rendering operation.
MotionTile refine(const Proxy& ref, const Proxy& src, Window window,
                  MotionTile initial, float limit, float floor) {
    MotionTile best = initial;
    Cost objective = cost(ref,src,window,best.dx,best.dy,floor);
    const auto step = std::max(1U,std::max(window.x1-window.x0,window.y1-window.y0)/48U);
    for (unsigned iteration = 0; iteration < 12; ++iteration) {
        float xx=0, xy=0, yy=0, bx=0, by=0;
        for (auto y=window.y0; y<window.y1; y+=step) for (auto x=window.x0; x<window.x1; x+=step) {
            const auto& r=ref.at(x,y);
            const float sx=static_cast<float>(x)+best.dx, sy=static_cast<float>(y)+best.dy;
            if (r.usable==0 || sx<0 || sy<0 || sx>=static_cast<float>(src.width-1U) || sy>=static_cast<float>(src.height-1U)) continue;
            const auto ix=static_cast<std::uint32_t>(sx), iy=static_cast<std::uint32_t>(sy);
            const auto& a=src.at(ix,iy); const auto& b=src.at(ix+1U,iy);
            const auto& c=src.at(ix,iy+1U); const auto& d=src.at(ix+1U,iy+1U);
            if (a.usable==0 || b.usable==0 || c.usable==0 || d.usable==0) continue;
            const auto s=interpolate(src,sx,sy);
            const float fx=sx-static_cast<float>(ix), fy=sy-static_cast<float>(iy);
            const float gx=(b.value-a.value)*(1-fy)+(d.value-c.value)*fy;
            const float gy=(c.value-a.value)*(1-fx)+(d.value-b.value)*fx;
            const float delta=s->value-r.value, variance=r.variance+s->variance+floor;
            const float z=std::abs(delta)/std::sqrt(variance);
            const float weight=(z>3 ? 3/z : 1)/variance;
            xx+=weight*gx*gx; xy+=weight*gx*gy; yy+=weight*gy*gy;
            bx+=weight*gx*delta; by+=weight*gy*delta;
        }
        const float det=xx*yy-xy*xy, trace=xx+yy;
        // A rank-deficient/aperture-problem system cannot establish 2D motion.
        if (!std::isfinite(det) || det<=1.0e-6F*trace*trace || trace<=0) break;
        float dx=(xy*by-yy*bx)/det, dy=(xy*bx-xx*by)/det;
        const float magnitude=std::max(std::abs(dx),std::abs(dy));
        if (!std::isfinite(magnitude) || magnitude<1.0e-4F) break;
        const float bound=std::min(1.0F,0.25F/magnitude);
        dx*=bound; dy*=bound;
        bool accepted=false;
        for (unsigned line=0; line<8; ++line) {
            const float nx=std::clamp(best.dx+dx,std::max(-limit,initial.dx-1.0F),std::min(limit,initial.dx+1.0F));
            const float ny=std::clamp(best.dy+dy,std::max(-limit,initial.dy-1.0F),std::min(limit,initial.dy+1.0F));
            const auto candidate=cost(ref,src,window,nx,ny,floor);
            if (candidate.value<objective.value) {
                best.dx=nx; best.dy=ny; objective=candidate; accepted=true; break;
            }
            dx*=0.5F; dy*=0.5F;
        }
        if (!accepted) break;
    }
    best.residual=objective.value;
    best.confidence=objective.coverage/(1+0.25F*objective.value);
    return best;
}

MotionTile searchInteger(const Proxy& ref, const Proxy& src, Window window,
    MotionTile center, int radius, float limit, float floor) {
    MotionTile best = center;
    auto bestCost = cost(ref, src, window, center.dx, center.dy, floor);
    for (int j = -radius; j <= radius; ++j) for (int i = -radius; i <= radius; ++i) {
        const float dx = center.dx + static_cast<float>(i);
        const float dy = center.dy + static_cast<float>(j);
        if (std::abs(dx) > limit || std::abs(dy) > limit) continue;
        const auto c = cost(ref, src, window, dx, dy, floor);
        // Strict comparison preserves the center on a flat/tied cost surface.
        if (c.value < bestCost.value) { best.dx = dx; best.dy = dy; bestCost = c; }
    }
    best.residual = bestCost.value;
    best.confidence = bestCost.coverage / (1.0F + 0.25F * bestCost.value);
    return best;
}
// Noise-debiased reference gradient structure. This bounded score measures
// observed two-dimensional texture, not a calibrated probability/covariance.
float textureSupport(const Proxy& image, Window window) {
    float xx=0, xy=0, yy=0, noiseX=0, noiseY=0;
    for (auto y=std::max(1U,window.y0); y<std::min(image.height-1U,window.y1); ++y)
        for (auto x=std::max(1U,window.x0); x<std::min(image.width-1U,window.x1); ++x) {
            const auto& l=image.at(x-1U,y); const auto& r=image.at(x+1U,y);
            const auto& t=image.at(x,y-1U); const auto& b=image.at(x,y+1U);
            if (l.usable==0 || r.usable==0 || t.usable==0 || b.usable==0) continue;
            const float gx=(r.value-l.value)*0.5F, gy=(b.value-t.value)*0.5F;
            xx+=gx*gx; xy+=gx*gy; yy+=gy*gy;
            noiseX+=0.25F*(r.variance+l.variance); noiseY+=0.25F*(b.variance+t.variance);
        }
    xx-=noiseX; yy-=noiseY;
    const float trace=xx+yy;
    const float minimum=std::max(0.0F,0.5F*(trace-std::hypot(xx-yy,2*xy)));
    if (trace<=0 || minimum<=0) return 0;
    return (2*minimum/trace)*(minimum/(minimum+noiseX+noiseY));
}
float distinctGap(const Proxy& ref, const Proxy& src, Window window,
                  MotionTile best, std::span<const MotionTile> alternatives, float floor) {
    const auto optimum=cost(ref,src,window,best.dx,best.dy,floor);
    if (optimum.coverage==0) return 0;
    // Compare photometric terms, not overlap rewards, so a periodic alias cannot
    // become "unique" merely because its displacement exposes fewer borders.
    const float bestPhotometric=optimum.value-(1-optimum.coverage);
    float gap=1;
    bool compared=false;
    for (const auto& a:alternatives) {
        if (std::hypot(a.dx-best.dx,a.dy-best.dy)<1) continue;
        const auto c=cost(ref,src,window,a.dx,a.dy,floor);
        if (c.coverage==0) continue;
        compared=true;
        gap=std::min(gap,std::max(0.0F,(c.value-(1-c.coverage)-bestPhotometric)/(1+bestPhotometric)));
    }
    return compared ? gap : 0;
}
GeometryEvidence geometry(const Proxy& ref, const Proxy& src, Window window,
                          MotionTile motion, float gap, float limit, float floor) {
    GeometryEvidence result{};
    result.textureSupport=textureSupport(ref,window);
    result.distinctCostGap=gap;
    // Latent policy diagnostics: 5% texture/gap and a one-sensor-pixel cycle
    // gate. They are not measured sensor calibration or uncertainty quantiles.
    if (result.textureSupport<0.05F) result.issues|=GeometryIssue::Underconstrained;
    if (gap<0.05F) result.issues|=GeometryIssue::Ambiguous;
    const auto c=cost(ref,src,window,motion.dx,motion.dy,floor);
    if (c.coverage==0) { result.issues|=GeometryIssue::InsufficientOverlap; return result; }
    const auto shiftBound=[](std::uint32_t value,float shift,std::uint32_t bound) {
        return static_cast<std::uint32_t>(std::clamp(std::round(static_cast<float>(value)+shift),0.0F,static_cast<float>(bound)));
    };
    const Window reverseWindow{shiftBound(window.x0,motion.dx,src.width),shiftBound(window.y0,motion.dy,src.height),
        shiftBound(window.x1,motion.dx,src.width),shiftBound(window.y1,motion.dy,src.height)};
    auto backward=searchInteger(src,ref,reverseWindow,{-motion.dx,-motion.dy,0,0},1,limit,floor);
    backward=refine(src,ref,reverseWindow,backward,limit,floor);
    result.cycleErrorPixels=2*std::hypot(motion.dx+backward.dx,motion.dy+backward.dy);
    if (result.cycleErrorPixels>1) result.issues|=GeometryIssue::Inconsistent;
    if (backward.confidence==0) result.issues|=GeometryIssue::InsufficientOverlap;
    return result;
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
    for (const auto* image : {&reference,&source}) for (const auto& sample : image->samples) {
        if (!std::isfinite(sample.value) || !std::isfinite(sample.variance) || sample.variance<0 ||
            (sample.usable!=0 && sample.usable!=1)) throw std::invalid_argument("invalid normalized alignment observation");
    }
    AlignmentField field{};
    field.source = sourceId; field.reference = referenceId; field.extent = reference.extent;
    field.tileSize = policy.tileSize;
    field.columns = (field.extent.width + field.tileSize - 1U) / field.tileSize;
    field.rows = (field.extent.height + field.tileSize - 1U) / field.tileSize;
    field.tiles.resize(static_cast<std::size_t>(field.columns) * field.rows);
    field.tileEvidence.resize(field.tiles.size());
    if (sourceId == referenceId) {
        field.global.confidence = 1;
        std::fill(field.tiles.begin(), field.tiles.end(), field.global);
        field.globalEvidence = {1,0,1,GeometryIssue::Reference};
        std::fill(field.tileEvidence.begin(),field.tileEvidence.end(),field.globalEvidence);
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
    std::vector<MotionTile> alternatives;
    for (const auto& h : hypotheses) {
        const auto refined = refine(r, s, {0, 0, r.width, r.height}, h, limit, policy.varianceFloor);
        alternatives.push_back(refined);
        if (refined.residual < global.residual) global = refined;
    }
    if (global.confidence < policy.minimumAlignmentConfidence) {
        // Recovery searches a bounded, sparse proxy grid, never the full RAW.
        // This catches a coarse pyramid alias instead of trusting it silently.
        const std::array<MotionTile, 1> origin{};
        const auto recovery = beamSearch(r, s, origin, static_cast<int>(std::ceil(limit)),
                                         limit, policy.varianceFloor, 16U);
        for (const auto& h : recovery) {
            auto refined = searchInteger(r, s, {0, 0, r.width, r.height}, h, 1, limit, policy.varianceFloor);
            refined = refine(r, s, {0, 0, r.width, r.height}, refined, limit, policy.varianceFloor);
            alternatives.push_back(refined);
            if (refined.residual < global.residual) global = refined;
        }
    }
    const Window whole{0,0,r.width,r.height};
    const float gap=distinctGap(r,s,whole,global,alternatives,policy.varianceFloor);
    field.globalEvidence=geometry(r,s,whole,global,gap,limit,policy.varianceFloor);
    if ((field.globalEvidence.issues & GeometryIssue::Underconstrained) != 0) {
        global=searchInteger(r,s,whole,{},0,limit,policy.varianceFloor);
        field.globalEvidence=geometry(r,s,whole,global,gap,limit,policy.varianceFloor);
        field.globalEvidence.issues|=GeometryIssue::IdentityPrior;
    }
    field.global = global; field.global.dx *= 2.0F; field.global.dy *= 2.0F;
    for (std::uint32_t ty = 0; ty < field.rows; ++ty) for (std::uint32_t tx = 0; tx < field.columns; ++tx) {
        const auto x0 = tx * field.tileSize / 2U, y0 = ty * field.tileSize / 2U;
        const auto x1 = std::min(r.width, x0 + field.tileSize / 2U);
        const auto y1 = std::min(r.height, y0 + field.tileSize / 2U);
        auto tile = searchInteger(r, s, {x0, y0, x1, y1}, global, 2, limit, policy.varianceFloor);
        tile = refine(r, s, {x0, y0, x1, y1}, tile, limit, policy.varianceFloor);
        const auto index=static_cast<std::size_t>(ty)*field.columns+tx;
        auto evidence=geometry(r,s,{x0,y0,x1,y1},tile,gap,limit,policy.varianceFloor);
        if ((evidence.issues & GeometryIssue::Underconstrained) != 0) {
            tile=searchInteger(r,s,{x0,y0,x1,y1},global,0,limit,policy.varianceFloor);
            evidence=geometry(r,s,{x0,y0,x1,y1},tile,gap,limit,policy.varianceFloor);
            evidence.issues|=GeometryIssue::GlobalPrior;
        } else if ((evidence.issues & GeometryIssue::Inconsistent) != 0) {
            tile.confidence/=1+evidence.cycleErrorPixels*evidence.cycleErrorPixels;
        }
        tile.dx *= 2.0F; tile.dy *= 2.0F;
        field.tiles[index] = tile;
        field.tileEvidence[index] = evidence;
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
