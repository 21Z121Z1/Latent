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
// Continuous, noise-weighted Lucas-Kanade refinement for period-averaged
// RAW guides. Rank-deficient (flat/aperture) windows are unobservable, not an
// identity-motion observation. Legacy alignment keeps its original behavior.
MotionTile refineContinuous(const Proxy& ref,const Proxy& src,Window window,MotionTile initial,
    float limit,float floor) {
    auto best=initial;auto old=cost(ref,src,window,best.dx,best.dy,floor);
    float conditionConfidence=0;
    for(unsigned iteration=0;iteration<8;++iteration) {
        double xx=0,xy=0,yy=0,bx=0,by=0,samples=0;
        const auto stride=std::max(1U,std::max(window.x1-window.x0,window.y1-window.y0)/48U);
        for(auto y=window.y0;y<window.y1;y+=stride)for(auto x=window.x0;x<window.x1;x+=stride) {
            const auto& r=ref.at(x,y);if(r.usable==0)continue;
            const float sx=static_cast<float>(x)+best.dx,sy=static_cast<float>(y)+best.dy;
            const auto value=interpolate(src,sx,sy),left=interpolate(src,sx-0.5F,sy),right=interpolate(src,sx+0.5F,sy);
            const auto up=interpolate(src,sx,sy-0.5F),down=interpolate(src,sx,sy+0.5F);
            if(!value||!left||!right||!up||!down)continue;
            const double delta=static_cast<double>(r.value)-value->value;
            const double variance=std::max(static_cast<double>(floor),static_cast<double>(r.variance)+value->variance);
            const double weight=1.0/(variance*std::max(1.0,std::abs(delta)/std::sqrt(variance)/3.0));
            const double gx=right->value-left->value,gy=down->value-up->value;
            xx+=weight*gx*gx;xy+=weight*gx*gy;yy+=weight*gy*gy;
            bx+=weight*gx*delta;by+=weight*gy*delta;samples+=1;
        }
        const double det=xx*yy-xy*xy,tr=xx+yy;
        if(samples<8 || tr/samples<1e-3 || det<=tr*tr*1e-4) {best.confidence=0;return best;}
        conditionConfidence=static_cast<float>(std::min(1.0,det/(tr*tr)*8));
        float dx=static_cast<float>(std::clamp((yy*bx-xy*by)/det,-0.5,0.5));
        float dy=static_cast<float>(std::clamp((xx*by-xy*bx)/det,-0.5,0.5));
        if(std::abs(dx)+std::abs(dy)<0.0005F)break;
        bool improved=false;
        for(unsigned trial=0;trial<5;++trial) {
            const float nx=std::clamp(best.dx+dx,-limit,limit),ny=std::clamp(best.dy+dy,-limit,limit);
            const auto next=cost(ref,src,window,nx,ny,floor);
            if(next.value<old.value) {best.dx=nx;best.dy=ny;old=next;improved=true;break;}
            dx*=0.5F;dy*=0.5F;
        }
        if(!improved)break;
    }
    best.residual=old.value;best.confidence=conditionConfidence*old.coverage/(1+0.25F*old.value);
    return best;
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

namespace {
AlignmentField alignProxies(Proxy reference, Proxy source, imaging::Extent rawExtent,
    float pixelStep, imaging::FrameId referenceId, imaging::FrameId sourceId, const TemporalPolicy& policy, bool continuous = false) {
    AlignmentField field{};
    field.source = sourceId; field.reference = referenceId; field.extent = rawExtent;
    field.tileSize = policy.tileSize;
    field.columns = (field.extent.width + field.tileSize - 1U) / field.tileSize;
    field.rows = (field.extent.height + field.tileSize - 1U) / field.tileSize;
    field.tiles.resize(static_cast<std::size_t>(field.columns) * field.rows);
    if (sourceId == referenceId) {
        field.global.confidence = 1;
        std::fill(field.tiles.begin(), field.tiles.end(), field.global);
        return field;
    }
    std::vector<Proxy> rp, sp;
    rp.push_back(std::move(reference)); sp.push_back(std::move(source));
    float scale = pixelStep;
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
    const float limit = static_cast<float>(policy.maximumDisplacement) / pixelStep;
    const auto& r = rp.front(); const auto& s = sp.front();
    global.residual = 1.0e20F;
    for (const auto& h : hypotheses) {
        const auto refined = search(r, s, {0, 0, r.width, r.height}, h, 4, 0.125F, limit, policy.varianceFloor);
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
            refined = search(r, s, {0, 0, r.width, r.height}, refined, 4, 0.125F, limit, policy.varianceFloor);
            if (refined.residual < global.residual) global = refined;
        }
    }
    if(continuous) global=refineContinuous(r,s,{0,0,r.width,r.height},global,limit,policy.varianceFloor);
    field.global = global; field.global.dx *= pixelStep; field.global.dy *= pixelStep;
    for (std::uint32_t ty = 0; ty < field.rows; ++ty) for (std::uint32_t tx = 0; tx < field.columns; ++tx) {
        const auto step = static_cast<std::uint32_t>(pixelStep);
        const auto x0 = std::min(r.width, tx * field.tileSize / step), y0 = std::min(r.height, ty * field.tileSize / step);
        const auto x1 = std::min(r.width, x0 + std::max(1U,field.tileSize / step));
        const auto y1 = std::min(r.height, y0 + std::max(1U,field.tileSize / step));
        auto tile = search(r, s, {x0, y0, x1, y1}, global, 2, 1.0F, limit, policy.varianceFloor);
        tile = search(r, s, {x0, y0, x1, y1}, tile, 4, 0.125F, limit, policy.varianceFloor);
        if(continuous) {
            tile=refineContinuous(r,s,{x0,y0,x1,y1},tile,limit,policy.varianceFloor);
            // Forward/backward consistency on the transported guide window.
            const auto clampX=[&](float x){return static_cast<std::uint32_t>(std::clamp(x,0.0F,static_cast<float>(s.width)));};
            const auto clampY=[&](float y){return static_cast<std::uint32_t>(std::clamp(y,0.0F,static_cast<float>(s.height)));};
            const Window reverseWindow{clampX(static_cast<float>(x0)+tile.dx),clampY(static_cast<float>(y0)+tile.dy),
                clampX(static_cast<float>(x1)+tile.dx),clampY(static_cast<float>(y1)+tile.dy)};
            const auto back=refineContinuous(s,r,reverseWindow,{-tile.dx,-tile.dy,1,0},limit,policy.varianceFloor);
            const float ex=(tile.dx+back.dx)*pixelStep,ey=(tile.dy+back.dy)*pixelStep;
            const float consistency=std::max(0.0F,1.0F-(ex*ex+ey*ey)/4.0F);
            tile.confidence=std::min(tile.confidence,back.confidence)*consistency*consistency;
        }
        tile.dx *= pixelStep; tile.dy *= pixelStep;
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
} // namespace
AlignmentField alignTemporalRaw(const NormalizedRaw& reference,const NormalizedRaw& source,
    imaging::FrameId referenceId,imaging::FrameId sourceId,const TemporalPolicy& policy) {
    validateTemporalPolicy(policy);
    if(source.extent!=reference.extent || source.cfa!=reference.cfa || reference.extent.width<2 || reference.extent.height<2 ||
       source.samples.size()!=source.extent.pixelCount() || reference.samples.size()!=source.samples.size())
        throw std::invalid_argument("alignment requires compatible sensor coordinates");
    return alignProxies(proxy(reference),proxy(source),reference.extent,2,referenceId,sourceId,policy);
}
AlignmentField alignRawGuides(const AlignmentGuide& reference,const AlignmentGuide& source,
    imaging::Extent rawExtent,imaging::FrameId referenceId,imaging::FrameId sourceId,const TemporalPolicy& policy) {
    validateTemporalPolicy(policy);
    if(reference.extent!=source.extent || reference.pixelStep!=source.pixelStep || reference.pixelStep==0 ||
       reference.samples.size()!=reference.extent.pixelCount() || source.samples.size()!=reference.samples.size() ||
       reference.extent.width<2 || reference.extent.height<2)
        throw std::invalid_argument("invalid RAW registration guide");
    return alignProxies({reference.extent.width,reference.extent.height,reference.samples},
        {source.extent.width,source.extent.height,source.samples},rawExtent,
        static_cast<float>(reference.pixelStep),referenceId,sourceId,policy,true);
}
}  // namespace latent::reference
