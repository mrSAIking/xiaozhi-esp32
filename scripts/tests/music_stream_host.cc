// Run the production ring, framing, downmix and lifetime logic with deterministic device adapters.
#include <cassert>
#include <fstream>
#include "music_stream.cc"
std::atomic<bool> cancelled{false};
unsigned buffers = 0;
void updateState(const char* state) {
    if (!strcmp(state, "Buffering"))
        ++buffers;
}
std::string readFixture(const std::string& name) {
    std::ifstream f(name, std::ios::binary);
    assert(f);
    return {std::istreambuf_iterator<char>(f), {}};
}
int main(int argc, char** argv) {
    assert(argc == 2);
    AudioService audio;
    for (const char* file : {"studio.mp3", "evening.mp3"}) {
        auto source = readFixture(std::string(argv[1]) + "/" + file);
        for (int repeat = 0; repeat < 3; repeat++) {
            content = source;
            code = 200;
            mime = "audio/mpeg";
            cut = false;
            audio.frames = 0;
            assert(!music::Stream("https://raw.githubusercontent.com/a/b/main/a.mp3", audio,
                                  cancelled, updateState));
            assert(audio.frames > 120 && allocations == 0);
        }
    }
    cut = true;
    assert(music::Stream("test", audio, cancelled, updateState));
    cut = false;
    code = 404;
    assert(music::Stream("test", audio, cancelled, updateState));
    code = 200;
    mime = "text/html";
    assert(music::Stream("test", audio, cancelled, updateState));
    mime = "audio/mpeg";
    content = "ID3xxxxxxxxxxxx";
    assert(music::Stream("test", audio, cancelled, updateState));
    content = readFixture(std::string(argv[1]) + "/evening.mp3");
    auto start = content.find_first_of(char(0xff), 10);
    assert(start != content.npos);
    auto payload = content.substr(start);
    for (int i = 0; i < 8; i++)
        content += payload;
    audio.frames = 0;
    audio.on_frame = []() { cancelled = true; };
    music::Stream("test", audio, cancelled, updateState);
    assert(audio.frames == 1 && allocations == 0);
}
