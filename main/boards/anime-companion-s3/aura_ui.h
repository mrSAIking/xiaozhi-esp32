#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace aura {
inline bool Listening(bool listening, bool processor, bool awaiting_drain) {
    return listening && processor && !awaiting_drain;
}
inline int ScrollX(int width, int64_t elapsed) {
    constexpr int viewport = 304;
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

// One visible line plus the latest incoming sentence. Never build a backlog behind playback.
class Captions {
public:
    std::string text;
    bool assistant = false;
    int64_t shown = 0;
    unsigned revision = 0;
    void Clear() {
        text.clear();
        pending_.clear();
        fading_ = -1;
        end_ = -1;
        ++revision;
    }
    void Enable(bool value) {
        if (value != enabled_) {
            enabled_ = value;
            Clear();
        }
    }
    void Submit(bool answer, const char* value, int64_t now) {
        if (!enabled_ || !value || !*value)
            return;
        pending_.assign(value);
        std::replace(pending_.begin(), pending_.end(), '\n', ' ');
        std::replace(pending_.begin(), pending_.end(), '\r', ' ');
        next_answer_ = answer;
        if (text.empty() || !answer)
            Present(now);
    }
    void Tick(int64_t now, bool audio_active) {
        if (text.empty())
            return;
        if (!assistant || audio_active)
            end_ = -1;
        else if (end_ < 0)
            end_ = now + 1000;
        const bool replace = !pending_.empty() && now - shown >= (assistant ? 120 : 450);
        if (fading_ < 0 &&
            (replace || (end_ >= 0 && now >= end_) || (!assistant && now - shown >= 12000))) {
            fade_from_ = Opacity(now);
            fading_ = now;
        }
        if (fading_ >= 0 && now - fading_ >= 240) {
            if (!pending_.empty())
                Present(now);
            else
                Clear();
        }
    }
    int Opacity(int64_t now) const {
        if (text.empty())
            return 0;
        if (fading_ >= 0)
            return int(std::max<int64_t>(0, 240 - (now - fading_)) * fade_from_ / 240);
        return int(std::clamp<int64_t>(now - shown, 0, 240) * 255 / 240);
    }

private:
    void Present(int64_t now) {
        text.swap(pending_);
        pending_.clear();
        assistant = next_answer_;
        shown = now;
        fading_ = end_ = -1;
        ++revision;
    }
    bool enabled_ = false, next_answer_ = false;
    int fade_from_ = 0;
    int64_t fading_ = -1, end_ = -1;
    std::string pending_;
};
}  // namespace aura
