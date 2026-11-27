#include <cassert>
#include "control_button.h"
using namespace aura;
int main() {
    ControlButton volume, settings;
    int level = 90, saves = 0, opens = 0;
    for (int t = 0; t <= 1700; t += 10) {
        const bool active = t == 10 || (t >= 40 && t < 1300);
        auto edge = volume.Sample(active, t, true);
        if (edge == Edge::Press || edge == Edge::Repeat) {
            assert(t == 80 || t == 580 || (t > 580 && (t-580)%180==0));
            level = StepVolume(level, 1);
        }
        if (edge == Edge::Release) ++saves;
        if (settings.Sample(active,t,false) == Edge::Press) ++opens;
    }
    assert(saves==1 && opens==1 && level==100 && StepVolume(2,-1)==0);
}
