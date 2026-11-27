// Desktop driver for the same compositing, mesh, sampler and dirty regions used on the board.
#include <array>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "aura_ui.h"
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
    if (argc == 3 && std::string(argv[1]) == "--track") {
        const double advance = std::stod(argv[2]);  // PIL preview font, monospaced Latin fixture
        aura::Captions track;
        track.Enable(true);
        track.Submit(false, "Why is the sky blue?", 0);
        for (int ms = 0; ms < 45000; ms += 80) {
            if (ms == 1600)
                track.Submit(true, "Sunlight scatters.", ms);
            if (ms == 3600)
                track.Submit(true, "Blue scatters more.", ms);
            if (ms == 6000)
                track.Submit(true, "That colors our sky.", ms);
            if (ms == 8400)
                track.Finish();
            track.Tick(ms, ms >= 1600 && ms < 8800,
                       [advance](const char*, size_t n) { return int(n * advance + 0.5); });
            std::cout << ms << '\t' << track.X() << '\t' << track.StripOpacity(ms) << '\t'
                      << track.Opacity(ms) << '\t' << track.assistant << '\t' << track.text << '\n';
            if (ms > 9000 && track.text.empty())
                return 0;
        }
        throw std::runtime_error("Subtitle track did not drain");
    }
    if (argc != 12 && argc != 13)
        return 2;
    const std::string dir = argv[1];
    const int mode = std::stoi(argv[2]);
    const Rect eyes{std::stoi(argv[3]), std::stoi(argv[4]), std::stoi(argv[5]), std::stoi(argv[6])};
    const Rect mouth{std::stoi(argv[7]), std::stoi(argv[8]), std::stoi(argv[9]),
                     std::stoi(argv[10])};
    auto src = Read(dir + "/base.rgb565", 320 * 480), frame = src;
    std::ofstream output(argv[11], std::ios::binary);
    const std::string scenario = argc == 13 ? argv[12] : "default";
    AnimationSettings settings;
    settings.breath_on = scenario != "breathing-disabled";
    settings.hand_on = scenario != "hand-disabled";
    if (scenario == "fast-breathing")
        settings.breath_ds = 20;
    if (scenario == "slow-breathing")
        settings.breath_ds = 80;
    LiveMotion motion;
    int64_t pixels = 0;
    for (int i = 0; i < 225; ++i) {
        int t = i * 40;
        const int holds[] = {0, 50, 60, 60, 0};
        const int eye = EyeFrame(t % 3200, holds);
        const char* eye_name = std::array{"open", "half", "closed"}[eye];
        const char* mouth_name = t % 3000 < 800 && i % 8 < 4 ? "medium" : "closed";
        Composite(frame.data(), eyes,
                  Read(dir + "/eyes/eyes_" + eye_name + ".rgb565", eyes.w * eyes.h).data());
        Composite(frame.data(), mouth,
                  Read(dir + "/mouth/mouth_" + mouth_name + ".rgb565", mouth.w * mouth.h).data());
        if (scenario == "runtime-speed-change" && t == 4000) {
            settings.breath_ds = 20;
            settings.hand_ds = 75;
        }
        auto pose = motion.Step(t, settings, 3, mode ? 3 : 2);
        PoseRegions(mode, [&](Rect r) {
            auto dirty = RenderRegion(src.data(), frame.data(), mode, pose, r);
            if (dirty.w > 0)
                pixels += dirty.w * dirty.h;
        });
        if (i % 50 == 0) {
            auto reference = src;
            RenderRegion(src.data(), reference.data(), mode, pose, {0, 0, 320, 480});
            Composite(reference.data(), eyes,
                      Read(dir + "/eyes/eyes_" + eye_name + ".rgb565", eyes.w * eyes.h).data());
            Composite(
                reference.data(), mouth,
                Read(dir + "/mouth/mouth_" + mouth_name + ".rgb565", mouth.w * mouth.h).data());
            if (reference != frame)
                throw std::runtime_error("Dirty region coverage mismatch");
        }
        output.write(reinterpret_cast<const char*>(frame.data()), frame.size() * 2);
    }
    std::cout << "Mean dirty pixels/pose: " << pixels / 225 << "\n";
    return output ? 0 : 1;
}
