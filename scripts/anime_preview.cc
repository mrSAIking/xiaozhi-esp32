// Desktop driver for the same compositing, mesh, sampler and dirty regions used on the board.
#include <array>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "avatar_pose.h"
using namespace avatar;

std::vector<uint16_t> Read(const std::string& path, int pixels) {
    std::vector<uint16_t> data(pixels);
    std::ifstream stream(path, std::ios::binary);
    if (!stream.read(reinterpret_cast<char*>(data.data()), 2 * pixels))
        throw std::runtime_error("Cannot read " + path);
    return data;
}
int main(int argc, char** argv) {
    if (argc != 12)
        return 2;
    const std::string dir = argv[1];
    const int mode = std::stoi(argv[2]);
    const Rect eyes{std::stoi(argv[3]), std::stoi(argv[4]), std::stoi(argv[5]), std::stoi(argv[6])};
    const Rect mouth{std::stoi(argv[7]), std::stoi(argv[8]), std::stoi(argv[9]),
                     std::stoi(argv[10])};
    auto src = Read(dir + "/base.rgb565", 320 * 480), frame = src;
    std::ofstream output(argv[11], std::ios::binary);
    int64_t pixels = 0;
    for (int i = 0; i < 225; ++i) {
        int t = i * 40;
        const int holds[] = {0, 50, 60, 60, 0};
        const int eye = EyeFrame(t % 3200, holds);
        const char* eye_name = std::array{"open", "half", "closed"}[eye];
        const char* mouth_name = t % 3000 < 800 && i % 8 < 4 ? "medium" : "closed";
        Composite(src.data(), eyes,
                  Read(dir + "/eyes/eyes_" + eye_name + ".rgb565", eyes.w * eyes.h).data());
        Composite(src.data(), mouth,
                  Read(dir + "/mouth/mouth_" + mouth_name + ".rgb565", mouth.w * mouth.h).data());
        auto pose = MakePose(t, mode, {4800, 3, 5500, 6500, mode ? 3 : 2, 8200, 15});
        PoseRegions(mode, [&](Rect r) {
            auto dirty = RenderRegion(src.data(), frame.data(), mode, pose, r);
            if (dirty.w > 0)
                pixels += dirty.w * dirty.h;
        });
        if (i % 50 == 0) {
            auto reference = src;
            RenderRegion(src.data(), reference.data(), mode, pose, {0, 0, 320, 480});
            if (reference != frame)
                throw std::runtime_error("Dirty region coverage mismatch");
        }
        output.write(reinterpret_cast<const char*>(frame.data()), frame.size() * 2);
    }
    std::cout << "Mean dirty pixels/pose: " << pixels / 225 << "\n";
    return output ? 0 : 1;
}
