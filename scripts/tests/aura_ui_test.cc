#include "aura_ui.h"
#include <cassert>
int main() {
    aura::Captions c;
    auto width = [](const char*, size_t bytes) { return int(bytes * 9); };
    auto tick = [&](int t, bool active) { c.Tick(t, active, width); };
    c.Enable(true);
    c.Submit(false, "Question?", 0);
    tick(15000, false);
    assert(c.text == "Question?" && c.X() > 10 && c.Opacity(15000) == 255);
    c.Submit(true, "First.", 15000);
    tick(15000, true);
    tick(15300, true);
    tick(16300, true);
    const int before = c.X();
    c.Submit(true, "Second.", 16300);
    c.Submit(true, "Second.", 16301);
    tick(16301, true);
    assert(c.X() <= before && c.X() >= before - 1 && c.text == "First. Second.");
    assert(c.Opacity(16301) == 255);
    c.Finish();
    int faded_frames = 0, previous = 255;
    for (int t = 16400; t < 40000; t += 40) {
        tick(t, false);
        int alpha = c.Opacity(t);
        assert(alpha <= previous);
        if (alpha > 0 && alpha < 255)
            ++faded_frames;
        previous = alpha;
    }
    assert(faded_frames >= 11 && faded_frames <= 13);
    assert(c.text.empty());
    // A large inter-packet gap can exhaust the visible text. The strip stays
    // opaque until more data arrives, without a second turn transition.
    c.Submit(true, "A", 40000);
    tick(40000, true);
    tick(48000, true);
    assert(c.text.empty() && c.Visible() && c.Opacity(48000) == 255);
    c.Submit(true, "B", 48020);
    tick(48020, true);
    assert(c.text == "B" && c.X() > 300);
    c.Clear();
    c.Submit(false, "New?", 49000);
    std::string long_text;
    for (int i = 0; i < 6000; i++)
        long_text += "界";
    c.Submit(true, long_text.c_str(), 49000);
    tick(49000, true);
    tick(49400, true);
    assert(c.Buffered() <= c.capacity && c.text.size() % 3 == 0);
    const size_t initial = c.Buffered();
    for (int t = 49440; t < 190000; t += 40) {
        tick(t, true);
        for (size_t i = 0; i < c.text.size(); i += 3)
            assert(c.text.compare(i, 3, "界") == 0);
        assert(c.text.size() % 3 == 0 && c.Buffered() <= c.capacity);
    }
    assert(c.Buffered() + 384 < initial);
    c.Clear();
    assert(c.text.empty() && !c.Buffered());
    c.Submit(false, "次の質問", 200000);
    c.Enable(false);
    assert(c.text.empty() && !c.Buffered());
    assert(!aura::Listening(true, false, false) && !aura::Listening(true, true, true));
    assert(aura::Listening(true, true, false) && !aura::Listening(false, true, false));
    assert(aura::ScrollX(100, 0) == 100 && aura::ScrollX(500, 1500) < 0);
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
