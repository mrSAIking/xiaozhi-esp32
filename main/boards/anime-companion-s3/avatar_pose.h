#pragma once
#include <cstring>
#include "avatar_math.h"

namespace avatar {
struct MotionConfig {
    int breath_ms, breath_px, left_ms, right_ms, arm_px;
};
struct Pose {
    float breath{}, arm[2]{};
};
inline Pose MakePose(int64_t ms, MotionConfig c) {
    constexpr float tau = 6.283185307f;
    const auto phase = [=](int period) { return tau * float(ms % period) / period; };
    const float t = float(ms % c.breath_ms) / c.breath_ms;
    const float breath =
        .5f * (1.f - std::cos((tau * .5f) * (t < .42f ? t / .42f : (1.f - t) / .58f)));
    Pose p;
    p.breath = breath * c.breath_px;
    p.arm[0] = c.arm_px * std::sin(phase(c.left_ms) + .12f * breath);
    p.arm[1] = c.arm_px * std::sin(phase(c.right_ms) + .69f - .12f * breath);
    return p;
}
struct Delta {
    int x, y;
};
inline Delta Offset(const Pose& p, int mode, int x, int y) {
    const auto torso = kBreathingRegions[mode];
    const float weight = p.breath * BreathY(float(y - torso.y) / (torso.h - 1));
    const auto axis = BreathX(float(x - torso.x) / (torso.w - 1));
    const int side = x >= 160;
    const float arm = ArmWeight(ArmRow(mode, y), side ? 319 - x : x);
    // Hold the braids where they overlap the moving bodice. Fade the guard
    // through neighboring fabric so it cannot introduce a hard seam.
    const float braid_x = mode ? 113.f - 35.f * SmoothStep((y - 190.f) / 142.f)
                               : 141.f - 14.f * SmoothStep((y - 68.f) / 62.f);
    const float dist = std::abs((side ? 319 - x : x) - braid_x);
    const float hair = 1.f - SmoothStep((dist - (mode ? 9.f : 4.f)) / (mode ? 10.f : 5.f));
    const float guard =
        1.f - hair * SmoothStep(((mode ? 389.f : 145.f) - y) / (mode ? 24.f : 12.f));
    const float dx = guard * (axis.spread * weight + p.arm[side] * arm);
    const float dy = guard * axis.lift * weight;
    return {int(std::round(dx * 256.f)), int(std::round(dy * 256.f))};
}

inline uint16_t SampleAt(const uint16_t* src, int qx, int qy) {
    qx = std::clamp(qx, 0, 319 * 256);
    qy = std::clamp(qy, 0, 479 * 256);
    const int fx = qx & 255, fy = qy & 255;
    src += (qy >> 8) * 320 + (qx >> 8);
    if (!(fx | fy))
        return *src;
    const int right = fx != 0, below = fy ? 320 : 0;
    const unsigned a = src[0], b = src[right], c = src[below], d = src[below + right];
    if (a == b && a == c && a == d)
        return a;
    const int w = (256 - fx) * (256 - fy), u = fx * (256 - fy), v = (256 - fx) * fy, z = fx * fy;
    auto mix = [&](int shift, unsigned mask) {
        return (((((a >> shift) & mask) * w + ((b >> shift) & mask) * u +
                  ((c >> shift) & mask) * v + ((d >> shift) & mask) * z + 32768) >>
                 16)
                << shift);
    };
    return mix(11, 31) | mix(5, 63) | mix(0, 31);
}
inline void Composite(uint16_t* src, Rect r, const uint16_t* patch) {
    for (int y = 0; y < r.h; ++y)
        std::memcpy(src + (r.y + y) * 320 + r.x, patch + y * r.w, r.w * sizeof(*src));
}
// Fixed 8-pixel lattice in canvas coordinates, including when rendering only a facial patch.
inline Rect RenderRegion(const uint16_t* src, uint16_t* frame, int mode, const Pose& p, Rect r) {
    int left = 320, top = 480, right = -1, bottom = -1;
    for (int ty = r.y & ~7; ty < r.y + r.h; ty += 8) {
        Delta a = Offset(p, mode, r.x & ~7, ty), c = Offset(p, mode, r.x & ~7, ty + 8);
        for (int tx = r.x & ~7; tx < r.x + r.w; tx += 8) {
            const Delta b = Offset(p, mode, tx + 8, ty), d = Offset(p, mode, tx + 8, ty + 8);
            const bool stationary = !(a.x | a.y | b.x | b.y | c.x | c.y | d.x | d.y);
            for (int y = std::max(r.y, ty); y < std::min(ty + 8, r.y + r.h); ++y) {
                const int j = y - ty;
                const int lx = 8 * a.x + j * (c.x - a.x), ly = 8 * a.y + j * (c.y - a.y);
                const int sx = 8 * b.x + j * (d.x - b.x) - lx, sy = 8 * b.y + j * (d.y - b.y) - ly;
                const int start = std::max(tx, r.x), end = std::min(tx + 8, r.x + r.w);
                if (stationary && std::memcmp(src + y * 320 + start, frame + y * 320 + start,
                                              (end - start) * sizeof(*src)) == 0)
                    continue;
                int dx = 8 * lx + (start - tx) * sx, dy = 8 * ly + (start - tx) * sy;
                for (int x = start; x < end; ++x, dx += sx, dy += sy) {
                    const int index = y * 320 + x;
                    const uint16_t value = SampleAt(src, 256 * x + (dx >> 6), 256 * y + (dy >> 6));
                    if (frame[index] == value)
                        continue;
                    frame[index] = value;
                    left = std::min(left, x);
                    right = std::max(right, x);
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
            }
            a = b;
            c = d;
        }
    }
    return {left, top, right - left + 1, bottom - top + 1};
}
// Fixed world-space strips keep a complete pose coherent across short LVGL callbacks.
inline Rect BodyRegion(int mode, int index) {
    const int first = mode ? 208 : 72, split = mode ? 344 : 184, end = mode ? 480 : 280;
    const int rows = 8, upper = (split - first) / rows;
    if (index < upper) {
        const int x = mode ? 0 : 80;
        return {x, first + index * rows, 320 - 2 * x, rows};
    }
    const int part = index - upper, y = split + part / 2 * rows;
    if (y >= end)
        return {};
    const int x = mode ? 0 : 72, w = mode ? 88 : 56;
    return {part % 2 ? 320 - x - w : x, y, w, std::min(rows, end - y)};
}
template <class Visitor>
void PoseRegions(int mode, Visitor visit) {
    for (int i = 0;; ++i) {
        auto r = BodyRegion(mode, i);
        if (!r.h)
            return;
        visit(r);
    }
}
}  // namespace avatar
