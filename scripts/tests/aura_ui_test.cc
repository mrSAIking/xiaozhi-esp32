#include "aura_ui.h"
#include <cassert>
int main() {
    aura::Captions c;
    c.Submit(false, "hidden", 0);
    c.Tick(10, false);
    assert(c.text.empty());
    c.Enable(true);
    c.Submit(false, "Question?", 100);
    c.Tick(100, false);
    assert(c.text == "Question?" && c.Opacity(220) > 0 && c.Opacity(220) < 255);
    c.Submit(true, "First", 250);
    c.Tick(400, true);
    assert(c.text == "Question?");
    c.Tick(600, true);
    c.Tick(850, true);
    assert(c.text == "First");
    c.Tick(1150, true);
    c.Submit(true, "Second", 1150);
    c.Tick(1150, true);
    c.Tick(1400, true);
    assert(c.text == "Second");
    c.Tick(1500, false);
    c.Tick(2600, false);
    c.Tick(2850, false);
    assert(c.text.empty());
    c.Submit(true, "interrupt", 3000);
    c.Clear();
    assert(c.text.empty());
    c.Submit(true, "disable", 3100);
    c.Enable(false);
    assert(c.text.empty());
    assert(!aura::Listening(true, false, false) && !aura::Listening(true, true, true));
    assert(aura::Listening(true, true, false) && !aura::Listening(false, true, false));
    assert(aura::ScrollX(100, 0) == 102 && aura::ScrollX(500, 1500) < 0);
    for (bool speaking : {false, true}) {
        int count = 0;
        aura::BodyBudget budget(10, speaking);
        while (budget.Available()) {
            budget.Charge(320, 500);
            ++count;
        }
        assert(count > 0 && count <= 32);
    }
}
