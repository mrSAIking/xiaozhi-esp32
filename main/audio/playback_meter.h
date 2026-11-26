#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>

// Mono decoded PCM, after output resampling. Only the output task publishes.
// A busy reader can cost one visual update, but never holds up I2S.
class PlaybackMeter {
public:
    void Submit(const int16_t* pcm, size_t count, int rate, int64_t now_us, int64_t dma_us,
                uint32_t generation) {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock || generation != generation_ || rate <= 0)
            return;
        const size_t window = std::max(1, rate / 20);
        auto start = std::max(next_us_, now_us + dma_us);
        while (count) {
            const size_t n = std::min(window, count);
            uint64_t energy = 0;
            for (size_t i = 0; i < n; ++i) {
                const int32_t sample = pcm[i];
                energy += static_cast<uint32_t>(sample * sample);
            }
            const float rms = std::sqrt(static_cast<float>(energy) / n) / 32768.f;
            const int64_t end = start + static_cast<int64_t>(n) * 1000000 / rate;
            windows_[cursor_] = {start, end, rms};
            cursor_ = (cursor_ + 1) % windows_.size();
            start = end;
            pcm += n;
            count -= n;
        }
        next_us_ = start;
    }

    float Read(int64_t now_us) const {
        std::lock_guard lock(mutex_);
        for (const auto& w : windows_) {
            if (w.start <= now_us && now_us < w.end)
                return w.rms;
        }
        return 0;
    }

    void Reset(uint32_t generation) {
        std::lock_guard lock(mutex_);
        generation_ = generation;
        windows_ = {};
        next_us_ = 0;
        cursor_ = 0;
    }

private:
    struct Window {
        int64_t start, end;
        float rms;
    };
    mutable std::mutex mutex_;
    std::array<Window, 16> windows_{};
    size_t cursor_ = 0;
    int64_t next_us_ = 0;
    uint32_t generation_ = 0;
};
