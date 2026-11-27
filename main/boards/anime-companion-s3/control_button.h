#pragma once
#include <algorithm>
#include <cstdint>

namespace aura {
enum class Edge { None, Press, Repeat, Release };
class ControlButton {
    bool raw_ = false, down_ = false;
    int64_t changed_ = 0, next_ = 0;
public:
    Edge Sample(bool pressed, int64_t ms, bool repeat) {
        if (pressed != raw_) { raw_ = pressed; changed_ = ms; }
        if (raw_ != down_ && ms - changed_ >= 40) {
            down_ = raw_;
            next_ = ms + 500;
            return down_ ? Edge::Press : Edge::Release;
        }
        if (repeat && down_ && raw_ && ms >= next_) {
            next_ = ms + 180;
            return Edge::Repeat;
        }
        return Edge::None;
    }
};
inline int StepVolume(int level, int direction) { return std::clamp(level + 5 * direction, 0, 100); }
}
