#include "reference_history.h"
#include <cassert>
#include <vector>

int main() {
    ReferenceHistory h;
    int32_t ramp[] = {0, 2 << 16, 4 << 16, 6 << 16};
    h.append(100, ramp, 4);
    assert(h.at(99) == 0 && h.at(104) == 0);
    assert(h.mono16(200) == 0 && h.mono16(203) == 3 && h.mono16(206) == 6);
    h.append(108, ramp, 4);
    assert(h.at(102) == 0 && h.at(107) == 0 && h.at(110) == 4);
    std::vector<int32_t> loud(h.capacity, -65536);
    h.append(112, loud.data(), loud.size());
    assert(h.at(111) == 0 && h.at(112) == -1 && h.at(h.end - 1) == -1);
    assert(h.at(h.end) == 0);
}
