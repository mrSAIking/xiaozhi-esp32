#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace avatar {
struct Rect {
    int x, y, w, h;
};
struct Mode {
    int base;
    Rect eyes;
    int eye_offsets[3];
    Rect mouth;
    int mouth_offsets[5];
    Rect torso;
    int holds[5];
    int interval[2];
    float thresholds[3];
    int speech_ms;
};

inline void Expand565(uint16_t pixel, uint8_t* out) {
    const unsigned r = pixel >> 11;
    const unsigned b = pixel & 31;
    out[0] = ((r << 1) | (r >> 4)) << 2;
    out[1] = ((pixel >> 5) & 63) << 2;
    out[2] = ((b << 1) | (b >> 4)) << 2;
}

inline bool FitsRgb565Buffer(int width, int rows, size_t stride, size_t capacity) {
    return width > 0 && rows > 0 && stride >= static_cast<size_t>(width) * 2 &&
           stride % alignof(uint16_t) == 0 && static_cast<size_t>(rows) <= capacity / stride;
}

inline int EyeFrame(int elapsed, const int* holds) {
    constexpr int sequence[] = {0, 1, 2, 1, 0};
    for (int i = 0; i < 5; ++i) {
        if (elapsed < holds[i])
            return sequence[i];
        elapsed -= holds[i];
    }
    return 0;
}

inline int MouthFrame(float rms, const float* thresholds) {
    return (rms >= thresholds[0]) + (rms >= thresholds[1]) + (rms >= thresholds[2]);
}

// Bounding boxes for the upper torso in the two fixed production views.
// Leave the face, upper hair and the full-body skirt hem outside the warp.
inline constexpr Rect kBreathingRegions[] = {{97, 75, 126, 103}, {14, 213, 292, 128}};

inline int BreathAmount(int64_t elapsed_ms, int period_ms, int amplitude_px) {
    const int inhale_ms = period_ms * 42 / 100;
    const int cycle_ms = elapsed_ms % period_ms;
    const float progress = cycle_ms < inhale_ms
                               ? float(cycle_ms) / inhale_ms
                               : float(period_ms - cycle_ms) / (period_ms - inhale_ms);
    return int(32.f * amplitude_px * (1.f - std::cos(3.14159265f * progress)) + .5f);
}

inline float SmoothStep(float value) {
    const float t = std::clamp(value, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

struct BreathAxis {
    float spread;
    float lift;
};

inline BreathAxis BreathX(float x) {
    const float width = x * 2.f - 1.f;
    const float edge = SmoothStep(std::min(x, 1.f - x) / .105f);
    // Inverse sampling expands the silhouette and lifts the shoulders.
    return {-.52f * width * edge, (1.f - .17f * std::abs(width)) * edge};
}

inline float BreathY(float y) { return SmoothStep(y / .22f) * SmoothStep((1.f - y) / .65f); }

// Offsets are in sixteenths of a pixel. Interpolate from the immutable source
// and round RGB channels only once, so repeated frames never accumulate error.
inline uint16_t Sample565(const uint16_t* pixel, int stride, int dx, int dy) {
    const int x = (dx < 0 ? dx - 15 : dx) / 16;
    const int y = dy / 16;
    const unsigned fx = dx - x * 16;
    const unsigned fy = dy - y * 16;
    pixel += y * stride + x;
    if (!(fx || fy))
        return *pixel;
    const unsigned right = fx ? 1 : 0;
    const unsigned below = fy ? stride : 0;
    const uint16_t a = pixel[0], b = pixel[right], c = pixel[below], d = pixel[below + right];
    const unsigned wa = (16 - fx) * (16 - fy), wb = fx * (16 - fy);
    const unsigned wc = (16 - fx) * fy, wd = fx * fy;
    const auto mix = [=](unsigned shift, unsigned mask) {
        return ((((a >> shift) & mask) * wa + ((b >> shift) & mask) * wb +
                 ((c >> shift) & mask) * wc + ((d >> shift) & mask) * wd + 128) >>
                8)
               << shift;
    };
    return mix(11, 31) | mix(5, 63) | mix(0, 31);
}
}  // namespace avatar
