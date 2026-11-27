#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace music {
inline uint16_t DiscPixel(int x, int y) {
    float xx = x - 103.5f, yy = y - 103.5f, r = std::sqrt(xx * xx + yy * yy);
    auto rgb = [](int r, int g, int b) {
        return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    };
    if (r > 103.5f || r < 5)
        return rgb(9, 9, 14);
    if (r < 7 || r > 101)
        return rgb(212, 208, 228);
    if (r < 29)
        return rgb(114, 59, 168);
    if (r < 31 || r > 98)
        return rgb(82, 77, 101);
    float f = (xx * .91f + yy * .414f) / r;
    float glint = std::abs(f * f * 2 - 1);
    int v = 97 + int(115 * glint) + int(r) % 4 * 3;
    return rgb(v, std::max(0, v - int(15 * std::abs(yy / r))), std::min(255, v + 18));
}
}  // namespace music
