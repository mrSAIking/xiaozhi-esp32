#include <fstream>
#include "music_disc.h"
int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    std::ofstream out(argv[1], std::ios::binary);
    for (int y = 0; y < 208; y++)
        for (int x = 0; x < 208; x++) {
            auto p = music::DiscPixel(x, y);
            out.put(char(p));
            out.put(char(p >> 8));
        }
    return out ? 0 : 1;
}
