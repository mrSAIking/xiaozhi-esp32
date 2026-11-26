#include <cassert>
#include <vector>
#include "avatar_pose.h"
int main() {
    using namespace avatar;
    std::vector<uint16_t> image(320 * 480);
    image[120 * 320 + 160] = 0xffff;
    assert(SampleAt(image.data(), 160 * 256, 120 * 256) == 0xffff);
    assert(SampleAt(image.data(), 160 * 256 + 128, 120 * 256) == 0x8410);
    assert(SampleAt(image.data(), -200, -20) == 0);
    for (int mode : {0, 1}) {
        MotionConfig cfg{4800, 3, 5500, 6500, 3, 8200, 15};
        for (int t = 0; t < 18000; t += 40) {
            auto a = MakePose(t, mode, cfg), b = MakePose(t + 1, mode, cfg);
            for (int y : {20, 45, 60, 123, 166, 238, 312, 479}) {
                auto v = Offset(a, mode, 160, y), w = Offset(b, mode, 160, y);
                assert(std::abs(v.x - w.x) < 4 && std::abs(v.y - w.y) < 4);
                if (!mode && y > 280)
                    assert(v.x == 0 && v.y == 0);
            }
        }
        auto pose = MakePose(1830, mode, cfg);
        auto neutral = Pose{};
        std::vector<uint16_t> frame(320 * 480);
        Rect patch{150, mode ? 120 : 45, 18, 8};
        std::vector<uint16_t> mark(patch.w * patch.h, 0xffff);
        std::fill(image.begin(), image.end(), 0);
        Composite(image.data(), patch, mark.data());
        RenderRegion(image.data(), frame.data(), mode, pose, {0, 0, 320, 480});
        auto full = frame;
        RenderRegion(image.data(), frame.data(), mode, neutral, {0, 0, 320, 480});
        RenderRegion(image.data(), frame.data(), mode, pose, FaceBounds(patch));
        assert(frame == full && std::count(frame.begin(), frame.end(), 0xffff) > 40);
    }
}
