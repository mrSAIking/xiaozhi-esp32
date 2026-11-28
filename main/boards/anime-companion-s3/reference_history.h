#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>

// Absolute speaker-frame positions keep silence, wraparound and missed blocks distinct.
struct ReferenceHistory {
    static constexpr int capacity = 8192;
    int16_t samples[capacity]{};
    int64_t begin = 0, end = 0;
    void append(int64_t first, const int32_t* pcm, size_t n) {
        if (first != end) begin = first;
        for (size_t i = 0; i < n; ++i) samples[(first + i) % capacity] = pcm[i] >> 16;
        end = first + n;
        begin = std::max(begin, end - capacity);
    }
    int16_t at(int64_t pos) const {
        return pos >= begin && pos < end ? samples[pos % capacity] : 0;
    }
    int16_t mono16(int64_t half_frame) const {
        auto pos = half_frame / 2;
        return (half_frame & 1) ? (int(at(pos)) + at(pos + 1)) / 2 : at(pos);
    }
};
