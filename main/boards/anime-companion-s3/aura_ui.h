#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace aura {
inline bool Listening(bool listening, bool processor, bool awaiting_drain) {
    return listening && processor && !awaiting_drain;
}
inline int ScrollX(int width, int64_t elapsed) {
    constexpr int viewport = 300;
    if (width <= viewport)
        return (viewport - width) / 2;
    const int64_t travel = (width - viewport) * 1000LL / 35;
    const int64_t t = elapsed % (2 * travel + 1400);
    return -int(
        std::clamp<int64_t>(t <= travel + 700 ? t - 700 : 2 * travel + 1400 - t, 0, travel) * 35 /
        1000);
}
class BodyBudget {
public:
    BodyBudget(int mhz, bool speaking) : rate_(mhz), remaining_(speaking ? 9000 : 18000) {}
    bool Available() const { return remaining_ > 0; }
    void Charge(int pixels, int cpu_us) { remaining_ -= 24 * pixels / rate_ + std::max(cpu_us, 1); }

private:
    int rate_, remaining_;
};

// The label holds a rolling UTF-8 window. Pending bytes have never been drawn;
// bytes leave the front only after their measured glyphs have left the strip.
class Captions {
public:
    static constexpr size_t capacity = 8192;
    std::string text;
    unsigned revision = 0;
    bool assistant = false;
    void Enable(bool value) {
        if (enabled_ != value) {
            enabled_ = value;
            Clear();
        }
    }
    size_t Buffered() const { return text.size() + pending_.size(); }
    void Clear() {
        text.clear();
        pending_.clear();
        assistant = ended_ = false;
        last_hash_ = 0;
        fade_ = -1;
        ++revision;
    }
    void Finish() {
        if (assistant || !pending_.empty())
            ended_ = true;
    }
    void Submit(bool answer, const char* value, int64_t now) {
        if (!enabled_ || !value || !*value)
            return;
        if (!answer) {
            Clear();
            shown_ = now;
            Append(text, value, 1024);
            ++revision;
            return;
        }
        uint64_t hash = 14695981039346656037ULL;
        for (auto p = value; *p; ++p)
            hash = (hash ^ uint8_t(*p)) * 1099511628211ULL;
        if (last_hash_ == hash)
            return;
        last_hash_ = hash;
        auto space = capacity - Buffered();
        if (space < 5)
            return;
        if (assistant || !pending_.empty()) {
            pending_ += ' ';
            --space;
        }
        Append(pending_, value, space);
    }
    template <class Measure>
    void Tick(int64_t now, bool playing, Measure width) {
        if (!assistant) {
            if (pending_.empty() || (!playing && !ended_)) {
                MeasureWindow(width);
                x_ = 10 + ScrollX(width_, now - shown_);
                return;
            }
            if (!text.empty()) {
                if (fade_ < 0) {
                    fade_alpha_ = Opacity(now);
                    fade_ = now;
                }
                if (now - fade_ < 240)
                    return;
            }
            text.clear();
            ++revision;
            assistant = true;
            fade_ = -1;
            shown_ = last_ = now;
            pos_ = 310000;
        }
        if (text.size() < window && !pending_.empty()) {
            size_t n = Prefix(pending_, window - text.size());
            if (n) {
                text.append(pending_, 0, n);
                pending_.erase(0, n);
                ++revision;
            }
        }
        MeasureWindow(width);
        const int speed = Buffered() > 800 ? 50 : 45;
        pos_ -= (now - last_) * speed;
        last_ = now;
        // Keep the tail readable if playback catches up with sentence delivery.
        if ((!ended_ || playing) && pending_.empty())
            pos_ = std::max(pos_, int64_t(std::min(10, 280 - width_)) * 1000);
        x_ = int(pos_ / 1000);
        if (text.size() > 128 && x_ < 0) {
            const size_t n = Prefix(text, 96);
            if (x_ + prefix_width_ < 0) {
                int old = width_;
                text.erase(0, n);
                ++revision;
                MeasureWindow(width);
                pos_ += int64_t(old - width_) * 1000;
                x_ = int(pos_ / 1000);
            }
        }
        if (ended_ && !playing && pending_.empty() && x_ + width_ <= 0) {
            if (fade_ < 0) {
                fade_ = now;
                fade_alpha_ = 255;
            }
            if (now - fade_ >= 500)
                Clear();
        }
    }
    int X() const { return x_; }
    void FontChanged() { ++revision; }
    int Opacity(int64_t now) const {
        if (text.empty())
            return 0;
        if (fade_ >= 0) {
            const int duration = assistant ? 500 : 240;
            return int(std::max<int64_t>(0, duration - (now - fade_)) * fade_alpha_ / duration);
        }
        return assistant ? 255 : int(std::clamp<int64_t>(now - shown_, 0, 240) * 255 / 240);
    }

private:
    static constexpr size_t window = 384;
    static size_t Prefix(const std::string& s, size_t n) {
        if (n >= s.size())
            return s.size();
        while (n && (uint8_t(s[n]) & 0xc0) == 0x80)
            --n;
        return n;
    }
    static void Append(std::string& dest, const char* src, size_t room) {
        size_t n = 0;
        while (n < room && src[n])
            ++n;
        while (n && (uint8_t(src[n]) & 0xc0) == 0x80)
            --n;
        auto start = dest.size();
        dest.append(src, n);
        for (size_t i = start; i < dest.size(); ++i)
            if (dest[i] == '\n' || dest[i] == '\r' || dest[i] == '\t')
                dest[i] = ' ';
    }
    template <class Measure>
    void MeasureWindow(Measure width) {
        if (measured_ != revision) {
            width_ = width(text.data(), text.size());
            prefix_width_ = text.size() > 128 ? width(text.data(), Prefix(text, 96)) : 0;
            measured_ = revision;
        }
    }
    std::string pending_;
    bool enabled_ = false, ended_ = false;
    unsigned measured_ = ~0u;
    uint64_t last_hash_ = 0;
    int width_ = 0, prefix_width_ = 0, x_ = 0, fade_alpha_ = 255;
    int64_t pos_ = 0, shown_ = 0, last_ = 0, fade_ = -1;
};
}  // namespace aura
