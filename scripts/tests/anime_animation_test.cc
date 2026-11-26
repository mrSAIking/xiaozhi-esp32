#include <array>
#include <cassert>
#include <cmath>
#include "avatar_math.h"
#include "playback_meter.h"

int main() {
    uint8_t rgb[3];
    avatar::Expand565(0xffff, rgb);
    assert(rgb[0] == 252 && rgb[1] == 252 && rgb[2] == 252);
    avatar::Expand565(0xf800, rgb);
    assert(rgb[0] == 252 && rgb[1] == 0 && rgb[2] == 0);
    avatar::Expand565(0x07e0, rgb);
    assert(rgb[0] == 0 && rgb[1] == 252 && rgb[2] == 0);
    avatar::Expand565(0x001f, rgb);
    assert(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 252);
    avatar::Expand565(0x0000, rgb);
    assert(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0);
    assert(avatar::FitsRgb565Buffer(320, 8, 640, 5120));
    assert(avatar::FitsRgb565Buffer(37, 13, 76, 986));
    assert(avatar::FitsRgb565Buffer(320, 8, 644, 5152));
    assert(!avatar::FitsRgb565Buffer(320, 8, 640, 5119));
    assert(!avatar::FitsRgb565Buffer(37, 13, 74, 960));
    assert(!avatar::FitsRgb565Buffer(37, 13, 75, 1000));
    assert(!avatar::FitsRgb565Buffer(320, 0, 640, 0));
    const int holds[] = {0, 50, 60, 60, 0};
    assert(avatar::EyeFrame(0, holds) == 1 && avatar::EyeFrame(49, holds) == 1);
    assert(avatar::EyeFrame(50, holds) == 2 && avatar::EyeFrame(109, holds) == 2);
    assert(avatar::EyeFrame(110, holds) == 1 && avatar::EyeFrame(170, holds) == 0);
    assert(avatar::Displacement(0, 0.5f, 1) == 0);
    assert(avatar::Displacement(0.5f, 1, 1) == 0);
    assert(avatar::Displacement(0.5f, 0.5f, 1) == 2);
    const float thresholds[] = {.015f, .065f, .15f};
    assert(avatar::MouthFrame(.014f, thresholds) == 0);
    assert(avatar::MouthFrame(.015f, thresholds) == 1);
    assert(avatar::MouthFrame(.065f, thresholds) == 2);
    assert(avatar::MouthFrame(.15f, thresholds) == 3);

    PlaybackMeter meter;
    std::array<int16_t, 2400> pcm{};
    std::fill_n(pcm.begin(), 1200, -16384);
    meter.Submit(pcm.data(), pcm.size(), 24000, 1000000, 50000, 0);
    assert(meter.Read(1049999) == 0);
    assert(std::abs(meter.Read(1050000) - .5f) < .0001f);
    assert(meter.Read(1100000) == 0 && meter.Read(1150000) == 0);
    meter.Reset(1);
    meter.Submit(pcm.data(), 1200, 24000, 0, 0, 0);
    assert(meter.Read(1050000) == 0 && meter.Read(1) == 0);
    meter.Submit(pcm.data(), 1200, 24000, 2000000, 0, 1);
    assert(meter.Read(2049999) == .5f && meter.Read(2050000) == 0);
}
