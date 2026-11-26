#pragma once

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

// The separable polynomial is zero at each edge and peaks at the center.
inline float Displacement(float x, float y, float phase) {
    return 32.f * x * (1.f - x) * y * (1.f - y) * phase;
}

inline uint16_t Blend565(uint16_t a, uint16_t b, unsigned frac) {
    const auto mix = [frac](unsigned a, unsigned b) {
        return (a * (16 - frac) + b * frac + 8) >> 4;
    };
    return (mix(a >> 11, b >> 11) << 11) | (mix((a >> 5) & 63, (b >> 5) & 63) << 5) |
           mix(a & 31, b & 31);
}
}  // namespace avatar
